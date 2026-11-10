// The UI language packs on the card (lib/I18n/LangPack.h): the key hashes they name strings by, their
// header, and their payload read as the device reads it -- from a pack gen_i18n.py wrote.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "I18nStrings.h"
#include "LangPack.h"

namespace {

using langpack::Header;

constexpr uint16_t KEY_COUNT = static_cast<uint16_t>(StrId::_COUNT);

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> germanPack() { return readFile(std::string(LANG_PACKS_DIR) + "/Language-German/de.cplang"); }

// A section of a file in memory, handed out in pieces of at most `step` bytes, as a card read would.
struct VectorSource {
  const std::vector<uint8_t>* bytes;
  size_t at;
  size_t end;
  size_t step = 512;
  static int read(void* ctx, uint8_t* buf, const size_t len) {
    auto* self = static_cast<VectorSource*>(ctx);
    const size_t n = std::min({len, self->step, self->end - self->at});
    memcpy(buf, self->bytes->data() + self->at, n);
    self->at += n;
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
  langpack::EntryParser* parser;
  static bool write(void* ctx, const uint8_t* data, const size_t len) {
    return static_cast<ParserSink*>(ctx)->parser->feed(data, len);
  }
};

// The payload of `file` through inflateSection() into `parser`, as I18n reads a pack into the slot.
bool inflatePayload(const std::vector<uint8_t>& file, const Header& h, langpack::EntryParser& parser,
                    const size_t step = 512, const uint32_t len = 0) {
  VectorSource src{&file, h.payloadOffset(), h.payloadOffset() + (len ? len : h.payloadLen), step};
  std::vector<uint8_t> ring(langpack::WINDOW_BYTES);
  std::vector<uint8_t> io(langpack::IO_BYTES);
  ParserSink sink{&parser};
  return langpack::inflateSection({&VectorSource::read, &src}, len ? len : h.payloadLen, h.rawLen, h.payloadCrc,
                                  ring.data(), io.data(), {&ParserSink::write, &sink});
}

TEST(LangPack, KeyHashIsTheGeneratorsFnv1a) {
  static_assert(langpack::keyHash("STR_SETTINGS_TITLE") == 0xe88925e1u, "gen_i18n.py key_hash()");
  EXPECT_EQ(langpack::keyHash(""), 0x811c9dc5u);
  // Two names with one hash: why the generator checks every key against every other.
  EXPECT_EQ(langpack::keyHash("STR_T132789"), langpack::keyHash("STR_T729192"));
  EXPECT_EQ(i18n_strings::KEY_HASHES[static_cast<size_t>(StrId::STR_SETTINGS_TITLE)],
            langpack::keyHash("STR_SETTINGS_TITLE"));
}

TEST(LangPack, EveryKeyIsFoundByItsHashAndNoOtherHashIs) {
  for (uint16_t id = 0; id < KEY_COUNT; ++id) {
    ASSERT_EQ(langpack::findKey(i18n_strings::KEY_HASHES[id], i18n_strings::KEY_HASHES, i18n_strings::KEYS_BY_HASH,
                                KEY_COUNT),
              id);
  }
  EXPECT_EQ(langpack::findKey(langpack::keyHash("STR_NO_SUCH_KEY"), i18n_strings::KEY_HASHES,
                              i18n_strings::KEYS_BY_HASH, KEY_COUNT),
            langpack::NO_KEY);
  EXPECT_EQ(langpack::findKey(0, i18n_strings::KEY_HASHES, i18n_strings::KEYS_BY_HASH, 0), langpack::NO_KEY);
}

TEST(LangPack, AHeaderReadsBackAsWritten) {
  Header h;
  h.entries = 849;
  strcpy(h.code, "de");
  h.payloadCrc = 0x12345678u;
  h.payloadLen = 11000;
  h.rawLen = 30000;
  strcpy(h.name, "Deutsch");
  uint8_t bytes[langpack::HEADER_BYTES];
  langpack::encodeHeader(h, bytes);
  Header back;
  ASSERT_TRUE(langpack::parseHeader(bytes, sizeof(bytes), back));
  EXPECT_EQ(back.entries, 849);
  EXPECT_STREQ(back.code, "de");
  EXPECT_EQ(back.payloadCrc, 0x12345678u);
  EXPECT_EQ(back.payloadLen, 11000u);
  EXPECT_EQ(back.rawLen, 30000u);
  EXPECT_EQ(back.extLen, 0u);
  EXPECT_STREQ(back.name, "Deutsch");

  uint8_t bad[langpack::HEADER_BYTES];
  memcpy(bad, bytes, sizeof(bad));
  bad[4] = 2;  // a format this firmware does not read
  EXPECT_FALSE(langpack::parseHeader(bad, sizeof(bad), back));
  memcpy(bad, bytes, sizeof(bad));
  bad[8] = 'D';  // codes are lower case
  EXPECT_FALSE(langpack::parseHeader(bad, sizeof(bad), back));
  memcpy(bad, bytes, sizeof(bad));
  bad[24] = 1;  // an extension that inflates to nothing
  EXPECT_FALSE(langpack::parseHeader(bad, sizeof(bad), back));
  EXPECT_FALSE(langpack::parseHeader(bytes, sizeof(bytes) - 1, back));
}

TEST(LangPack, TheStampFollowsThePackAndTheKeyOrder) {
  Header h;
  h.payloadCrc = 1;
  const uint32_t stamp = langpack::slotStamp(h, 7);
  EXPECT_NE(langpack::slotStamp(h, 8), stamp) << "a firmware with other keys rebuilds";
  Header other = h;
  other.payloadCrc = 2;
  EXPECT_NE(langpack::slotStamp(other, 7), stamp) << "another pack rebuilds";
  other = h;
  other.extCrc = 3;
  EXPECT_NE(langpack::slotStamp(other, 7), stamp) << "other faces rebuild";
  EXPECT_EQ(langpack::slotStamp(h, 7), stamp);
}

TEST(LangPack, TheGeneratedGermanPackReadsAsTheDeviceReadsIt) {
  const std::vector<uint8_t> file = germanPack();
  ASSERT_GT(file.size(), langpack::HEADER_BYTES) << "gen_i18n.py --packs wrote no German pack";
  Header h;
  ASSERT_TRUE(langpack::parseHeader(file.data(), file.size(), h));
  EXPECT_STREQ(h.code, "de");
  EXPECT_STREQ(h.name, "Deutsch");
  EXPECT_EQ(h.payloadOffset() + h.payloadLen + h.extLen, file.size());

  Collected got;
  langpack::EntryParser parser(got);
  ASSERT_TRUE(inflatePayload(file, h, parser));
  EXPECT_TRUE(parser.atBoundary());
  EXPECT_EQ(parser.entries(), h.entries);
  EXPECT_EQ(got.values.size(), h.entries);
  EXPECT_EQ(got.values[langpack::keyHash("STR_SETTINGS_TITLE")], "Einstellungen");
  // Every string the pack names is one of this tree's keys: none is lost to a hash nothing knows.
  for (const auto& [hash, value] : got.values) {
    EXPECT_NE(langpack::findKey(hash, i18n_strings::KEY_HASHES, i18n_strings::KEYS_BY_HASH, KEY_COUNT),
              langpack::NO_KEY);
    EXPECT_FALSE(value.empty()) << "an empty value is left out of the pack";
  }
}

TEST(LangPack, EntriesCutAtEveryByteReadAlike) {
  const std::vector<uint8_t> file = germanPack();
  Header h;
  ASSERT_TRUE(langpack::parseHeader(file.data(), file.size(), h));
  Collected whole;
  Collected byByte;
  langpack::EntryParser wholeParser(whole);
  langpack::EntryParser byteParser(byByte);
  ASSERT_TRUE(inflatePayload(file, h, wholeParser));
  ASSERT_TRUE(inflatePayload(file, h, byteParser, /*step=*/1));
  EXPECT_EQ(whole.values, byByte.values);
}

TEST(LangPack, APackThatIsNotWhatItsHeaderSaysIsRefused) {
  const std::vector<uint8_t> file = germanPack();
  Header h;
  ASSERT_TRUE(langpack::parseHeader(file.data(), file.size(), h));

  std::vector<uint8_t> flipped = file;
  flipped[h.payloadOffset() + h.payloadLen - 3] ^= 0x01;
  Collected a;
  langpack::EntryParser pa(a);
  EXPECT_FALSE(inflatePayload(flipped, h, pa)) << "a byte changed: the CRC differs";

  Collected b;
  langpack::EntryParser pb(b);
  EXPECT_FALSE(inflatePayload(file, h, pb, 512, h.payloadLen - 100)) << "cut short";

  Header longer = h;
  longer.rawLen += 1;
  Collected c;
  langpack::EntryParser pc(c);
  EXPECT_FALSE(inflatePayload(file, longer, pc)) << "inflates to less than it says";
}

}  // namespace
