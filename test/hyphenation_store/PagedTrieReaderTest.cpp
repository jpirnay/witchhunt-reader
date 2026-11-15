// The hyphenation trie read in place from its pack file (PagedTrieReader): what a C3 without the flash store
// hyphenates by. It must break every word exactly as the trie in memory does, whatever the pages, and a card read
// that fails must cost the word its breaks, never give it others.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "lib/Epub/Epub/hyphenation/HyphenationCommon.h"
#include "lib/Epub/Epub/hyphenation/HyphenationPack.h"
#include "lib/Epub/Epub/hyphenation/HyphenationStore.h"
#include "lib/Epub/Epub/hyphenation/Hyphenator.h"
#include "lib/Epub/Epub/hyphenation/LanguageHyphenator.h"
#include "lib/Epub/Epub/hyphenation/LanguageRegistry.h"
#include "lib/Epub/Epub/hyphenation/PagedTrieReader.h"

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A pack file as the card holds it: `pad` bytes, then the pack, so the trie can start at any offset of a page.
class FileSource final : public HyphenationPackSource {
 public:
  explicit FileSource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  uint32_t size() const override { return static_cast<uint32_t>(bytes_.size()); }
  bool read(const uint32_t offset, void* dst, const uint32_t len) override {
    ++reads;
    if (failReads > 0) {
      --failReads;
      return false;
    }
    if (offset > bytes_.size() || len > bytes_.size() - offset) return false;
    std::memcpy(dst, bytes_.data() + offset, len);
    return true;
  }
  const std::vector<uint8_t>& bytes() const { return bytes_; }

  uint32_t reads = 0;
  uint32_t failReads = 0;  // the next this many reads fail

 private:
  std::vector<uint8_t> bytes_;
};

struct HostedPack {
  std::string code;
  std::vector<uint8_t> bytes;
};

// Every pack the repo offers for download: assets/sd-fonts/Hyphenation-<Language>/hyph-<code>.cphyph.
std::vector<HostedPack> hostedPacks() {
  std::vector<HostedPack> packs;
  for (const auto& dir : std::filesystem::directory_iterator(HYPHENATION_PACK_DIR)) {
    if (!dir.is_directory() || dir.path().filename().string().rfind("Hyphenation-", 0) != 0) continue;
    for (const auto& file : std::filesystem::directory_iterator(dir.path())) {
      const std::string name = file.path().filename().string();
      if (name.size() == 14 && name.rfind("hyph-", 0) == 0 && file.path().extension() == ".cphyph") {
        packs.push_back({name.substr(5, 2), readFile(file.path().string())});
      }
    }
  }
  return packs;
}

// The words to break in `code`: the hyphenation_eval list of the language where there is one (words from books, in
// their order), else the reference of its patterns. Ukrainian has neither and borrows Russian's.
std::vector<std::string> wordsFor(const std::string& code) {
  static const std::pair<const char*, const char*> kEval[] = {
      {"de", "german"},     {"es", "spanish"}, {"fr", "french"},  {"it", "italian"}, {"pl", "polish"},
      {"pt", "portuguese"}, {"ru", "russian"}, {"sv", "swedish"}, {"uk", "russian"}};
  std::string path = std::string(HYPHENATION_PACK_REFERENCE_DIR) + "/" + code + "_reference.txt";
  for (const auto& [c, name] : kEval) {
    if (code == c) path = std::string(HYPHENATION_RESOURCES_DIR) + "/" + name + "_hyphenation_tests.txt";
  }
  std::vector<std::string> words;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    words.push_back(line.substr(0, line.find('|')));
  }
  return words;
}

// The pack's header, from the pack's bytes.
hyphpack::Header headerOf(const std::vector<uint8_t>& pack) {
  hyphpack::Header h;
  EXPECT_TRUE(hyphpack::parseHeader(pack.data(), h));
  return h;
}

// The language's hyphenator over `patterns`, with the rules the registry gives it.
LanguageHyphenator hyphenatorOf(const std::string& code, const SerializedHyphenationPatterns& patterns) {
  const LanguageEntry* entry = findLanguageEntry(code.c_str());
  EXPECT_NE(entry, nullptr) << code;
  return LanguageHyphenator(patterns, entry->isLetter, entry->toLower, entry->minPrefix, entry->minSuffix,
                            entry->insertsHyphen);
}

// A pack whose trie starts `pad` bytes later in its file than the pack's own 24.
std::vector<uint8_t> padded(const std::vector<uint8_t>& pack, const uint32_t pad) {
  std::vector<uint8_t> out(pad, 0xA5);
  out.insert(out.end(), pack.begin(), pack.end());
  return out;
}

}  // namespace

// Every hosted pack breaks every word of its list exactly as its trie in memory does, read through four pages of its
// file. Also measures what the card would be asked for: pages read per word, for every word of the list (a reader
// kept from word to word, as a section build keeps it) and for every tenth (about how often a line ends in a word the
// layout tries to break).
TEST(PagedTrieReader, EveryHostedPackBreaksAsTheTrieInMemory) {
  const std::vector<HostedPack> packs = hostedPacks();
  ASSERT_GE(packs.size(), 31u);
  size_t words = 0;
  size_t breaks = 0;
  for (const HostedPack& pack : packs) {
    SCOPED_TRACE(pack.code);
    const hyphpack::Header h = headerOf(pack.bytes);
    const SerializedHyphenationPatterns inMemory{h.rootOffset, pack.bytes.data() + hyphpack::kHeaderBytes,
                                                 h.payloadSize};
    FileSource everyFile(pack.bytes);
    PagedTrieReader everyReader(everyFile, hyphpack::kHeaderBytes, h.payloadSize);
    FileSource tenthFile(pack.bytes);
    PagedTrieReader tenthReader(tenthFile, hyphpack::kHeaderBytes, h.payloadSize);

    const LanguageHyphenator reference = hyphenatorOf(pack.code, inMemory);
    const LanguageHyphenator every = hyphenatorOf(pack.code, {h.rootOffset, nullptr, h.payloadSize, &everyReader});
    const LanguageHyphenator tenth = hyphenatorOf(pack.code, {h.rootOffset, nullptr, h.payloadSize, &tenthReader});
    ASSERT_TRUE(every.hasPatterns());

    const std::vector<std::string> list = wordsFor(pack.code);
    ASSERT_GE(list.size(), 100u);
    size_t tenthWords = 0;
    for (size_t i = 0; i < list.size(); ++i) {
      const std::vector<CodepointInfo> cps = collectCodepoints(list[i]);
      const std::vector<size_t> expected = reference.breakIndexes(cps);
      ASSERT_EQ(every.breakIndexes(cps), expected) << list[i];
      if (i % 10 == 0) {
        ASSERT_EQ(tenth.breakIndexes(cps), expected) << list[i];
        ++tenthWords;
      }
      breaks += expected.size();
    }
    EXPECT_FALSE(everyReader.failed());
    words += list.size();
    std::printf("[ pages    ] %s: %6u B trie, %5zu words: %.2f pages/word; every tenth word: %.2f pages/word\n",
                pack.code.c_str(), static_cast<unsigned>(h.payloadSize), list.size(),
                static_cast<double>(everyReader.misses()) / static_cast<double>(list.size()),
                static_cast<double>(tenthReader.misses()) / static_cast<double>(tenthWords));
  }
  EXPECT_GT(words, 20000u);
  EXPECT_GT(breaks, 20000u);
}

// Nodes straddle page edges wherever the trie lies in its file: shifted through a page, German still breaks as in
// memory, and so does Hungarian, the largest trie.
TEST(PagedTrieReader, BreaksTheSameWhereverTheTrieStartsInAPage) {
  for (const HostedPack& pack : hostedPacks()) {
    if (pack.code != "de" && pack.code != "hu") continue;
    SCOPED_TRACE(pack.code);
    const hyphpack::Header h = headerOf(pack.bytes);
    const SerializedHyphenationPatterns inMemory{h.rootOffset, pack.bytes.data() + hyphpack::kHeaderBytes,
                                                 h.payloadSize};
    const LanguageHyphenator reference = hyphenatorOf(pack.code, inMemory);
    std::vector<std::string> list = wordsFor(pack.code);
    list.resize(std::min<size_t>(list.size(), 400));
    for (const uint32_t pad : {1u, 2u, 3u, 100u, 255u, 256u, 487u, 509u, 510u, 511u}) {
      SCOPED_TRACE(pad);
      FileSource file(padded(pack.bytes, pad));
      PagedTrieReader reader(file, pad + hyphpack::kHeaderBytes, h.payloadSize);
      const LanguageHyphenator paged = hyphenatorOf(pack.code, {h.rootOffset, nullptr, h.payloadSize, &reader});
      for (const std::string& word : list) {
        const std::vector<CodepointInfo> cps = collectCodepoints(word);
        ASSERT_EQ(paged.breakIndexes(cps), reference.breakIndexes(cps)) << word;
      }
    }
  }
}

// A card read that fails costs the word it falls in its breaks, and only that word: the next one reads the card again.
TEST(PagedTrieReader, AFailedReadCostsTheWordItsBreaksAndNothingMore) {
  const std::vector<HostedPack> packs = hostedPacks();
  const auto german = std::find_if(packs.begin(), packs.end(), [](const HostedPack& p) { return p.code == "de"; });
  ASSERT_NE(german, packs.end());
  const hyphpack::Header h = headerOf(german->bytes);
  const LanguageHyphenator reference =
      hyphenatorOf("de", {h.rootOffset, german->bytes.data() + hyphpack::kHeaderBytes, h.payloadSize});
  FileSource file(german->bytes);
  PagedTrieReader reader(file, hyphpack::kHeaderBytes, h.payloadSize);
  const LanguageHyphenator paged = hyphenatorOf("de", {h.rootOffset, nullptr, h.payloadSize, &reader});

  const std::vector<std::string> list = wordsFor("de");
  size_t lost = 0;
  size_t compared = 0;
  for (size_t i = 0; i < list.size() && i < 3000; ++i) {
    const std::vector<CodepointInfo> cps = collectCodepoints(list[i]);
    // Every seventh word, its next read fails.
    if (i % 7 == 3) file.failReads = 1;
    const uint32_t readsBefore = file.reads;
    const std::vector<size_t> got = paged.breakIndexes(cps);
    const std::vector<size_t> expected = reference.breakIndexes(cps);
    const bool failedHere = file.failReads == 0 && i % 7 == 3 && file.reads > readsBefore;
    if (failedHere) {
      EXPECT_TRUE(got.empty()) << list[i];
      if (!expected.empty()) ++lost;
    } else {
      EXPECT_EQ(got, expected) << list[i];
      ++compared;
    }
    file.failReads = 0;
    EXPECT_FALSE(reader.failed()) << "a failure is the word's alone";
  }
  EXPECT_GT(lost, 0u);
  EXPECT_GT(compared, 2000u);
}

// A source that cannot read at all: nothing breaks, nothing is read past a failed read within a word, and the reader
// asks the card again for the next word.
TEST(PagedTrieReader, ACardThatReadsNothingBreaksNothing) {
  const std::vector<HostedPack> packs = hostedPacks();
  const auto german = std::find_if(packs.begin(), packs.end(), [](const HostedPack& p) { return p.code == "de"; });
  ASSERT_NE(german, packs.end());
  const hyphpack::Header h = headerOf(german->bytes);
  FileSource file(german->bytes);
  PagedTrieReader reader(file, hyphpack::kHeaderBytes, h.payloadSize);
  const LanguageHyphenator paged = hyphenatorOf("de", {h.rootOffset, nullptr, h.payloadSize, &reader});
  for (const char* word : {"Silbentrennung", "Donaudampfschifffahrt", "Haus"}) {
    file.failReads = 1000;
    const uint32_t before = file.reads;
    EXPECT_TRUE(paged.breakIndexes(collectCodepoints(word)).empty()) << word;
    EXPECT_EQ(file.reads - before, 1u) << "one failed read ends the word";
  }
  EXPECT_EQ(reader.misses(), 0u);
  file.failReads = 0;
  EXPECT_FALSE(paged.breakIndexes(collectCodepoints("Silbentrennung")).empty());
}

// A trie with bytes gone wrong (a pack damaged on the card) is walked inside its bounds: read from the card it breaks
// exactly as in memory, so neither walk reaches past the trie (the paged one would fail, and break nothing).
TEST(PagedTrieReader, ADamagedTrieIsWalkedInsideItsBounds) {
  const std::vector<HostedPack> packs = hostedPacks();
  const auto german = std::find_if(packs.begin(), packs.end(), [](const HostedPack& p) { return p.code == "de"; });
  ASSERT_NE(german, packs.end());
  const hyphpack::Header h = headerOf(german->bytes);
  std::vector<std::string> list = wordsFor("de");
  list.resize(std::min<size_t>(list.size(), 600));
  std::mt19937 rng(20261114);
  for (int round = 0; round < 12; ++round) {
    SCOPED_TRACE(round);
    std::vector<uint8_t> damaged = german->bytes;
    // Damage the bytes the walks read most: the nodes near the root, and the levels list at the trie's start.
    for (int i = 0; i < 64; ++i) {
      const uint32_t span = h.payloadSize < 8192 ? h.payloadSize : 8192;
      const uint32_t back = static_cast<uint32_t>(rng() % span);
      const uint32_t near = (rng() & 1) ? h.rootOffset - std::min(h.rootOffset, back) : back;
      damaged[hyphpack::kHeaderBytes + near] = static_cast<uint8_t>(rng());
    }
    const LanguageHyphenator inMemory =
        hyphenatorOf("de", {h.rootOffset, damaged.data() + hyphpack::kHeaderBytes, h.payloadSize});
    FileSource file(damaged);
    PagedTrieReader reader(file, hyphpack::kHeaderBytes, h.payloadSize);
    const LanguageHyphenator paged = hyphenatorOf("de", {h.rootOffset, nullptr, h.payloadSize, &reader});
    for (const std::string& word : list) {
      const std::vector<CodepointInfo> cps = collectCodepoints(word);
      ASSERT_EQ(paged.breakIndexes(cps), inMemory.breakIndexes(cps)) << word;
    }
  }
}

// The registry hands a pack language its patterns from the card as from the store: the provider gives a reader, and
// the book's words break through Hyphenator as they would from the flash store.
TEST(PagedTrieReader, TheRegistryHyphenatesThroughACardProvider) {
  const std::vector<HostedPack> packs = hostedPacks();
  const auto german = std::find_if(packs.begin(), packs.end(), [](const HostedPack& p) { return p.code == "de"; });
  ASSERT_NE(german, packs.end());
  const hyphpack::Header h = headerOf(german->bytes);
  static FileSource* file = nullptr;
  static PagedTrieReader* reader = nullptr;
  static hyphpack::Header header;
  FileSource source(german->bytes);
  PagedTrieReader pages(source, hyphpack::kHeaderBytes, h.payloadSize);
  file = &source;
  reader = &pages;
  header = h;
  forgetPackPatterns();
  setHyphenationPatternProvider([](const char* code, SerializedHyphenationPatterns& out, uint32_t& crc32) {
    if (std::strcmp(code, "de") != 0) return false;
    out = SerializedHyphenationPatterns{header.rootOffset, nullptr, header.payloadSize, reader};
    crc32 = header.crc32;
    return true;
  });
  EXPECT_EQ(hyphenationLayoutKey("de"), h.crc32 == findLanguageEntry("de")->imageTrieCrc ? 0u : h.crc32);
  const LanguageHyphenator* hyphenator = Hyphenator::forLanguage("de");
  ASSERT_NE(hyphenator, nullptr);
  ASSERT_TRUE(hyphenator->hasPatterns());
  const LanguageHyphenator reference =
      hyphenatorOf("de", {h.rootOffset, german->bytes.data() + hyphpack::kHeaderBytes, h.payloadSize});
  Hyphenator::setLanguageHyphenator(hyphenator);
  std::vector<std::string> list = wordsFor("de");
  list.resize(std::min<size_t>(list.size(), 500));
  size_t broken = 0;
  for (const std::string& word : list) {
    const std::vector<Hyphenator::BreakInfo> got = Hyphenator::breakOffsets(word, false);
    Hyphenator::setLanguageHyphenator(&reference);
    const std::vector<Hyphenator::BreakInfo> expected = Hyphenator::breakOffsets(word, false);
    Hyphenator::setLanguageHyphenator(hyphenator);
    ASSERT_EQ(got.size(), expected.size()) << word;
    for (size_t i = 0; i < got.size(); ++i) {
      EXPECT_EQ(got[i].byteOffset, expected[i].byteOffset) << word;
      EXPECT_EQ(got[i].requiresInsertedHyphen, expected[i].requiresInsertedHyphen) << word;
    }
    broken += got.empty() ? 0 : 1;
  }
  EXPECT_GT(broken, 100u);
  EXPECT_GT(file->reads, 0u);
  Hyphenator::setLanguageHyphenator(nullptr);
  forgetPackPatterns();
  setHyphenationPatternProvider(nullptr);
}
