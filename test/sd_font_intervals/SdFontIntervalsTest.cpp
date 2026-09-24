// SdCardFont shares a style's interval table with an earlier style when the two are
// byte-identical, instead of allocating a copy per style. These tests pin the ownership rules
// that make that safe: only the owner frees, a borrower survives unloadMetadata() and is
// re-pointed by reloadMetadata(), and a same-sized but different table is never shared.
// Run under the ASan build to catch a double free or a stale borrowed pointer.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

#include "EpdFontData.h"
#include "SdCardFont.h"

namespace {

// Counts array allocations of exactly one interval table's size. The count is chosen so no
// other allocation in SdCardFont is plausibly the same size.
constexpr uint32_t INTERVALS = 37;
constexpr size_t TABLE_BYTES = INTERVALS * sizeof(EpdUnicodeInterval);
bool countingTables = false;
int tableAllocs = 0;

}  // namespace

void* operator new[](const size_t size) {
  if (countingTables && size == TABLE_BYTES) ++tableAllocs;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](const size_t size, const std::nothrow_t&) noexcept {
  if (countingTables && size == TABLE_BYTES) ++tableAllocs;
  return std::malloc(size ? size : 1);
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {

constexpr uint32_t GLYPHS = INTERVALS * 2;
constexpr uint32_t HEADER = 32;
constexpr uint32_t TOC_ENTRY = 32;

void putU16(std::vector<uint8_t>& b, const size_t at, const uint16_t v) {
  b[at] = v & 0xFF;
  b[at + 1] = v >> 8;
}
void putU32(std::vector<uint8_t>& b, const size_t at, const uint32_t v) {
  for (int i = 0; i < 4; i++) b[at + i] = (v >> (8 * i)) & 0xFF;
}

// A v4 .cpfont with four styles and no kerning or ligatures. Each interval covers two
// codepoints starting at `firstCp[style]` (the last holds U+FFFD), so styles built from the same base have
// byte-identical tables and a style built from another base has the same COUNT but
// different contents.
std::vector<uint8_t> buildFont(const uint32_t (&firstCp)[SdCardFont::MAX_STYLES]) {
  const uint32_t styleBytes = INTERVALS * sizeof(EpdUnicodeInterval) + GLYPHS * sizeof(EpdGlyph);
  const uint32_t dataStart = HEADER + SdCardFont::MAX_STYLES * TOC_ENTRY;
  std::vector<uint8_t> b(dataStart + SdCardFont::MAX_STYLES * styleBytes, 0);

  const char magic[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
  std::copy(magic, magic + 8, b.begin());
  putU16(b, 8, 4);   // version
  putU16(b, 10, 0);  // 1-bit
  b[12] = SdCardFont::MAX_STYLES;

  for (uint8_t s = 0; s < SdCardFont::MAX_STYLES; s++) {
    const size_t toc = HEADER + s * TOC_ENTRY;
    const uint32_t data = dataStart + s * styleBytes;
    b[toc] = s;
    putU32(b, toc + 4, INTERVALS);
    putU32(b, toc + 8, GLYPHS);
    b[toc + 12] = 20;  // advanceY
    putU32(b, toc + 24, data);
    for (uint32_t j = 0; j < INTERVALS; j++) {
      const size_t at = data + j * sizeof(EpdUnicodeInterval);
      // The last interval holds U+FFFD, which prewarm() always adds to the set it resolves.
      const uint32_t first = (j == INTERVALS - 1) ? 0xFFFD : firstCp[s] + 3 * j;
      putU32(b, at, first);
      putU32(b, at + 4, first + 1);
      putU32(b, at + 8, 2 * j);
    }
  }
  return b;
}

std::string writeTemp(const std::vector<uint8_t>& bytes) {
  const auto path = std::filesystem::temp_directory_path() / "sd_font_intervals_test.cpfont";
  FILE* f = std::fopen(path.string().c_str(), "wb");
  EXPECT_NE(f, nullptr);
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  return path.string();
}

// Regular, bold and italic share one table; bold-italic has its own of the same size.
constexpr uint32_t kMixed[SdCardFont::MAX_STYLES] = {'A', 'A', 'A', 'a'};

// Glyphs of style `style` for "A" that the font could not resolve.
int missesForA(SdCardFont& font, const uint8_t style) {
  return font.prewarm("A", static_cast<uint8_t>(1u << style), /*metadataOnly=*/true);
}

class Counting {
 public:
  Counting() {
    tableAllocs = 0;
    countingTables = true;
  }
  ~Counting() { countingTables = false; }
};

TEST(SdFontIntervals, LoadSharesIdenticalTablesOnly) {
  const std::string path = writeTemp(buildFont(kMixed));
  SdCardFont font;
  {
    Counting c;
    ASSERT_TRUE(font.load(path.c_str()));
    EXPECT_EQ(tableAllocs, 2);  // one for styles 0-2, one for style 3
  }
  for (uint8_t s = 0; s < 3; s++) EXPECT_EQ(missesForA(font, s), 0) << "style " << int(s);
  // Same size as the others but a different table: 'A' is not in it. Had it been wrongly
  // aliased to style 0's table, this would resolve.
  EXPECT_EQ(missesForA(font, 3), 1);
}

TEST(SdFontIntervals, UnloadReloadRepointsBorrowers) {
  const std::string path = writeTemp(buildFont(kMixed));
  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  for (int round = 0; round < 3; round++) {
    font.clearCache();
    font.unloadMetadata();
    Counting c;
    ASSERT_TRUE(font.reloadMetadata());
    EXPECT_EQ(tableAllocs, 2) << "round " << round;
  }
  font.clearCache();
  for (uint8_t s = 0; s < 3; s++) EXPECT_EQ(missesForA(font, s), 0) << "style " << int(s);
  EXPECT_EQ(missesForA(font, 3), 1);
}

TEST(SdFontIntervals, ReloadIntoAnotherFontFreesCleanly) {
  const std::string path = writeTemp(buildFont(kMixed));
  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  font.unloadMetadata();
  // load() starts with freeAll() over a font whose borrowers hold no table: must not free twice.
  ASSERT_TRUE(font.load(path.c_str()));
}

TEST(SdFontIntervals, MmapSharesIdenticalTablesOnly) {
  const std::vector<uint8_t> bytes = buildFont(kMixed);
  const std::string path = writeTemp(bytes);
  SdCardFont font;
  {
    Counting c;
    ASSERT_TRUE(font.loadFromMmap(bytes.data(), bytes.size(), path.c_str()));
    EXPECT_EQ(tableAllocs, 2);
  }
  // An mmap font's metadata prewarm wires each style straight to its full table, so the
  // sharing is visible as pointer identity.
  ASSERT_EQ(font.prewarm("A", 0x0F, /*metadataOnly=*/true), 0);
  const EpdUnicodeInterval* regular = font.getEpdFont(0)->data->intervals;
  ASSERT_NE(regular, nullptr);
  EXPECT_EQ(font.getEpdFont(1)->data->intervals, regular);
  EXPECT_EQ(font.getEpdFont(2)->data->intervals, regular);
  EXPECT_NE(font.getEpdFont(3)->data->intervals, regular);
  EXPECT_EQ(font.getEpdFont(3)->data->intervals[0].first, static_cast<uint32_t>('a'));
}

TEST(SdFontIntervals, AllDifferentSharesNothing) {
  constexpr uint32_t kDistinct[SdCardFont::MAX_STYLES] = {'A', 'a', 0x100, 0x200};
  const std::string path = writeTemp(buildFont(kDistinct));
  SdCardFont font;
  Counting c;
  ASSERT_TRUE(font.load(path.c_str()));
  EXPECT_EQ(tableAllocs, 4);
}

}  // namespace
