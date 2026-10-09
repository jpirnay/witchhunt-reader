// SdCardFont's per-page kern matrix against two malformed-or-extreme fonts that the
// .cpfont format admits. Fonts are user-supplied files, so neither is hypothetical:
//  - a class ID past the class count the header declares, which used to send the SD
//    sweep past the matrix (a ~4 GB read into a 4 KB buffer, refused here only
//    because the test file is short);
//  - a page that uses all 255 classes, which wrapped the uint8_t loop counters and
//    never returned.
// The font has glyphs for A-D and U+FFFD only: prewarm() builds the kern matrix from
// the page's codepoints whether or not the font draws them.
#include <Arduino.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "EpdFont.h"
#include "EpdFontData.h"
#include "SdCardFont.h"

namespace {

constexpr uint32_t HEADER = 32;
constexpr uint32_t TOC_ENTRY = 32;
constexpr uint32_t LETTERS = 4;           // A..D
constexpr uint32_t GLYPHS = LETTERS + 1;  // + U+FFFD
constexpr uint32_t INTERVALS = 2;
constexpr uint16_t BITMAP_BYTES = 8;

using ClassTable = std::vector<std::pair<uint16_t, uint8_t>>;  // sorted by codepoint

void putU16(std::vector<uint8_t>& b, const size_t at, const uint16_t v) {
  b[at] = v & 0xFF;
  b[at + 1] = v >> 8;
}
void putU32(std::vector<uint8_t>& b, const size_t at, const uint32_t v) {
  for (int i = 0; i < 4; i++) b[at + i] = (v >> (8 * i)) & 0xFF;
}

// The value stored for an (old) class pair, so a test can check what the mini matrix copied.
int8_t kernValue(const uint32_t left, const uint32_t right) {
  return static_cast<int8_t>(static_cast<int>((left * 7 + right * 3) % 21) - 10);
}

// A one-style v4 .cpfont with the given kern class tables and a leftClasses x rightClasses
// matrix filled from kernValue(). Same layout as test/sd_font_arena's builder.
std::vector<uint8_t> buildFont(const ClassTable& left, const ClassTable& right, const uint8_t leftClasses,
                               const uint8_t rightClasses) {
  const uint32_t dataStart = HEADER + TOC_ENTRY;
  const uint32_t intervalsAt = dataStart;
  const uint32_t glyphsAt = intervalsAt + INTERVALS * sizeof(EpdUnicodeInterval);
  const uint32_t kernLeftAt = glyphsAt + GLYPHS * sizeof(EpdGlyph);
  const uint32_t kernRightAt = kernLeftAt + left.size() * sizeof(EpdKernClassEntry);
  const uint32_t matrixAt = kernRightAt + right.size() * sizeof(EpdKernClassEntry);
  const uint32_t bitmapsAt = matrixAt + static_cast<uint32_t>(leftClasses) * rightClasses;
  std::vector<uint8_t> b(bitmapsAt + GLYPHS * BITMAP_BYTES, 0);

  const char magic[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
  std::copy(magic, magic + 8, b.begin());
  putU16(b, 8, 4);   // version
  putU16(b, 10, 0);  // 1-bit
  b[12] = 1;         // one style

  const size_t toc = HEADER;
  putU32(b, toc + 4, INTERVALS);
  putU32(b, toc + 8, GLYPHS);
  b[toc + 12] = 20;                                // advanceY
  putU16(b, toc + 13, 16);                         // ascender
  putU16(b, toc + 15, static_cast<uint16_t>(-4));  // descender
  putU16(b, toc + 17, static_cast<uint16_t>(left.size()));
  putU16(b, toc + 19, static_cast<uint16_t>(right.size()));
  b[toc + 21] = leftClasses;
  b[toc + 22] = rightClasses;
  putU32(b, toc + 24, dataStart);

  putU32(b, intervalsAt + 0, 'A');
  putU32(b, intervalsAt + 4, 'A' + LETTERS - 1);
  putU32(b, intervalsAt + 8, 0);
  putU32(b, intervalsAt + 12, 0xFFFD);
  putU32(b, intervalsAt + 16, 0xFFFD);
  putU32(b, intervalsAt + 20, LETTERS);

  for (uint32_t g = 0; g < GLYPHS; g++) {
    const size_t at = glyphsAt + g * sizeof(EpdGlyph);
    b[at] = 8;
    b[at + 1] = 8;
    putU16(b, at + 2, 10 << 4);
    putU16(b, at + 6, 8);
    putU16(b, at + 8, BITMAP_BYTES);
    putU32(b, at + 12, g * BITMAP_BYTES);
  }
  for (size_t i = 0; i < left.size(); i++) {
    putU16(b, kernLeftAt + i * 3, left[i].first);
    b[kernLeftAt + i * 3 + 2] = left[i].second;
  }
  for (size_t i = 0; i < right.size(); i++) {
    putU16(b, kernRightAt + i * 3, right[i].first);
    b[kernRightAt + i * 3 + 2] = right[i].second;
  }
  for (uint32_t l = 1; l <= leftClasses; l++) {
    for (uint32_t r = 1; r <= rightClasses; r++) {
      b[matrixAt + (l - 1) * rightClasses + (r - 1)] = static_cast<uint8_t>(kernValue(l, r));
    }
  }
  return b;
}

std::string writeTemp(const std::vector<uint8_t>& bytes) {
  const auto path =
      std::filesystem::temp_directory_path() /
      (std::string("sd_font_kern_") + testing::UnitTest::GetInstance()->current_test_info()->name() + ".cpfont");
  FILE* f = std::fopen(path.string().c_str(), "wb");
  EXPECT_NE(f, nullptr);
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  return path.string();
}

void appendUtf8(std::string& s, const uint32_t cp) {
  if (cp < 0x80) {
    s += static_cast<char>(cp);
  } else if (cp < 0x800) {
    s += static_cast<char>(0xC0 | (cp >> 6));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    s += static_cast<char>(0xE0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

}  // namespace

// 'D' claims left class 7 in a font with two: it must kern as nothing, while A and B
// keep their pairs.
TEST(SdFontKern, ClassIdPastTheMatrixIsUnkerned) {
  const std::string path = writeTemp(buildFont({{'A', 1}, {'B', 2}, {'D', 7}}, {{'B', 1}, {'C', 2}}, 2, 2));
  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  ASSERT_EQ(0, font.prewarm("ABCD", 0x01, /*metadataOnly=*/false, /*loadKernLigatureData=*/true));

  const EpdFont* epd = font.getEpdFont(0);
  ASSERT_NE(nullptr, epd);
  ASSERT_NE(nullptr, epd->data->kernMatrix) << "the kern build failed instead of skipping the bad entry";
  EXPECT_EQ(2u, epd->data->kernLeftEntryCount);
  EXPECT_EQ(2u, epd->data->kernLeftClassCount);
  EXPECT_EQ(kernValue(1, 1), epd->getKerning('A', 'B'));
  EXPECT_EQ(kernValue(2, 2), epd->getKerning('B', 'C'));
  EXPECT_EQ(0, epd->getKerning('D', 'B'));
}

// Every one of the 255 left and right classes on one page. Before the counters were
// widened this never returned; the ctest timeout turns that into a failure.
TEST(SdFontKern, AllTwoHundredFiftyFiveClassesOnOnePage) {
  ClassTable classes;
  std::string page;
  for (uint32_t c = 1; c <= 255; c++) {
    const uint16_t cp = static_cast<uint16_t>(0x100 + c);
    classes.emplace_back(cp, static_cast<uint8_t>(c));
    appendUtf8(page, cp);
  }
  const std::string path = writeTemp(buildFont(classes, classes, 255, 255));
  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  // prewarm() returns how many codepoints have no glyph: all of these, by design.
  EXPECT_EQ(255, font.prewarm(page.c_str(), 0x01, false, true));

  const EpdFont* epd = font.getEpdFont(0);
  ASSERT_NE(nullptr, epd);
  ASSERT_NE(nullptr, epd->data->kernMatrix);
  EXPECT_EQ(255u, epd->data->kernLeftClassCount);
  EXPECT_EQ(255u, epd->data->kernRightClassCount);
  for (const uint32_t l : {1u, 2u, 128u, 254u, 255u}) {
    for (const uint32_t r : {1u, 17u, 200u, 255u}) {
      EXPECT_EQ(kernValue(l, r), epd->getKerning(0x100 + l, 0x100 + r)) << "classes " << l << ", " << r;
    }
  }
}
