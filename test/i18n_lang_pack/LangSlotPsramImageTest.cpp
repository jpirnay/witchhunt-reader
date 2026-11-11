// Issue #733: an X4 Pro that kept its stock partition table has no `spiffs` partition, so the language slot had nowhere
// to live and the interface stayed English with the pack on the card. The real FlashArea, built as on the S3 boards
// (CONFIG_SPIRAM), over no partition at all (test/flash_lang_partition's in-memory one, empty) takes the
// PSRAM image (test/flash_area_select's esp_heap_caps.h), and the Czech and the Polish pack gen_i18n.py writes build
// their slot in it through the producer I18n uses: every string the pack has, by this build's StrIds.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "FlashArea.h"
#include "FlashLangPartition.h"
#include "I18nStrings.h"
#include "LangPack.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"

namespace {

namespace flp = FlashLangPartition;
using i18n_strings::KEY_HASHES;
using i18n_strings::KEYS_BY_HASH;

constexpr uint16_t KEY_COUNT = static_cast<uint16_t>(StrId::_COUNT);
constexpr langpack::KeyTable KEYS{KEY_HASHES, KEYS_BY_HASH, KEY_COUNT};

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A pack on the card, in memory, read from where it stands.
struct MemFile {
  std::vector<uint8_t> bytes;
  size_t at = 0;
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

struct ParserSink {
  static bool write(void* ctx, const uint8_t* data, const size_t len) {
    return static_cast<langpack::EntryParser*>(ctx)->feed(data, len);
  }
};

// The pack's entries, as its payload holds them.
std::map<uint32_t, std::string> entriesOf(MemFile& file, const langpack::Header& h) {
  Collected got;
  langpack::EntryParser parser(got);
  std::vector<uint8_t> ring(langpack::WINDOW_BYTES);
  std::vector<uint8_t> io(langpack::IO_BYTES);
  file.at = h.payloadOffset();
  EXPECT_TRUE(langpack::inflateSection({&MemFile::read, &file}, h.payloadLen, h.rawLen, h.payloadCrc, ring.data(),
                                       io.data(), {&ParserSink::write, &parser}));
  return got.values;
}

struct Job {
  MemFile* file;
  langpack::Header h;
  int builds = 0;
  static bool produce(void* ctx, flp::StringsWriter& out) {
    auto* job = static_cast<Job*>(ctx);
    ++job->builds;
    job->file->at = job->h.payloadOffset();
    return langpack::inflateStrings({&MemFile::read, job->file}, job->h, KEYS, out);
  }
};

class LangSlotPsramImage : public ::testing::TestWithParam<const char*> {
 protected:
  static void SetUpTestSuite() { fake_flash::reset(0); }
  void TearDown() override { flp::unmap(); }
};

}  // namespace

TEST_P(LangSlotPsramImage, APackOnTheCardBecomesTheInterfaceWithoutThePartition) {
  const std::string dir = GetParam();
  const std::string code = dir == "Language-Czech" ? "cs" : "pl";
  MemFile file{readFile(std::string(LANG_PACKS_DIR) + "/" + dir + "/" + code + ".cplang")};
  langpack::Header h;
  ASSERT_TRUE(langpack::parseHeader(file.bytes.data(), file.bytes.size(), h)) << "gen_i18n.py --packs wrote no " << dir;
  EXPECT_EQ(code, h.code);

  ASSERT_NE(nullptr, FlashArea::get());
  EXPECT_EQ(FlashArea::Backend::PsramImage, FlashArea::backend()) << "no partition: the PSRAM image";

  Job job{&file, h};
  const uint32_t stamp = langpack::slotStamp(h, i18n_strings::KEY_LAYOUT_STAMP);
  const auto lang = static_cast<uint8_t>(code == "cs" ? Language::CS : Language::PL);
  flp::Mapped m{};
  ASSERT_TRUE(flp::acquire(lang, stamp, KEY_COUNT, flp::Strings{h.rawLen, &Job::produce, &job}, nullptr, &m));
  EXPECT_EQ(1, job.builds);

  const std::map<uint32_t, std::string> expected = entriesOf(file, h);
  ASSERT_GT(expected.size(), 700u);
  size_t translated = 0;
  for (uint16_t id = 0; id < KEY_COUNT; ++id) {
    const auto it = expected.find(KEY_HASHES[id]);
    if (it == expected.end()) {
      EXPECT_EQ(m.offsets[id], flp::FALLBACK) << "a key the pack lacks reads as English";
      continue;
    }
    ASSERT_EQ(m.offsets[id] & flp::FALLBACK, 0);
    EXPECT_EQ(std::string(m.data + m.offsets[id]), it->second);
    ++translated;
  }
  EXPECT_EQ(translated, expected.size());
  const auto title = static_cast<size_t>(StrId::STR_SETTINGS_TITLE);
  EXPECT_STREQ(m.data + m.offsets[title], code == "cs" ? "Nastavení" : "Ustawienia");

  flp::Mapped held{};
  ASSERT_TRUE(flp::acquire(lang, stamp, KEY_COUNT, flp::Strings{h.rawLen, &Job::produce, &job}, nullptr, &held));
  EXPECT_EQ(1, job.builds) << "the language setting applied again in the same run reads the slot, not the pack";
}

INSTANTIATE_TEST_SUITE_P(CzechAndPolish, LangSlotPsramImage, ::testing::Values("Language-Czech", "Language-Polish"));
