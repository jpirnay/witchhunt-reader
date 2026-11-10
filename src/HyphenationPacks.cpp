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
  ~SdPackSource() { file_.close(); }
  uint32_t size() const override { return size_; }
  bool read(const uint32_t offset, void* dst, const uint32_t len) override {
    return file_.seekSet(offset) && file_.read(dst, len) == static_cast<int>(len);
  }

 private:
  HalFile file_;
  uint32_t size_ = 0;
};

void packPath(const char* code, char (&out)[48]) { snprintf(out, sizeof(out), "%s/hyph-%s.cphyph", DIR, code); }

// Opens the card's pack of `code` and reads its header. False, quietly, when the card has none.
bool openPack(const char* code, SdPackSource& src, hyphpack::Header& header) {
  char path[48];
  packPath(code, path);
  if (!Storage.exists(path)) return false;
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

bool provide(const char* code, SerializedHyphenationPatterns& out, uint32_t& crc32) {
  if (!loaded.data || loaded.code[0] != code[0] || loaded.code[1] != code[1]) return false;
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
  if (loaded.data && loaded.code[0] == code[0] && loaded.code[1] == code[1]) return;
  release();
  SdPackSource src;
  hyphpack::Header header;
  if (!openPack(code, src, header)) {
    LOG_INF("HYP", "%s: no hyphenation pack on the card", code);
    return;
  }
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
  return mounted && store.map(code, out, &crc32);
}

// Copies the card's pack into the store when the store lacks it or holds other bytes for the language.
void ensureStored(const char* code) {
  HyphenationStore::Entry stored;
  const bool has = store.find(code, stored);
  SdPackSource src;
  hyphpack::Header header;
  if (!openPack(code, src, header)) {
    if (!has) LOG_INF("HYP", "%s: no hyphenation pack on the card", code);
    return;
  }
  if (has && stored.crc32 == header.crc32 && stored.size == header.payloadSize &&
      stored.rootOffset == header.rootOffset) {
    return;
  }
  auto scratch = makeUniqueNoThrow<uint8_t[]>(kScratchBytes);
  if (!scratch) {
    LOG_ERR("HYP", "%s: no %u B for the copy", code, static_cast<unsigned>(kScratchBytes));
    return;
  }
  const uint32_t start = millis();
  const HyphenationStore::Fill result = store.fill(src, header, scratch.get(), kScratchBytes);
  LOG_INF("HYP", "%s: %u B into the flash store: %s in %lu ms", code, static_cast<unsigned>(header.payloadSize),
          result == HyphenationStore::Fill::Stored ? "stored" : "FAILED", static_cast<unsigned long>(millis() - start));
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

void prepareForBook(const std::string& languageTag) {
  const char* code = Hyphenator::languageCode(languageTag);
  const LanguageEntry* entry = code ? findLanguageEntry(code) : nullptr;
#if CONFIG_SPIRAM
  if (!entry || entry->builtIn) {
    release();
    return;
  }
  load(code);
#else
  // Every language's mapping goes, while no build holds its patterns: only the open book's is mapped again (on its
  // first lookup), and the MMU pages of the others return to the font cache.
  forgetPackPatterns();
  store.unmapAll();
  if (!mounted || !entry || entry->builtIn) return;
  ensureStored(code);
#endif
}

}  // namespace HyphenationPacks
