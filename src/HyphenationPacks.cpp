#include "HyphenationPacks.h"

#include <Arduino.h>
#include <Epub/hyphenation/HyphenationPack.h>
#include <Epub/hyphenation/HyphenationStore.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/hyphenation/LanguageRegistry.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_rom_crc.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

#include "CrossPointState.h"

#if CONFIG_SPIRAM
#include <esp_heap_caps.h>
#else
#include <Epub/hyphenation/PagedTrieReader.h>
#include <FlashAreaRegion.h>
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

FlashAreaRegion flash;
HyphenationStore store(flash);
bool mounted = false;

// The copy buffer of a fill. A program call costs ~0.1 ms on top of its bytes on the X3 (the glyph store's
// measurements), so 1 KB pieces add ~17 ms to German's copy over 4 KB ones, and hold 3 KB less while a book opens.
constexpr uint32_t kScratchBytes = 1024;

// A trie read in place from its pack on the card, a page at a time (PagedTrieReader), where the flash store cannot hold
// it: every pack language's when the store is not mounted (a partition table whose flash area has no room for the
// region: FlashFontPartition::hyphRegionOffset()), and a language's whose copy failed for a reason the next book open
// would meet again (copyFailed). Its pages and the open pack file are held while the language is hyphenated, and go
// with the patterns (unmapAll()).
//
// The payload's CRC is not checked here, where the store and the PSRAM load check it: that would be one read of the
// whole trie (German's: 206 KB) at the first book open after every wake, as a wake is a boot. The Font
// Manager checked the file against its manifest's CRC when it installed it, the header must describe exactly the
// file's size, and the walk checks every address, so a pack that went bad on the card since gives wrong breaks, never
// a read outside the trie.
struct CardTrie {
  explicit CardTrie(const char* code)
      : found(openPack(code, src, header)), reader(src, hyphpack::kHeaderBytes, found ? header.payloadSize : 0) {}
  SdPackSource src;
  hyphpack::Header header;
  bool found;
  PagedTrieReader reader;
};

// As many languages as the registry hyphenates at once: the open book's, and one more.
std::unique_ptr<CardTrie> cardTries[HyphenationStore::kMaxMapped];

// The languages read from the card although the store is mounted: their copy into it failed with NoSpace or
// FlashFailed (a bit per registry entry). Cleared when a copy succeeds.
uint32_t copyFailed = 0;

bool readsCard(const char* code) { return !mounted || (copyFailed & bitOf(findLanguageEntry(code))) != 0; }

void releaseCardTries() {
  for (auto& trie : cardTries) trie.reset();
}

// The trie of `code` on the card: held already, or opened into a free slot. Null when the card has no pack for it,
// there is no memory, or both slots hold other languages (until the next book open frees them).
CardTrie* cardTrie(const char* code) {
  std::unique_ptr<CardTrie>* slot = nullptr;
  for (auto& trie : cardTries) {
    if (trie && trie->header.code[0] == code[0] && trie->header.code[1] == code[1]) return trie.get();
    if (!trie && !slot) slot = &trie;
  }
  if (!slot) {
    LOG_ERR("HYP", "%s: two languages read from the card already", code);
    return nullptr;
  }
  auto trie = makeUniqueNoThrow<CardTrie>(code);
  if (!trie) {
    LOG_ERR("HYP", "%s: no %u B to read the trie from the card", code, static_cast<unsigned>(sizeof(CardTrie)));
    return nullptr;
  }
  markOnCard(code, trie->found);
  if (!trie->found) {
    LOG_INF("HYP", "%s: no hyphenation pack on the card", code);
    return nullptr;
  }
  LOG_INF("HYP", "%s: its %u B trie is read from the pack on the card, through %u pages of %u B", code,
          static_cast<unsigned>(trie->header.payloadSize), static_cast<unsigned>(PagedTrieReader::kPages),
          static_cast<unsigned>(PagedTrieReader::kPageBytes));
  *slot = std::move(trie);
  return slot->get();
}

bool provide(const char* code, SerializedHyphenationPatterns& out, uint32_t& crc32) {
  if (knownMissing(code)) return false;
  if (!readsCard(code)) return store.map(code, out, &crc32);
  CardTrie* trie = cardTrie(code);
  if (!trie) return false;
  out = SerializedHyphenationPatterns{trie->header.rootOffset, nullptr, trie->header.payloadSize, &trie->reader};
  crc32 = trie->header.crc32;
  return true;
}

// Every language's patterns taken back and unmapped, while no build holds them: their MMU pages return to the font
// cache, the tries read from the card close their files and free their pages, and the next lookup of a language
// maps or opens it again.
void unmapAll() {
  forgetPackPatterns();
  store.unmapAll();
  releaseCardTries();
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
  const uint32_t bit = bitOf(findLanguageEntry(code));
  HyphenationStore::Entry stored;
  if (store.find(code, stored) && stored.crc32 == header.crc32 && stored.size == header.payloadSize &&
      stored.rootOffset == header.rootOffset) {
    copyFailed &= ~bit;
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
  switch (result) {
    case HyphenationStore::Fill::Stored:
    case HyphenationStore::Fill::Unchanged:
    case HyphenationStore::Fill::BadSource:  // the pack fails its CRC: not hyphenated, from the card either
      copyFailed &= ~bit;
      break;
    case HyphenationStore::Fill::NoSpace:
    case HyphenationStore::Fill::FlashFailed:
      LOG_INF("HYP", "%s: its trie is read from the card instead", code);
      copyFailed |= bit;
      break;
    case HyphenationStore::Fill::WouldDrop:  // the next book open copies it, dropping another language if it must
      break;
  }
}

#endif

}  // namespace

void begin() {
#if !CONFIG_SPIRAM
  const size_t base = FlashFontPartition::hyphRegionOffset();
  mounted = base != 0 && flash.beginRegion(static_cast<uint32_t>(base), FlashFontPartition::HYPH_RESERVED_BYTES) &&
            store.mount();
  if (!mounted) LOG_ERR("HYP", "no hyphenation store: the tries of books in other languages are read from the card");
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
  // Every language's mapping goes: only the open book's is mapped again (on its first lookup).
  unmapAll();
  if (!entry || entry->builtIn) return;
  if (mounted) ensureStored(code, /*mayDrop=*/true, beforeCopy);
  // A trie read from the card is opened now, while the book opens, rather than at its first line-end word in a build.
  if (readsCard(code)) cardTrie(code);
#endif
}

void bookClosed() {
#if CONFIG_SPIRAM
  // The loaded trie stays: it lies in PSRAM, which is mapped whole at boot, so it holds no MMU page and no internal
  // RAM, and the next book in its language opens without reading the pack again (load()).
#else
  const uint32_t before = flash.freeMapPages();
  unmapAll();
  LOG_DBG("HYP", "book closed: tries unmapped or closed, MMU pages free %u -> %u", static_cast<unsigned>(before),
          static_cast<unsigned>(flash.freeMapPages()));
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
  // A trie read from the card holds the file the Font Manager just replaced.
  releaseCardTries();
  if (mounted) ensureStored(code, /*mayDrop=*/false, nullptr);
#endif
}

bool packOnCard(const char* code) {
  char path[48];
  return code && findPack(code, path);
}

namespace {

// Each pack language's name for the offer. The interface fonts draw Latin and Cyrillic, not the Indic scripts.
struct Name {
  const char* code;
  const char* name;
};
constexpr Name kNames[] = {{"de", "Deutsch"},     {"fr", "Français"},   {"es", "Español"},  {"it", "Italiano"},
                           {"pt", "Português"},   {"pl", "Polski"},     {"ru", "Русский"},  {"uk", "Українська"},
                           {"sv", "Svenska"},     {"nl", "Nederlands"}, {"cs", "Čeština"},  {"da", "Dansk"},
                           {"sl", "Slovenščina"}, {"lt", "Lietuvių"},   {"ca", "Català"},   {"be", "Беларуская"},
                           {"fi", "Suomi"},       {"tr", "Türkçe"},     {"as", "Assamese"}, {"bn", "Bengali"},
                           {"gu", "Gujarati"},    {"hi", "Hindi"},      {"kn", "Kannada"},  {"ml", "Malayalam"},
                           {"mr", "Marathi"},     {"or", "Odia"},       {"pa", "Punjabi"},  {"sa", "Sanskrit"},
                           {"ta", "Tamil"},       {"te", "Telugu"},     {"hu", "Magyar"}};

}  // namespace

bool declined(const char* code) {
  const std::string& list = APP_STATE.hyphenationOffersDeclined;
  for (size_t i = 0; i + 1 < list.size(); i += 2) {
    if (list[i] == code[0] && list[i + 1] == code[1]) return true;
  }
  return false;
}

bool offerDue(const char* code) {
  const LanguageEntry* entry = code ? findLanguageEntry(code) : nullptr;
  return entry && !entry->builtIn && !declined(code) && !packOnCard(code);
}

bool bookOfferDue(const std::string& languageTag, char (&code)[3]) {
  const char* pack = Hyphenator::languageCode(languageTag);
  if (!offerDue(pack)) return false;
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
#else
  releaseCardTries();
#endif
}

}  // namespace HyphenationPacks
