// The hyphenation pack path: a pack read from the card, copied into the flash store, mapped, and hyphenating
// exactly as the trie compiled into the image did.

#include <esp_rom_crc.h>
#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "SimNorFlash.h"
#include "lib/Epub/Epub/hyphenation/HyphenationCommon.h"
#include "lib/Epub/Epub/hyphenation/HyphenationPack.h"
#include "lib/Epub/Epub/hyphenation/HyphenationStore.h"
#include "lib/Epub/Epub/hyphenation/Hyphenator.h"
#include "lib/Epub/Epub/hyphenation/LanguageHyphenator.h"
#include "lib/Epub/Epub/hyphenation/LanguageRegistry.h"
// The reference: the tries as they were compiled in. This target builds the registry without them.
#include "lib/Epub/Epub/hyphenation/generated/hyph-de.trie.h"
#include "lib/Epub/Epub/hyphenation/generated/hyph-es.trie.h"
#include "lib/Epub/Epub/hyphenation/generated/hyph-fr.trie.h"
#include "lib/Epub/Epub/hyphenation/generated/hyph-it.trie.h"
#include "lib/Epub/Epub/hyphenation/generated/hyph-pl.trie.h"
#include "lib/Epub/Epub/hyphenation/generated/hyph-pt.trie.h"
#include "lib/Epub/Epub/hyphenation/generated/hyph-ru.trie.h"
#include "lib/Epub/Epub/hyphenation/generated/hyph-sv.trie.h"
#include "lib/Epub/Epub/hyphenation/generated/hyph-uk.trie.h"

namespace {

struct Builtin {
  const char* code;
  const SerializedHyphenationPatterns* patterns;
  bool cyrillic;
  const char* testWords;  // in test/hyphenation_eval/resources, null when there is none
};

const Builtin kBuiltins[] = {
    {"de", &de_patterns, false, "german_hyphenation_tests.txt"},
    {"es", &es_patterns, false, "spanish_hyphenation_tests.txt"},
    {"fr", &fr_patterns, false, "french_hyphenation_tests.txt"},
    {"it", &it_patterns, false, "italian_hyphenation_tests.txt"},
    {"pl", &pl_patterns, false, "polish_hyphenation_tests.txt"},
    {"pt", &pt_patterns, false, "portuguese_hyphenation_tests.txt"},
    {"ru", &ru_patterns, true, "russian_hyphenation_tests.txt"},
    {"sv", &sv_patterns, false, "swedish_hyphenation_tests.txt"},
    {"uk", &uk_patterns, true, nullptr},
};

constexpr uint32_t kRegionBytes = 384 * 1024;

std::vector<uint8_t> packOf(const char* code, const SerializedHyphenationPatterns& p) {
  hyphpack::Header h;
  h.code[0] = code[0];
  h.code[1] = code[1];
  h.minPrefix = 2;
  h.minSuffix = 2;
  h.rootOffset = static_cast<uint32_t>(p.rootOffset);
  h.payloadSize = static_cast<uint32_t>(p.size);
  h.crc32 = esp_rom_crc32_le(0, p.data, static_cast<uint32_t>(p.size));
  std::vector<uint8_t> out(hyphpack::kHeaderBytes);
  hyphpack::writeHeader(h, out.data());
  out.insert(out.end(), p.data, p.data + p.size);
  return out;
}

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class BufferSource final : public HyphenationPackSource {
 public:
  explicit BufferSource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  uint32_t size() const override { return static_cast<uint32_t>(bytes_.size()); }
  bool read(const uint32_t offset, void* dst, const uint32_t len) override {
    if (offset > bytes_.size() || len > bytes_.size() - offset) return false;
    std::memcpy(dst, bytes_.data() + offset, len);
    return true;
  }
  std::vector<uint8_t>& bytes() { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
};

HyphenationStore::Fill fillFrom(HyphenationStore& store, std::vector<uint8_t> pack) {
  BufferSource src(std::move(pack));
  hyphpack::Header h;
  if (!readPackHeader(src, h)) return HyphenationStore::Fill::BadSource;
  uint8_t scratch[1024];
  return store.fill(src, h, scratch, sizeof(scratch));
}

const Builtin& builtin(const char* code) {
  for (const auto& b : kBuiltins) {
    if (std::strcmp(b.code, code) == 0) return b;
  }
  ADD_FAILURE() << "no built-in trie " << code;
  return kBuiltins[0];
}

// The words of a hyphenation_eval resource file.
std::vector<std::string> testWords(const char* file) {
  std::vector<std::string> words;
  std::ifstream in(std::string(HYPHENATION_RESOURCES_DIR) + "/" + file);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    words.push_back(line.substr(0, line.find('|')));
  }
  return words;
}

// The registry's provider for these tests: the store the test filled.
HyphenationStore* gStore = nullptr;
bool storeProvider(const char* code, SerializedHyphenationPatterns& out, uint32_t& crc32) {
  return gStore && gStore->map(code, out, &crc32);
}

class ProviderScope {
 public:
  explicit ProviderScope(HyphenationStore& store) {
    forgetPackPatterns();
    gStore = &store;
    setHyphenationPatternProvider(storeProvider);
  }
  ~ProviderScope() {
    forgetPackPatterns();
    setHyphenationPatternProvider(nullptr);
    gStore = nullptr;
    Hyphenator::setLanguageHyphenator(nullptr);
  }
};

std::string describe(const std::vector<Hyphenator::BreakInfo>& breaks) {
  std::ostringstream s;
  for (const auto& b : breaks) s << b.byteOffset << (b.requiresInsertedHyphen ? "+ " : " ");
  return s.str();
}

}  // namespace

TEST(HyphenationPack, HeaderRoundTripsAndRefusesWhatItDoesNotRead) {
  hyphpack::Header h;
  h.code[0] = 'd';
  h.code[1] = 'e';
  h.minPrefix = 2;
  h.minSuffix = 2;
  h.rootOffset = 0x32542;
  h.payloadSize = 206259;
  h.crc32 = 0xDEADBEEF;
  uint8_t bytes[hyphpack::kHeaderBytes];
  hyphpack::writeHeader(h, bytes);
  hyphpack::Header back;
  ASSERT_TRUE(hyphpack::parseHeader(bytes, back));
  EXPECT_STREQ(back.code, "de");
  EXPECT_EQ(back.rootOffset, h.rootOffset);
  EXPECT_EQ(back.payloadSize, h.payloadSize);
  EXPECT_EQ(back.crc32, h.crc32);

  auto refused = [&](const size_t at, const uint8_t value) {
    uint8_t copy[hyphpack::kHeaderBytes];
    std::memcpy(copy, bytes, sizeof(copy));
    copy[at] = value;
    hyphpack::Header out;
    return !hyphpack::parseHeader(copy, out);
  };
  EXPECT_TRUE(refused(0, 'X'));    // magic
  EXPECT_TRUE(refused(4, 2));      // version
  EXPECT_TRUE(refused(5, 'D'));    // code not lowercase
  EXPECT_TRUE(refused(9, 1));      // flags
  EXPECT_TRUE(refused(10, 1));     // reserved
  EXPECT_TRUE(refused(15, 0x7F));  // root offset past the payload
  EXPECT_TRUE(refused(19, 0x7F));  // payload past the limit
}

// The packs scripts/build_hyphenation_packs.py writes hold exactly the tries that were compiled in.
TEST(HyphenationPack, ScriptPacksHoldTheBuiltInTries) {
  for (const auto& b : kBuiltins) {
    SCOPED_TRACE(b.code);
    const auto file = readFile(std::string(HYPHENATION_PACK_DIR) + "/hyph-" + b.code + ".cphyph");
    EXPECT_EQ(file, packOf(b.code, *b.patterns));
  }
}

TEST(HyphenationStore, StoresMapsAndKnowsWhatItHolds) {
  SimNorFlash flash(kRegionBytes);
  HyphenationStore store(flash);
  ASSERT_TRUE(store.mount());
  EXPECT_EQ(store.count(), 0);

  EXPECT_EQ(fillFrom(store, packOf("de", de_patterns)), HyphenationStore::Fill::Stored);
  EXPECT_EQ(fillFrom(store, packOf("de", de_patterns)), HyphenationStore::Fill::Unchanged);
  EXPECT_EQ(store.count(), 1);

  SerializedHyphenationPatterns mapped{};
  ASSERT_TRUE(store.map("de", mapped));
  ASSERT_EQ(mapped.size, de_patterns.size);
  EXPECT_EQ(mapped.rootOffset, de_patterns.rootOffset);
  EXPECT_EQ(std::memcmp(mapped.data, de_patterns.data, mapped.size), 0);
  // German's 206,259 B start one header pair (8 KB) into a 64 KB-aligned region: four MMU pages.
  EXPECT_EQ(flash.freeMapPages(), 32u - 4u);
  EXPECT_FALSE(store.map("fr", mapped));

  // A reboot finds the same.
  flash.powerCycle();
  HyphenationStore again(flash);
  ASSERT_TRUE(again.mount());
  HyphenationStore::Entry e;
  ASSERT_TRUE(again.find("de", e));
  EXPECT_EQ(e.size, de_patterns.size);
  EXPECT_EQ(flash.violations(), 0u);
  EXPECT_EQ(flash.reprograms(), 0u);
}

TEST(HyphenationStore, RefusesAPackWhosePayloadFailsItsCrc) {
  SimNorFlash flash(kRegionBytes);
  HyphenationStore store(flash);
  ASSERT_TRUE(store.mount());
  auto pack = packOf("fr", fr_patterns);
  pack[hyphpack::kHeaderBytes + 100] ^= 0x01;
  EXPECT_EQ(fillFrom(store, pack), HyphenationStore::Fill::BadSource);
  EXPECT_EQ(store.count(), 0);
}

// Every built-in language at once: 323 KB of tries in a 384 KB region.
TEST(HyphenationStore, HoldsEveryLanguageThatWasBuiltIn) {
  SimNorFlash flash(kRegionBytes);
  HyphenationStore store(flash);
  ASSERT_TRUE(store.mount());
  for (const auto& b : kBuiltins) {
    EXPECT_EQ(fillFrom(store, packOf(b.code, *b.patterns)), HyphenationStore::Fill::Stored) << b.code;
  }
  EXPECT_EQ(store.count(), 9);
  for (const auto& b : kBuiltins) {
    HyphenationStore::Entry e;
    EXPECT_TRUE(store.find(b.code, e)) << b.code;
  }
}

TEST(HyphenationStore, DropsTheOldestWhenThereIsNoRoom) {
  // 256 KB: German (51 sectors) and Russian (9) fill 60 of the 62 payload sectors; Swedish (6) needs room.
  SimNorFlash flash(256 * 1024);
  HyphenationStore store(flash);
  ASSERT_TRUE(store.mount());
  ASSERT_EQ(fillFrom(store, packOf("de", de_patterns)), HyphenationStore::Fill::Stored);
  ASSERT_EQ(fillFrom(store, packOf("ru", ru_patterns)), HyphenationStore::Fill::Stored);
  ASSERT_EQ(fillFrom(store, packOf("sv", sv_patterns)), HyphenationStore::Fill::Stored);
  HyphenationStore::Entry e;
  EXPECT_FALSE(store.find("de", e));
  EXPECT_TRUE(store.find("ru", e));
  EXPECT_TRUE(store.find("sv", e));
  SerializedHyphenationPatterns mapped{};
  ASSERT_TRUE(store.map("sv", mapped));
  EXPECT_EQ(std::memcmp(mapped.data, sv_patterns.data, mapped.size), 0);
  ASSERT_TRUE(store.map("ru", mapped));
  EXPECT_EQ(std::memcmp(mapped.data, ru_patterns.data, mapped.size), 0);
  EXPECT_EQ(flash.violations(), 0u);
  EXPECT_EQ(flash.reprograms(), 0u);
}

TEST(HyphenationStore, ReplacesALanguageWithNewBytes) {
  SimNorFlash flash(kRegionBytes);
  HyphenationStore store(flash);
  ASSERT_TRUE(store.mount());
  ASSERT_EQ(fillFrom(store, packOf("it", it_patterns)), HyphenationStore::Fill::Stored);
  // Italian's header with Portuguese's trie under it: other bytes for the same language.
  ASSERT_EQ(fillFrom(store, packOf("it", pt_patterns)), HyphenationStore::Fill::Stored);
  EXPECT_EQ(store.count(), 1);
  SerializedHyphenationPatterns mapped{};
  ASSERT_TRUE(store.map("it", mapped));
  ASSERT_EQ(mapped.size, pt_patterns.size);
  EXPECT_EQ(std::memcmp(mapped.data, pt_patterns.data, mapped.size), 0);
}

// Bytes that changed behind a standing header (another firmware's font cache, written over the region) are never
// handed out.
TEST(HyphenationStore, WithholdsATrieThatChangedBehindItsHeader) {
  SimNorFlash flash(kRegionBytes);
  HyphenationStore store(flash);
  ASSERT_TRUE(store.mount());
  ASSERT_EQ(fillFrom(store, packOf("pl", pl_patterns)), HyphenationStore::Fill::Stored);
  HyphenationStore::Entry e;
  ASSERT_TRUE(store.find("pl", e));
  flash.raw()[e.offset + 1234] &= 0x0F;
  SerializedHyphenationPatterns mapped{};
  EXPECT_FALSE(store.map("pl", mapped));
  // The card's pack puts it right.
  EXPECT_EQ(fillFrom(store, packOf("pl", pl_patterns)), HyphenationStore::Fill::Unchanged);
}

// A power cut anywhere in a fill leaves a store that mounts, maps what it names correctly, and takes the fill again.
TEST(HyphenationStore, PowerCutAnywhereLeavesAWorkingStore) {
  // The step count of a clean fill of German over a store holding Russian, Polish and Swedish, which it drops all
  // three of to find 51 sectors in a row: the eviction header, the erase, the copy, the final header.
  auto prepared = [] {
    auto flash = std::make_unique<SimNorFlash>(256 * 1024);
    HyphenationStore store(*flash);
    EXPECT_TRUE(store.mount());
    EXPECT_EQ(fillFrom(store, packOf("ru", ru_patterns)), HyphenationStore::Fill::Stored);
    EXPECT_EQ(fillFrom(store, packOf("pl", pl_patterns)), HyphenationStore::Fill::Stored);
    EXPECT_EQ(fillFrom(store, packOf("sv", sv_patterns)), HyphenationStore::Fill::Stored);
    return flash;
  };
  uint64_t total = 0;
  {
    auto flash = prepared();
    HyphenationStore store(*flash);
    ASSERT_TRUE(store.mount());
    const uint64_t before = flash->steps();
    ASSERT_EQ(fillFrom(store, packOf("de", de_patterns)), HyphenationStore::Fill::Stored);
    total = flash->steps() - before;
  }
  ASSERT_GT(total, 200000u);

  std::vector<uint64_t> cuts;
  for (uint64_t c = 0; c < total; c += total / 61) cuts.push_back(c);
  for (uint64_t c = 0; c < 120; ++c) cuts.push_back(c);              // the eviction header and the erase
  for (uint64_t c = total - 120; c < total; ++c) cuts.push_back(c);  // the final header
  for (const uint64_t cut : cuts) {
    SCOPED_TRACE(cut);
    auto flash = prepared();
    {
      HyphenationStore store(*flash);
      ASSERT_TRUE(store.mount());
      flash->partialCut(cut % 2 == 1);
      flash->cutAfterSteps(static_cast<int64_t>(cut));
      EXPECT_NE(fillFrom(store, packOf("de", de_patterns)), HyphenationStore::Fill::Stored);
    }
    flash->powerCycle();
    HyphenationStore store(*flash);
    ASSERT_TRUE(store.mount());
    // Whatever it names maps to the right bytes.
    for (uint8_t i = 0; i < store.count(); ++i) {
      HyphenationStore::Entry e;
      ASSERT_TRUE(store.entryAt(i, e));
      SerializedHyphenationPatterns mapped{};
      ASSERT_TRUE(store.map(e.code, mapped)) << e.code;
      const Builtin& b = builtin(e.code);
      ASSERT_EQ(mapped.size, b.patterns->size);
      EXPECT_EQ(std::memcmp(mapped.data, b.patterns->data, mapped.size), 0) << e.code;
      store.unmapAll();
    }
    EXPECT_EQ(fillFrom(store, packOf("de", de_patterns)), HyphenationStore::Fill::Stored);
    SerializedHyphenationPatterns mapped{};
    ASSERT_TRUE(store.map("de", mapped));
    EXPECT_EQ(std::memcmp(mapped.data, de_patterns.data, mapped.size), 0);
    EXPECT_EQ(flash->violations(), 0u);
  }
}

// The benchmark: German from a pack breaks every word exactly where the compiled-in trie did, through the whole
// Hyphenator pipeline (explicit hyphens, apostrophes, fallback). The other languages likewise.
TEST(HyphenationStore, PackLanguagesHyphenateExactlyAsTheBuiltInTries) {
  SimNorFlash flash(kRegionBytes);
  HyphenationStore store(flash);
  ASSERT_TRUE(store.mount());
  for (const auto& b : kBuiltins)
    ASSERT_EQ(fillFrom(store, packOf(b.code, *b.patterns)), HyphenationStore::Fill::Stored);
  ProviderScope scope(store);

  // Every language's word list, through every language's patterns: 45,000 words through German's alone.
  std::vector<std::string> words;
  for (const auto& b : kBuiltins) {
    if (!b.testWords) continue;
    const auto w = testWords(b.testWords);
    words.insert(words.end(), w.begin(), w.end());
  }
  ASSERT_GT(words.size(), 40000u);
  words.push_back("US-Satellitensystems");
  words.push_back("Donaudampfschifffahrtsgesellschaftskapitän");
  words.push_back("l'état-major");

  for (const auto& b : kBuiltins) {
    SCOPED_TRACE(b.code);
    const LanguageHyphenator reference(*b.patterns, b.cyrillic ? isCyrillicLetter : isLatinLetter,
                                       b.cyrillic ? toLowerCyrillic : toLowerLatin);
    // One language mapped at a time, as a book open leaves it (kMaxMapped is 2).
    forgetPackPatterns();
    store.unmapAll();
    const LanguageHyphenator* fromPack = Hyphenator::forLanguage(b.code);
    ASSERT_NE(fromPack, nullptr);
    ASSERT_NE(fromPack->patterns().data, b.patterns->data);  // really the mapped copy
    size_t breaks = 0;
    for (const std::string& word : words) {
      for (const bool fallback : {false, true}) {
        Hyphenator::setLanguageHyphenator(&reference);
        const auto expected = Hyphenator::breakOffsets(word, fallback);
        Hyphenator::setLanguageHyphenator(fromPack);
        const auto actual = Hyphenator::breakOffsets(word, fallback);
        ASSERT_EQ(describe(actual), describe(expected)) << word;
        breaks += actual.size();
      }
    }
    EXPECT_GT(breaks, 10000u);
  }
}

TEST(HyphenationStore, NoPackMeansNoPatternsForThatLanguage) {
  SimNorFlash flash(kRegionBytes);
  HyphenationStore store(flash);
  ASSERT_TRUE(store.mount());
  ASSERT_EQ(fillFrom(store, packOf("de", de_patterns)), HyphenationStore::Fill::Stored);
  ProviderScope scope(store);
  EXPECT_NE(Hyphenator::forLanguage("de-DE"), nullptr);
  EXPECT_NE(Hyphenator::forLanguage("ger"), nullptr);
  EXPECT_EQ(Hyphenator::forLanguage("fr"), nullptr);
  EXPECT_NE(Hyphenator::forLanguage("en"), nullptr);  // English stays in the image
}

// forgetPackPatterns() before the store unmaps: a hyphenator a caller still holds finds no breaks rather than
// reading unmapped flash.
TEST(HyphenationStore, AForgottenPackLeavesHeldHyphenatorsHarmless) {
  SimNorFlash flash(kRegionBytes);
  HyphenationStore store(flash);
  ASSERT_TRUE(store.mount());
  ASSERT_EQ(fillFrom(store, packOf("de", de_patterns)), HyphenationStore::Fill::Stored);
  ProviderScope scope(store);
  const LanguageHyphenator* held = Hyphenator::forLanguage("de");
  ASSERT_NE(held, nullptr);
  Hyphenator::setLanguageHyphenator(held);
  EXPECT_FALSE(Hyphenator::breakOffsets("Silbentrennung", false).empty());
  forgetPackPatterns();
  store.unmapAll();
  EXPECT_TRUE(Hyphenator::breakOffsets("Silbentrennung", false).empty());
  // Asked again, the registry maps it again.
  EXPECT_NE(Hyphenator::forLanguage("de"), nullptr);
}

// A section's cache key: the image's own tries of before packs keep the layouts they made; another trie, or none, gets
// a layout of its own.
TEST(HyphenationStore, LayoutKeyKeepsTheImagesLayoutsAndSeparatesOthers) {
  SimNorFlash flash(kRegionBytes);
  HyphenationStore store(flash);
  ASSERT_TRUE(store.mount());
  ASSERT_EQ(fillFrom(store, packOf("de", de_patterns)), HyphenationStore::Fill::Stored);
  ASSERT_EQ(fillFrom(store, packOf("it", pt_patterns)), HyphenationStore::Fill::Stored);  // not Italian's bytes
  ProviderScope scope(store);
  EXPECT_EQ(Hyphenator::layoutKey("de-DE"), 0u);  // the bytes the image carried
  EXPECT_EQ(Hyphenator::layoutKey("en"), 0u);
  EXPECT_EQ(Hyphenator::layoutKey("ja"), 0u);  // not a language the firmware hyphenates: as before
  EXPECT_EQ(Hyphenator::layoutKey("fr"), kNoPatternsLayoutKey);
  EXPECT_EQ(Hyphenator::layoutKey("it"),
            esp_rom_crc32_le(0, pt_patterns.data, static_cast<uint32_t>(pt_patterns.size)));
}
