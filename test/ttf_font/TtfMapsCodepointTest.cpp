// ttf::mapsCodepoint: what a font file covers, read from its table directory and cmap before the font is installed.
// The reader asks it of a book's font on the card: a Chinese, Japanese or Korean book whose font has none of its
// script is drawn in a Noto CJK family instead (SdCardFontSystem::selectLanguageFamily).

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "TtfFace.h"
#include "TtfFontTestUtil.h"

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The whole file read from the card through the page cache, as the reader probes it.
bool mapsOnCard(ttf_test::MemoryFile& file, const uint32_t codepoint, bool* ioError = nullptr) {
  ttf::SdPageCache cache;
  const ttf::SdRange sd{file.source(), 0, 0, static_cast<uint32_t>(file.bytes.size()), &cache};
  FontBytes bytes(nullptr, 0, &sd);
  const bool maps = ttf::mapsCodepoint(bytes, codepoint);
  if (ioError) *ioError = bytes.ioError();
  return maps;
}

TEST(TtfMapsCodepoint, ALatinFontHasNoCjk) {
  ttf_test::MemoryFile file{readFile(TTF_FONT_DIR "/Bookerly/Bookerly-Regular.ttf")};
  ASSERT_FALSE(file.bytes.empty());
  EXPECT_TRUE(mapsOnCard(file, 'A'));
  EXPECT_FALSE(mapsOnCard(file, 0x3042));  // あ
  EXPECT_FALSE(mapsOnCard(file, 0x7684));  // 的
  EXPECT_FALSE(mapsOnCard(file, 0xAC00));  // 가
}

TEST(TtfMapsCodepoint, AJapaneseSubsetHasKanaAndNoHangul) {
  for (const char* name : {"/NotoSansJP-Cff.otf", "/NotoSansJP-TrueType.ttf"}) {
    ttf_test::MemoryFile file{readFile(std::string(TTF_FIXTURE_DIR) + name)};
    ASSERT_FALSE(file.bytes.empty()) << name;
    EXPECT_TRUE(mapsOnCard(file, 0x3042)) << name;   // あ
    EXPECT_TRUE(mapsOnCard(file, 0x65E5)) << name;   // 日
    EXPECT_FALSE(mapsOnCard(file, 0xAC00)) << name;  // 가
  }
}

TEST(TtfMapsCodepoint, AgreesWithTheParsedFace) {
  ttf_test::MemoryFile file{readFile(std::string(TTF_FIXTURE_DIR) + "/NotoSansJP-Cff.otf")};
  FontBytes whole(file.bytes.data(), static_cast<uint32_t>(file.bytes.size()));
  ttf::Face face;
  ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(whole, &face));
  for (uint32_t cp = 0x20; cp < 0x3100; ++cp) {
    ASSERT_EQ(ttf::glyphIndex(whole, face, cp) != 0, mapsOnCard(file, cp)) << std::hex << cp;
  }
}

TEST(TtfMapsCodepoint, AFileThatIsNoFontMapsNothing) {
  ttf_test::MemoryFile file{std::vector<uint8_t>(4096, 0x5A)};
  bool ioError = true;
  EXPECT_FALSE(mapsOnCard(file, 'A', &ioError));
  EXPECT_FALSE(ioError);
}

TEST(TtfMapsCodepoint, AFailedCardReadIsAnIoError) {
  ttf_test::MemoryFile file{readFile(TTF_FONT_DIR "/Bookerly/Bookerly-Regular.ttf")};
  file.failReads = true;
  bool ioError = false;
  EXPECT_FALSE(mapsOnCard(file, 'A', &ioError));
  EXPECT_TRUE(ioError);
}

}  // namespace
