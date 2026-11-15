// The interface language read from the card a string at a time (langcard::CardStrings), for a board whose flash area
// has no room for the language slot. Every pack gen_i18n.py writes reads key for key as the slot built from it reads
// (the real FlashLangPartition over test/flash_lang_partition's in-memory partition); a heap that refuses a chunk and a
// card that refuses a read leave the key English for a moment and are asked again; a language change and a file of
// another pack, another firmware or a cut write build the file again.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "Arduino.h"
#include "FlashLangPartition.h"
#include "I18nStrings.h"
#include "LangCardStrings.h"
#include "LangPack.h"
#include "esp_partition.h"

namespace {

namespace flp = FlashLangPartition;
using i18n_strings::KEY_HASHES;
using i18n_strings::KEY_LAYOUT_STAMP;
using i18n_strings::KEYS_BY_HASH;

constexpr uint16_t KEY_COUNT = static_cast<uint16_t>(StrId::_COUNT);
constexpr langpack::KeyTable KEYS{KEY_HASHES, KEYS_BY_HASH, KEY_COUNT};

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A pack, in memory, read from where it stands.
struct MemPack {
  std::vector<uint8_t> bytes;
  size_t at = 0;
  langpack::Header h;
  int builds = 0;

  static int read(void* ctx, uint8_t* buf, const size_t len) {
    auto* f = static_cast<MemPack*>(ctx);
    const size_t n = std::min(len, f->bytes.size() - f->at);
    memcpy(buf, f->bytes.data() + f->at, n);
    f->at += n;
    return static_cast<int>(n);
  }
  // What I18n hands the slot and the card file alike (producePackStrings).
  static bool produce(void* ctx, flp::StringsWriter& out) {
    auto* p = static_cast<MemPack*>(ctx);
    ++p->builds;
    p->at = p->h.payloadOffset();
    return langpack::inflateStrings({&MemPack::read, p}, p->h, KEYS, out);
  }
  flp::Strings strings() { return flp::Strings{h.rawLen, &MemPack::produce, this}; }
  uint32_t stamp() const { return langpack::slotStamp(h, KEY_LAYOUT_STAMP); }
};

MemPack loadPack(const std::string& path) {
  MemPack p;
  p.bytes = readFile(path);
  EXPECT_TRUE(langpack::parseHeader(p.bytes.data(), p.bytes.size(), p.h)) << path;
  return p;
}

std::string packPath(const char* dir, const char* code) {
  return std::string(LANG_PACKS_DIR) + "/" + dir + "/" + code + ".cplang";
}

// The card file, in memory: the file in place and the one being written, with a switch that fails every read.
class MemStringsFile final : public langcard::StringsFile {
 public:
  std::vector<uint8_t> placed;
  bool hasFile = false;
  std::vector<uint8_t> pending;
  bool writing = false;
  bool failReads = false;
  bool failCreate = false;
  int reads = 0;
  int releases = 0;

  uint32_t size() override { return hasFile && !failReads ? static_cast<uint32_t>(placed.size()) : 0; }
  bool read(const uint32_t offset, void* dst, const uint32_t len) override {
    ++reads;
    if (!hasFile || failReads || offset > placed.size() || len > placed.size() - offset) return false;
    memcpy(dst, placed.data() + offset, len);
    return true;
  }
  void release() override { ++releases; }
  bool create() override {
    if (failCreate) return false;
    pending.clear();
    writing = true;
    return true;
  }
  bool append(const void* src, const uint32_t len) override {
    if (!writing) return false;
    const auto* b = static_cast<const uint8_t*>(src);
    pending.insert(pending.end(), b, b + len);
    return true;
  }
  bool patch(const uint32_t offset, const void* src, const uint32_t len) override {
    if (!writing || offset > pending.size() || len > pending.size() - offset) return false;
    memcpy(pending.data() + offset, src, len);
    return true;
  }
  bool commit() override {
    if (!writing) return false;
    placed = pending;
    hasFile = true;
    writing = false;
    return true;
  }
  void discard() override {
    pending.clear();
    writing = false;
  }
};

bool growAlways(size_t) { return true; }
std::atomic<bool> heapRefuses{false};
bool growUnlessRefused(size_t) { return !heapRefuses.load(); }

// What the slot reads for `id`: its string, or nullptr for a key that reads as English.
const char* slotString(const flp::Mapped& m, const uint16_t id) {
  return (m.offsets[id] & flp::FALLBACK) ? nullptr : m.data + m.offsets[id];
}

std::string orNull(const char* s) { return s ? std::string(s) : std::string("<English>"); }

class LangCardStrings : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { fake_flash::reset(0x360000); }  // the C3's `spiffs` partition, for the slot
  void SetUp() override {
    host_clock::valueRef() = 5000;
    heapRefuses = false;
  }
  void TearDown() override {
    flp::unmap();
    host_clock::valueRef() = 0;
  }
};

TEST_F(LangCardStrings, EveryPackReadsKeyForKeyAsItsSlot) {
  std::vector<std::string> packs;
  for (const auto& dir : std::filesystem::directory_iterator(LANG_PACKS_DIR)) {
    for (const auto& file : std::filesystem::directory_iterator(dir.path())) {
      if (file.path().extension() == ".cplang") packs.push_back(file.path().string());
    }
  }
  std::sort(packs.begin(), packs.end());
  ASSERT_GE(packs.size(), 28u) << "gen_i18n.py --packs wrote " << packs.size() << " packs";

  for (size_t i = 0; i < packs.size(); ++i) {
    SCOPED_TRACE(packs[i]);
    MemPack pack = loadPack(packs[i]);
    const auto lang = static_cast<uint8_t>(i + 1);

    flp::Mapped m{};
    ASSERT_TRUE(flp::acquire(lang, pack.stamp(), KEY_COUNT, pack.strings(), nullptr, &m));

    MemStringsFile file;
    langcard::CardStrings card(file, &growAlways);
    ASSERT_TRUE(card.open(lang, pack.stamp(), KEY_COUNT, pack.strings()));
    EXPECT_TRUE(card.built());

    uint32_t expectedBytes = 0;
    size_t translated = 0;
    for (uint16_t id = 0; id < KEY_COUNT; ++id) {
      const char* want = slotString(m, id);
      ASSERT_EQ(orNull(want), orNull(card.get(id))) << "key " << id;
      if (want) {
        expectedBytes += static_cast<uint32_t>(strlen(want) + 1);
        ++translated;
      }
    }
    EXPECT_GT(translated, 700u);
    EXPECT_EQ(expectedBytes, card.internedBytes()) << "each string read once, with its NUL";
    // A string never crosses a chunk: past the strings, the arena holds the end of each chunk the next string did not
    // fit into (shorter than the longest string, 353 B) and what the last chunk has not used yet.
    EXPECT_LE(card.internedBytes(), card.arenaBytes());
    EXPECT_LT(card.arenaBytes() - card.internedBytes(),
              (card.arenaBytes() / langcard::CHUNK_BYTES) * 353 + langcard::CHUNK_BYTES);
    flp::unmap();
  }
}

TEST_F(LangCardStrings, AKeyIsReadFromTheCardOnceAndKeepsItsPointer) {
  MemPack pack = loadPack(packPath("Language-German", "de"));
  MemStringsFile file;
  langcard::CardStrings card(file, &growAlways);
  ASSERT_TRUE(card.open(4, pack.stamp(), KEY_COUNT, pack.strings()));
  const auto title = static_cast<uint16_t>(StrId::STR_SETTINGS_TITLE);
  EXPECT_EQ(0u, card.cardReads());
  const char* first = card.get(title);
  ASSERT_NE(nullptr, first);
  EXPECT_STREQ("Einstellungen", first);
  EXPECT_EQ(1u, card.cardReads());
  const int reads = file.reads;
  EXPECT_EQ(first, card.get(title)) << "the same pointer, from the arena";
  EXPECT_EQ(reads, file.reads) << "and no read of the card";
  EXPECT_EQ(static_cast<uint32_t>(langcard::CHUNK_BYTES), card.arenaBytes());
}

TEST_F(LangCardStrings, AHeapThatRefusesAChunkLeavesTheKeyEnglishUntilItGivesOne) {
  MemPack pack = loadPack(packPath("Language-German", "de"));
  MemStringsFile file;
  langcard::CardStrings card(file, &growUnlessRefused);
  ASSERT_TRUE(card.open(4, pack.stamp(), KEY_COUNT, pack.strings()));
  const auto title = static_cast<uint16_t>(StrId::STR_SETTINGS_TITLE);

  heapRefuses = true;
  EXPECT_EQ(nullptr, card.get(title)) << "no chunk: English";
  EXPECT_EQ(0u, card.arenaBytes());
  const uint32_t reads = card.cardReads();
  EXPECT_EQ(nullptr, card.get(title));
  EXPECT_EQ(reads, card.cardReads()) << "not asked again at once: every frame would";

  heapRefuses = false;
  EXPECT_EQ(nullptr, card.get(title)) << "still within the second";
  host_clock::valueRef() += langcard::RETRY_AFTER_MS;
  EXPECT_STREQ("Einstellungen", card.get(title));
  EXPECT_EQ(static_cast<uint32_t>(langcard::CHUNK_BYTES), card.arenaBytes());
}

TEST_F(LangCardStrings, ACardThatRefusesAReadLeavesTheKeyEnglishAndIsAskedAgainLater) {
  MemPack pack = loadPack(packPath("Language-French", "fr"));
  MemStringsFile file;
  langcard::CardStrings card(file, &growAlways);
  ASSERT_TRUE(card.open(5, pack.stamp(), KEY_COUNT, pack.strings()));
  const auto title = static_cast<uint16_t>(StrId::STR_SETTINGS_TITLE);
  const auto back = static_cast<uint16_t>(StrId::STR_BACK);

  file.failReads = true;  // USB Drive took the card, say
  const int releases = file.releases;
  EXPECT_EQ(nullptr, card.get(title));
  EXPECT_EQ(releases + 1, file.releases) << "the handle goes after a failed read";
  const int reads = file.reads;
  EXPECT_EQ(nullptr, card.get(back));
  EXPECT_EQ(reads, file.reads) << "a refusing card is left alone for a second, whichever key";

  file.failReads = false;
  host_clock::valueRef() += langcard::RETRY_AFTER_MS;
  const char* got = card.get(title);
  ASSERT_NE(nullptr, got);
  EXPECT_STRNE("Settings", got);
  EXPECT_NE(nullptr, card.get(back));
  EXPECT_EQ(static_cast<uint32_t>(strlen(got) + 1 + strlen(card.get(back)) + 1), card.internedBytes())
      << "the failed read kept nothing";
}

TEST_F(LangCardStrings, ALanguageChangeBuildsTheFileForTheNewPack) {
  MemPack de = loadPack(packPath("Language-German", "de"));
  MemPack fr = loadPack(packPath("Language-French", "fr"));
  const auto title = static_cast<uint16_t>(StrId::STR_SETTINGS_TITLE);
  MemStringsFile file;
  {
    langcard::CardStrings card(file, &growAlways);
    ASSERT_TRUE(card.open(4, de.stamp(), KEY_COUNT, de.strings()));
    EXPECT_STREQ("Einstellungen", card.get(title));
  }
  {
    // I18n drops the old store (and every pointer into it) before it opens the next one.
    langcard::CardStrings card(file, &growAlways);
    ASSERT_TRUE(card.open(5, fr.stamp(), KEY_COUNT, fr.strings()));
    EXPECT_TRUE(card.built());
    const char* got = card.get(title);
    ASSERT_NE(nullptr, got);
    EXPECT_STRNE("Einstellungen", got);
    EXPECT_EQ(1, fr.builds);
  }
  {
    langcard::CardStrings card(file, &growAlways);
    ASSERT_TRUE(card.open(4, de.stamp(), KEY_COUNT, de.strings()));
    EXPECT_TRUE(card.built()) << "back to German: one file, so built again";
    EXPECT_STREQ("Einstellungen", card.get(title));
  }
}

TEST_F(LangCardStrings, AFileOfAnotherPackFirmwareOrACutWriteIsBuiltAgain) {
  MemPack pack = loadPack(packPath("Language-Czech", "cs"));
  const auto title = static_cast<uint16_t>(StrId::STR_SETTINGS_TITLE);
  MemStringsFile file;
  auto openCard = [&](const uint32_t stamp, bool* built) {
    langcard::CardStrings card(file, &growAlways);
    const bool ok = card.open(6, stamp, KEY_COUNT, pack.strings());
    *built = card.built();
    return ok && std::string(card.get(title)) == "Nastavení";
  };
  bool built = false;
  ASSERT_TRUE(openCard(pack.stamp(), &built));
  EXPECT_TRUE(built);
  EXPECT_EQ(1, pack.builds);

  ASSERT_TRUE(openCard(pack.stamp(), &built));
  EXPECT_FALSE(built) << "the next boot reads the file as it is";
  EXPECT_EQ(1, pack.builds);

  ASSERT_TRUE(openCard(pack.stamp() ^ 1, &built));
  EXPECT_TRUE(built) << "another pack, or a firmware with other keys: another stamp";
  EXPECT_EQ(2, pack.builds);

  file.placed.pop_back();  // a file that lost its end
  ASSERT_TRUE(openCard(pack.stamp() ^ 1, &built));
  EXPECT_TRUE(built);

  file.placed[0] = 'X';  // not one of ours
  ASSERT_TRUE(openCard(pack.stamp() ^ 1, &built));
  EXPECT_TRUE(built);
  EXPECT_EQ(4, pack.builds);
}

TEST_F(LangCardStrings, AFileThatCannotBeWrittenLeavesTheLanguageEnglish) {
  MemPack pack = loadPack(packPath("Language-Polish", "pl"));
  MemStringsFile file;
  file.failCreate = true;
  langcard::CardStrings card(file, &growAlways);
  EXPECT_FALSE(card.open(7, pack.stamp(), KEY_COUNT, pack.strings()));
  EXPECT_EQ(nullptr, card.get(static_cast<uint16_t>(StrId::STR_SETTINGS_TITLE)));
  EXPECT_FALSE(file.hasFile);
}

TEST_F(LangCardStrings, TwoTasksAskingAtOnceReadEachStringOnce) {
  MemPack pack = loadPack(packPath("Language-Russian", "ru"));
  MemStringsFile file;
  langcard::CardStrings card(file, &growAlways);
  ASSERT_TRUE(card.open(8, pack.stamp(), KEY_COUNT, pack.strings()));

  // The render task and the loop task: one walks the keys up, the other down.
  std::vector<std::string> up(KEY_COUNT);
  std::vector<std::string> down(KEY_COUNT);
  std::thread a([&] {
    for (uint16_t id = 0; id < KEY_COUNT; ++id) up[id] = orNull(card.get(id));
  });
  std::thread b([&] {
    for (int id = KEY_COUNT - 1; id >= 0; --id) down[id] = orNull(card.get(static_cast<uint16_t>(id)));
  });
  a.join();
  b.join();
  uint32_t expectedBytes = 0;
  for (uint16_t id = 0; id < KEY_COUNT; ++id) {
    ASSERT_EQ(up[id], down[id]) << "key " << id;
    if (up[id] != "<English>") expectedBytes += static_cast<uint32_t>(up[id].size() + 1);
  }
  EXPECT_EQ(expectedBytes, card.internedBytes()) << "no string read into the arena twice";
}

}  // namespace
