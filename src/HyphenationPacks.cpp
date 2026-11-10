#include "HyphenationPacks.h"

#include <Arduino.h>
#include <Epub/hyphenation/HyphenationPack.h>
#include <Epub/hyphenation/HyphenationStore.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/hyphenation/LanguageRegistry.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_rom_crc.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>

#include "CrossPointSettings.h"
#include "CrossPointState.h"

#if CONFIG_SPIRAM
#include <esp_heap_caps.h>
#else
#include <EspNorFlash.h>
#include <FlashFontPartition.h>
#endif

namespace HyphenationPacks {

namespace {

class SdPackSource final : public HyphenationPackSource {
 public:
  bool open(const char* path) {
    if (!Storage.openFileForRead("HYP", path, file_)) return false;
    size_ = static_cast<uint32_t>(file_.fileSize());
    return true;
  }
  // Only a file it opened: a language whose pack neither folder holds never opens one, and HalFile::close() asserts
  // on a file that was never opened.
  ~SdPackSource() {
    if (file_.isOpen()) file_.close();
  }
  uint32_t size() const override { return size_; }
  bool read(const uint32_t offset, void* dst, const uint32_t len) override {
    return file_.seekSet(offset) && file_.read(dst, len) == static_cast<int>(len);
  }

 private:
  HalFile file_;
  uint32_t size_ = 0;
};

// Where the card holds the pack of `code`: our folder first, then CrossInk's. False when neither has it.
bool findPack(const char* code, char (&out)[48]) {
  for (const char* dir : {DIR, CROSSINK_DIR}) {
    snprintf(out, sizeof(out), "%s/hyph-%s.cphyph", dir, code);
    if (Storage.exists(out)) return true;
  }
  return false;
}

// The languages whose pack the card was last seen without (a bit per registry entry): the provider hands out none of
// their patterns, so a deleted pack stops hyphenating its books even while the flash store still holds the trie.
uint32_t missingOnCard = 0;
static_assert(sizeof(missingOnCard) * 8 >= 32, "a bit per language");

uint32_t bitOf(const LanguageEntry* entry) {
  const LanguageEntryView all = getLanguageEntries();
  const auto index = static_cast<size_t>(entry - all.data);
  return index < all.size && index < 32 ? (1u << index) : 0;
}

void markOnCard(const char* code, const bool onCard) {
  const uint32_t bit = bitOf(findLanguageEntry(code));
  missingOnCard = onCard ? (missingOnCard & ~bit) : (missingOnCard | bit);
}

[[maybe_unused]] bool knownMissing(const char* code) { return (missingOnCard & bitOf(findLanguageEntry(code))) != 0; }

// Opens the card's pack of `code` and reads its header. False, quietly, when the card has none.
bool openPack(const char* code, SdPackSource& src, hyphpack::Header& header) {
  char path[48];
  if (!findPack(code, path)) return false;
  if (!src.open(path)) return false;
  if (!readPackHeader(src, header)) {
    LOG_ERR("HYP", "%s is not a hyphenation pack this firmware reads", path);
    return false;
  }
  // The pack names its language and the minima it was made for; the registry's table decides how the language is
  // hyphenated, and a pack that disagrees with it is not used.
  const LanguageEntry* entry = findLanguageEntry(code);
  if (!entry || header.code[0] != code[0] || header.code[1] != code[1] || header.minPrefix != entry->minPrefix ||
      header.minSuffix != entry->minSuffix) {
    LOG_ERR("HYP", "%s: language %s, minima %u/%u do not match the firmware's", path, header.code, header.minPrefix,
            header.minSuffix);
    return false;
  }
  return true;
}

#if CONFIG_SPIRAM

// The open book's trie, whole, in PSRAM: one language at a time.
struct Loaded {
  char code[3] = {};
  uint8_t* data = nullptr;
  uint32_t size = 0;
  uint32_t rootOffset = 0;
  uint32_t crc32 = 0;
};
Loaded loaded;
constexpr uint32_t kBounceBytes = 4096;

bool isLoaded(const char* code) { return loaded.data && loaded.code[0] == code[0] && loaded.code[1] == code[1]; }

bool provide(const char* code, SerializedHyphenationPatterns& out, uint32_t& crc32) {
  if (!isLoaded(code)) return false;
  out = {loaded.rootOffset, loaded.data, loaded.size};
  crc32 = loaded.crc32;
  return true;
}

void release() {
  forgetPackPatterns();
  if (loaded.data) heap_caps_free(loaded.data);
  loaded = Loaded{};
}

void load(const char* code) {
  SdPackSource src;
  hyphpack::Header header;
  const bool onCard = openPack(code, src, header);
  markOnCard(code, onCard);
  if (!onCard) {
    release();
    LOG_INF("HYP", "%s: no hyphenation pack on the card", code);
    return;
  }
  // The trie loaded for the previous book in this language stays, unless the card's pack changed since.
  if (isLoaded(code) && loaded.crc32 == header.crc32 && loaded.size == header.payloadSize &&
      loaded.rootOffset == header.rootOffset) {
    return;
  }
  release();
  const uint32_t start = millis();
  auto* data = static_cast<uint8_t*>(heap_caps_malloc(header.payloadSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  // The card is read into internal RAM and copied on, whatever its driver does with a buffer in external RAM.
  auto bounce = static_cast<uint8_t*>(heap_caps_malloc(kBounceBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!data || !bounce) {
    LOG_ERR("HYP", "%s: no PSRAM for %u B", code, static_cast<unsigned>(header.payloadSize));
    heap_caps_free(data);
    heap_caps_free(bounce);
    return;
  }
  bool ok = true;
  for (uint32_t done = 0; ok && done < header.payloadSize;) {
    const uint32_t len = std::min(kBounceBytes, header.payloadSize - done);
    ok = src.read(hyphpack::kHeaderBytes + done, bounce, len);
    if (ok) memcpy(data + done, bounce, len);
    done += len;
  }
  heap_caps_free(bounce);
  if (!ok || esp_rom_crc32_le(0, data, header.payloadSize) != header.crc32) {
    LOG_ERR("HYP", "%s: the pack could not be read, or fails its CRC", code);
    heap_caps_free(data);
    return;
  }
  loaded.code[0] = code[0];
  loaded.code[1] = code[1];
  loaded.data = data;
  loaded.size = header.payloadSize;
  loaded.rootOffset = header.rootOffset;
  loaded.crc32 = header.crc32;
  LOG_INF("HYP", "%s: %u B loaded into PSRAM in %lu ms", code, static_cast<unsigned>(header.payloadSize),
          static_cast<unsigned long>(millis() - start));
}

#else

EspNorFlash flash;
HyphenationStore store(flash);
bool mounted = false;

// The copy buffer of a fill. A program call costs ~0.1 ms on top of its bytes on the X3 (the glyph store's
// measurements), so 1 KB pieces add ~17 ms to German's copy over 4 KB ones, and hold 3 KB less while a book opens.
constexpr uint32_t kScratchBytes = 1024;

bool provide(const char* code, SerializedHyphenationPatterns& out, uint32_t& crc32) {
  return mounted && !knownMissing(code) && store.map(code, out, &crc32);
}

const char* fillName(const HyphenationStore::Fill result) {
  switch (result) {
    case HyphenationStore::Fill::Stored:
      return "stored";
    case HyphenationStore::Fill::Unchanged:
      return "unchanged";
    case HyphenationStore::Fill::WouldDrop:
      return "no room beside the others, left for its first book";
    case HyphenationStore::Fill::NoSpace:
    case HyphenationStore::Fill::BadSource:
    case HyphenationStore::Fill::FlashFailed:
      break;
  }
  return "FAILED";
}

// Copies the card's pack into the store when the store lacks it or holds other bytes for the language; `beforeCopy`,
// when given, runs before the copy starts. With `mayDrop` false no other language leaves the store for it.
void ensureStored(const char* code, const bool mayDrop, const std::function<void()>& beforeCopy) {
  SdPackSource src;
  hyphpack::Header header;
  const bool onCard = openPack(code, src, header);
  markOnCard(code, onCard);
  if (!onCard) {
    LOG_INF("HYP", "%s: no hyphenation pack on the card", code);
    return;
  }
  HyphenationStore::Entry stored;
  if (store.find(code, stored) && stored.crc32 == header.crc32 && stored.size == header.payloadSize &&
      stored.rootOffset == header.rootOffset) {
    return;
  }
  auto scratch = makeUniqueNoThrow<uint8_t[]>(kScratchBytes);
  if (!scratch) {
    LOG_ERR("HYP", "%s: no %u B for the copy", code, static_cast<unsigned>(kScratchBytes));
    return;
  }
  if (beforeCopy) beforeCopy();
  // fill() unmaps whatever the store handed out: no hyphenator may still point into it.
  forgetPackPatterns();
  const uint32_t start = millis();
  const HyphenationStore::Fill result = store.fill(src, header, scratch.get(), kScratchBytes, mayDrop);
  LOG_INF("HYP", "%s: %u B into the flash store: %s in %lu ms", code, static_cast<unsigned>(header.payloadSize),
          fillName(result), static_cast<unsigned long>(millis() - start));
}

#endif

}  // namespace

void begin() {
#if !CONFIG_SPIRAM
  const size_t base = FlashFontPartition::hyphRegionOffset();
  mounted = base != 0 && flash.beginRegion(static_cast<uint32_t>(base), FlashFontPartition::HYPH_RESERVED_BYTES) &&
            store.mount();
  if (!mounted) LOG_ERR("HYP", "no hyphenation store: books in languages other than English are not hyphenated");
#endif
  setHyphenationPatternProvider(provide);
}

void prepareForBook(const std::string& languageTag, const std::function<void()>& beforeCopy) {
  const char* code = Hyphenator::languageCode(languageTag);
  const LanguageEntry* entry = code ? findLanguageEntry(code) : nullptr;
#if CONFIG_SPIRAM
  (void)beforeCopy;  // a load from the card into PSRAM takes no copy worth a word
  if (!entry || entry->builtIn) {
    release();
    return;
  }
  // The registry asks again for every language: one whose pack was missing for the last book may be there now.
  forgetPackPatterns();
  load(code);
#else
  // Every language's mapping goes, while no build holds its patterns: only the open book's is mapped again (on its
  // first lookup), and the MMU pages of the others return to the font cache.
  forgetPackPatterns();
  store.unmapAll();
  if (!mounted || !entry || entry->builtIn) return;
  ensureStored(code, /*mayDrop=*/true, beforeCopy);
#endif
}

bool codeOfPackFile(const char* fileName, char (&code)[3]) {
  // "hyph-" + two lowercase letters + ".cphyph"
  if (!fileName || strlen(fileName) != 14 || strncmp(fileName, "hyph-", 5) != 0 || strcmp(fileName + 7, ".cphyph") != 0)
    return false;
  if (fileName[5] < 'a' || fileName[5] > 'z' || fileName[6] < 'a' || fileName[6] > 'z') return false;
  code[0] = fileName[5];
  code[1] = fileName[6];
  code[2] = '\0';
  return true;
}

void packInstalled(const char* code) {
  const LanguageEntry* entry = findLanguageEntry(code);
  if (!entry || entry->builtIn) return;
  markOnCard(code, true);
  forgetPackPatterns();
#if CONFIG_SPIRAM
  // The next book in this language reads the new pack.
  if (isLoaded(code)) release();
#else
  if (mounted) ensureStored(code, /*mayDrop=*/false, nullptr);
#endif
}

bool packOnCard(const char* code) {
  char path[48];
  return code && findPack(code, path);
}

namespace {

// The hyphenation language of each interface language whose patterns come from a pack. Hungarian has no pack yet;
// Romanian and Kazakh have no patterns, and English, Chinese, Japanese, Korean and Vietnamese no hyphenation.
struct UiPack {
  const char* uiCode;  // the translation's _language_code
  const char* code;
};
constexpr UiPack kUiPacks[] = {{"DE", "de"}, {"FR", "fr"}, {"ES", "es"}, {"IT", "it"}, {"PT", "pt"},
                               {"PO", "pt"}, {"PL", "pl"}, {"RU", "ru"}, {"UK", "uk"}, {"SV", "sv"},
                               {"NL", "nl"}, {"CS", "cs"}, {"DA", "da"}, {"SI", "sl"}, {"LT", "lt"},
                               {"CA", "ca"}, {"BE", "be"}, {"FI", "fi"}, {"TR", "tr"}};

// Each pack language's name for the offer. The interface fonts draw Latin and Cyrillic, not the Indic scripts.
struct Name {
  const char* code;
  const char* name;
};
constexpr Name kNames[] = {
    {"de", "Deutsch"},    {"fr", "Français"}, {"es", "Español"},     {"it", "Italiano"},  {"pt", "Português"},
    {"pl", "Polski"},     {"ru", "Русский"},  {"uk", "Українська"},  {"sv", "Svenska"},   {"nl", "Nederlands"},
    {"cs", "Čeština"},    {"da", "Dansk"},    {"sl", "Slovenščina"}, {"lt", "Lietuvių"},  {"ca", "Català"},
    {"be", "Беларуская"}, {"fi", "Suomi"},    {"tr", "Türkçe"},      {"as", "Assamese"},  {"bn", "Bengali"},
    {"gu", "Gujarati"},   {"hi", "Hindi"},    {"kn", "Kannada"},     {"ml", "Malayalam"}, {"mr", "Marathi"},
    {"or", "Odia"},       {"pa", "Punjabi"},  {"sa", "Sanskrit"},    {"ta", "Tamil"},     {"te", "Telugu"}};

bool offerTaken = false;

bool declined(const char* code) {
  const std::string& list = APP_STATE.hyphenationOffersDeclined;
  for (size_t i = 0; i + 1 < list.size(); i += 2) {
    if (list[i] == code[0] && list[i + 1] == code[1]) return true;
  }
  return false;
}

// Whether a pack of `code` is worth offering: the firmware hyphenates the language from a pack, the card has none,
// and the offer was not declined.
bool offerable(const char* code) {
  const LanguageEntry* entry = code ? findLanguageEntry(code) : nullptr;
  return entry && !entry->builtIn && !declined(code) && !packOnCard(code);
}

}  // namespace

bool takeOffer(char (&code)[3]) {
  if (offerTaken) return false;
  offerTaken = true;
  if (!SETTINGS.hyphenationEnabled) return false;
  const char* uiCode = I18N.getLanguageCode(I18N.getLanguage());
  const char* pack = nullptr;
  for (const auto& entry : kUiPacks) {
    if (uiCode && std::strcmp(uiCode, entry.uiCode) == 0) pack = entry.code;
  }
  if (!offerable(pack)) return false;
  snprintf(code, sizeof(code), "%s", pack);
  LOG_INF("HYP", "%s: no hyphenation pack on the card for the interface language, offering it", pack);
  return true;
}

bool bookOfferDue(const std::string& languageTag, char (&code)[3]) {
  const char* pack = Hyphenator::languageCode(languageTag);
  if (!offerable(pack)) return false;
  snprintf(code, sizeof(code), "%s", pack);
  LOG_INF("HYP", "%s: no hyphenation pack on the card for the book's language, offering it", pack);
  return true;
}

void declineOffer(const char* code) {
  if (!code || std::strlen(code) != 2 || declined(code)) return;
  APP_STATE.hyphenationOffersDeclined.append(code, 2);
  APP_STATE.saveToFile();
}

const char* languageName(const char* code) {
  for (const auto& entry : kNames) {
    if (code && std::strcmp(code, entry.code) == 0) return entry.name;
  }
  return code ? code : "";
}

void packRemoved(const char* code) {
  const LanguageEntry* entry = findLanguageEntry(code);
  if (!entry || entry->builtIn) return;
  // Another copy may still be in CrossInk's folder: the next book open looks.
  markOnCard(code, false);
  forgetPackPatterns();
#if CONFIG_SPIRAM
  if (isLoaded(code)) release();
#endif
}

}  // namespace HyphenationPacks
