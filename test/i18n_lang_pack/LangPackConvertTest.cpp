// The conversion after an update from 2.40 or 2.41RC (lib/I18n/LangPackConvert.h): a German slot as the
// release left it in flash (legacy_slot_fixture.py repeats its generator and table walk), found where the
// release kept it, proved sound, written out as a pack, and that pack read into this firmware's slot as
// I18n reads any: the reader keeps German after the update without a download.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "FlashFontPartition.h"
#include "FlashLangPartition.h"
#include "I18nStrings.h"
#include "LangPack.h"
#include "LangPackConvert.h"
#include "esp_partition.h"

namespace {

namespace flp = FlashLangPartition;
using i18n_strings::KEY_HASHES;
using i18n_strings::KEYS_BY_HASH;

constexpr uint32_t PARTITION_BYTES = 256 * 1024;
constexpr uint16_t KEY_COUNT = static_cast<uint16_t>(StrId::_COUNT);
constexpr langpack::KeyTable KEYS{KEY_HASHES, KEYS_BY_HASH, KEY_COUNT};

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A release whose slot the firmware converts, and where that slot lay.
struct Release {
  const char* name;
  uint8_t format;
  uint16_t keyCount;
  size_t slot;
};
// 2.40 kept a 64 KB slot in the partition's last 64 KB; 2.41RC the 128 KB slot this firmware keeps.
const Release RELEASES[] = {{"2.40", 1, 870, PARTITION_BYTES - 64 * 1024},
                            {"2.41RC", 2, 878, PARTITION_BYTES - FlashFontPartition::LANG_RESERVED_BYTES}};

const i18n_strings::LegacyLayout* layoutOf(const char* release) {
  for (uint8_t i = 0; i < i18n_strings::LEGACY_LAYOUT_COUNT; ++i) {
    if (strcmp(i18n_strings::LEGACY_LAYOUTS[i].release, release) == 0) return &i18n_strings::LEGACY_LAYOUTS[i];
  }
  return nullptr;
}

std::string fixture(const Release& r, const char* suffix) {
  return std::string(LEGACY_FIXTURES_DIR) + "/legacy-" + r.name + "-de." + suffix;
}

// A file on the card, in memory: written from where it stands, rewound for the header.
struct MemFile {
  std::vector<uint8_t> bytes;
  size_t at = 0;
  static bool write(void* ctx, const void* data, const size_t len) {
    auto* f = static_cast<MemFile*>(ctx);
    if (f->bytes.size() < f->at + len) f->bytes.resize(f->at + len);
    memcpy(f->bytes.data() + f->at, data, len);
    f->at += len;
    return true;
  }
  static bool rewind(void* ctx) {
    static_cast<MemFile*>(ctx)->at = 0;
    return true;
  }
  static int read(void* ctx, uint8_t* buf, const size_t len) {
    auto* f = static_cast<MemFile*>(ctx);
    const size_t n = std::min(len, f->bytes.size() - f->at);
    memcpy(buf, f->bytes.data() + f->at, n);
    f->at += n;
    return static_cast<int>(n);
  }
};

struct Collected final : langpack::EntrySink {
  std::map<uint32_t, std::string> values;
  uint32_t hash = 0;
  std::string value;
  bool begin(const uint32_t h) override {
    hash = h;
    value.clear();
    return true;
  }
  bool bytes(const char* data, const size_t len) override {
    value.append(data, len);
    return true;
  }
  bool end() override {
    values[hash] = value;
    return true;
  }
};

// What a correct conversion yields: the release's German strings of the keys this build has.
std::map<uint32_t, std::string> expectedEntries(const Release& r) {
  const std::vector<uint8_t> raw = readFile(fixture(r, "entries"));
  Collected all;
  langpack::EntryParser parser(all);
  EXPECT_TRUE(parser.feed(raw.data(), raw.size()));
  std::map<uint32_t, std::string> known;
  for (const auto& [hash, value] : all.values) {
    if (langpack::findKey(hash, KEY_HASHES, KEYS_BY_HASH, KEY_COUNT) != langpack::NO_KEY) known[hash] = value;
  }
  return known;
}

struct ParserSink {
  static bool write(void* ctx, const uint8_t* data, const size_t len) {
    return static_cast<langpack::EntryParser*>(ctx)->feed(data, len);
  }
};

class LangPackConvertTest : public ::testing::TestWithParam<Release> {
 protected:
  void SetUp() override {
    flp::unmap();
    fake_flash::reset(PARTITION_BYTES);
    slot = readFile(fixture(GetParam(), "slot"));
    ASSERT_GT(slot.size(), 40u) << "legacy_slot_fixture.py wrote no slot";
    fake_flash::Chip& chip = fake_flash::chip();
    ASSERT_EQ(esp_partition_write(&chip.part, GetParam().slot, slot.data(), slot.size()), ESP_OK);
  }
  void TearDown() override { flp::unmap(); }

  static langpack::OldSlot view(const flp::LegacySlot& old) {
    return langpack::OldSlot{old.table, old.data, old.dataLen, old.keyCount};
  }

  // The pack the conversion writes for `old`.
  MemFile convert(const flp::LegacySlot& old) {
    MemFile file;
    std::vector<uint8_t> block(1024);
    langpack::PackWriter writer({&MemFile::write, &MemFile::rewind, &file}, block.data(), block.size());
    EXPECT_TRUE(writer.begin("de", "Deutsch"));
    EXPECT_TRUE(langpack::convertOldSlot(view(old), layoutOf(GetParam().name)->ids, KEY_HASHES, KEY_COUNT, writer));
    EXPECT_TRUE(writer.finish());
    return file;
  }

  std::vector<uint8_t> slot;
};

TEST_P(LangPackConvertTest, TheSlotIsFoundWhereTheReleaseKeptItAndHoldsTogether) {
  const Release& r = GetParam();
  const auto* layout = layoutOf(r.name);
  ASSERT_NE(layout, nullptr) << "lib/I18n/legacy/" << r.name << ".keys";
  EXPECT_EQ(layout->slotFormat, r.format);
  EXPECT_EQ(layout->keyCount, r.keyCount);

  flp::LegacySlot old{};
  ASSERT_TRUE(flp::mapLegacy(r.format, &old));
  EXPECT_EQ(old.keyCount, r.keyCount);
  EXPECT_FALSE(old.hasExtension);
  ASSERT_LT(old.langIndex, strlen(layout->languages) / 2);
  EXPECT_EQ(std::string(layout->languages + 2 * old.langIndex, 2), "DE");
  EXPECT_TRUE(langpack::oldSlotSound(view(old)));
  flp::unmap();

  flp::LegacySlot none{};
  EXPECT_FALSE(flp::mapLegacy(r.format == 1 ? 2 : 1, &none)) << "no slot of the other format here";
}

TEST_P(LangPackConvertTest, ConvertedItReadsAsGermanAgain) {
  flp::LegacySlot old{};
  ASSERT_TRUE(flp::mapLegacy(GetParam().format, &old));
  MemFile file = convert(old);
  flp::unmap();

  // The pack, as the device reads one: header, payload, every entry.
  langpack::Header h;
  ASSERT_TRUE(langpack::parseHeader(file.bytes.data(), file.bytes.size(), h));
  EXPECT_STREQ(h.code, "de");
  EXPECT_STREQ(h.name, "Deutsch");
  EXPECT_EQ(file.bytes.size(), h.extOffset());
  const std::map<uint32_t, std::string> expected = expectedEntries(GetParam());
  ASSERT_GT(expected.size(), 700u);
  EXPECT_EQ(h.entries, expected.size());
  Collected got;
  langpack::EntryParser parser(got);
  std::vector<uint8_t> ring(langpack::WINDOW_BYTES);
  std::vector<uint8_t> io(langpack::IO_BYTES);
  file.at = h.payloadOffset();
  ASSERT_TRUE(langpack::inflateSection({&MemFile::read, &file}, h.payloadLen, h.rawLen, h.payloadCrc, ring.data(),
                                       io.data(), {&ParserSink::write, &parser}));
  EXPECT_EQ(got.values, expected);

  // Into this firmware's slot, through the producer I18n uses: German by this build's StrIds.
  struct Job {
    MemFile* file;
    langpack::Header h;
    static bool produce(void* ctx, flp::StringsWriter& out) {
      auto* job = static_cast<Job*>(ctx);
      job->file->at = job->h.payloadOffset();
      return langpack::inflateStrings({&MemFile::read, job->file}, job->h, KEYS, out);
    }
  } job{&file, h};
  flp::Mapped m{};
  ASSERT_TRUE(flp::acquire(3, langpack::slotStamp(h, i18n_strings::KEY_LAYOUT_STAMP), KEY_COUNT,
                           flp::Strings{h.rawLen, &Job::produce, &job}, nullptr, &m));
  const auto title = static_cast<size_t>(StrId::STR_SETTINGS_TITLE);
  ASSERT_EQ(m.offsets[title] & flp::FALLBACK, 0);
  EXPECT_STREQ(m.data + m.offsets[title], expected.at(langpack::keyHash("STR_SETTINGS_TITLE")).c_str());
  size_t german = 0;
  for (uint16_t id = 0; id < KEY_COUNT; ++id) {
    const auto it = expected.find(KEY_HASHES[id]);
    if (it == expected.end()) {
      EXPECT_EQ(m.offsets[id], flp::FALLBACK) << "a key 2.40 left to English reads as English";
      continue;
    }
    ASSERT_EQ(m.offsets[id] & flp::FALLBACK, 0);
    EXPECT_EQ(std::string(m.data + m.offsets[id]), it->second);
    ++german;
  }
  EXPECT_EQ(german, expected.size());

  // Converted once: with this slot built, the old one is gone, so a pack removed from the card later reads as
  // missing rather than as the old firmware's strings again.
  flp::LegacySlot again{};
  EXPECT_FALSE(flp::mapLegacy(GetParam().format, &again));
}

TEST_P(LangPackConvertTest, ASlotThatDoesNotHoldTogetherIsNotConverted) {
  const Release& r = GetParam();
  flp::LegacySlot old{};
  ASSERT_TRUE(flp::mapLegacy(r.format, &old));
  langpack::OldSlot s = view(old);
  ASSERT_TRUE(langpack::oldSlotSound(s));

  // A key fewer or more than the data holds: another release's layout.
  langpack::OldSlot fewer = s;
  fewer.keyCount = static_cast<uint16_t>(s.keyCount - 1);
  EXPECT_FALSE(langpack::oldSlotSound(fewer));
  langpack::OldSlot cut = s;
  cut.dataLen -= 1;
  EXPECT_FALSE(langpack::oldSlotSound(cut));

  // A table entry that points anywhere but its entry's start.
  std::vector<uint16_t> table(s.table, s.table + s.keyCount);
  langpack::OldSlot moved = s;
  moved.table = table.data();
  for (uint16_t& entry : table) {
    if (!(entry & 0x8000)) {
      entry = static_cast<uint16_t>(entry + 1);
      break;
    }
  }
  EXPECT_FALSE(langpack::oldSlotSound(moved));
  flp::unmap();

  fake_flash::Chip& chip = fake_flash::chip();
  if (r.format == 2) {
    // A format-2 slot proves its strings by their CRC.
    chip.bytes[r.slot + slot.size() - 2] ^= 0x10;
    flp::LegacySlot changed{};
    EXPECT_FALSE(flp::mapLegacy(2, &changed));
    chip.bytes[r.slot + slot.size() - 2] ^= 0x10;
  }
  // A header whose strings run past its slot is not mapped at all.
  const uint32_t tooLong = 128 * 1024;
  memcpy(chip.bytes.data() + r.slot + 20, &tooLong, 4);
  flp::LegacySlot bad{};
  EXPECT_FALSE(flp::mapLegacy(r.format, &bad));
}

INSTANTIATE_TEST_SUITE_P(Releases, LangPackConvertTest, ::testing::ValuesIn(RELEASES),
                         [](const ::testing::TestParamInfo<Release>& info) {
                           return std::string(info.param.format == 1 ? "R240" : "R241RC");
                         });

}  // namespace
