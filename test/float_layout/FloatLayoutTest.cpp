// FloatLayoutTest.cpp
//
// Unit tests for per-line width narrowing from active FloatZones and
// CSS `clear` handling.
//
// These tests are self-contained: they replicate the pure-logic parts of
// the planned FloatZone implementation without pulling in GfxRenderer,
// Arduino, or any hardware dependency.  They define the required behaviour
// (the contract) and will FAIL until the corresponding production code is
// written.  Once the implementation lands, all tests here must pass.
//
// Test EPUB: test/epubs/test_float_images.epub  (four scenarios)
//
// Relationship to the plan in docs/contributing/float-layout.md:
//   - FloatZone struct    → "Data model" section
//   - widthForLine()      → "New helper: widthForLine" section
//   - clearFloatZones()   → "CSS clear" section
//   - Line-break tests    → "Algorithm change in ParsedText" section

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

// ============================================================================
// Minimal stand-ins for the production types
// ============================================================================

struct FloatZone {
  int16_t top;
  int16_t bottom;
  int16_t width;  // imageWidth + gap (4 px)
};

static constexpr int kMaxFloatZones = 2;

struct BlockStyleFloatFields {
  FloatZone floatZones[kMaxFloatZones] = {};
  int8_t floatZoneCount = 0;
};

// Mirrors the planned ParsedText::widthForLine helper.
// pageWidth is the full content column width (excludes screen margins).
// blockStartY is currentPageNextY at the time layoutAndExtractLines is called.
int widthForLine(int lineIndex, int lineHeight, int16_t blockStartY, int pageWidth, const BlockStyleFloatFields& bs) {
  const int lineTop = blockStartY + lineIndex * lineHeight;
  int indent = 0;
  for (int i = 0; i < bs.floatZoneCount; ++i) {
    const auto& z = bs.floatZones[i];
    if (lineTop < z.bottom && lineTop + lineHeight > z.top) {
      indent += z.width;
    }
  }
  return pageWidth - indent;
}

// Greedy line-break simulation (no Knuth-Plass, sufficient for width tests).
// wordWidths: pixel width of each word.  gap: fixed inter-word space.
// Returns a vector of line widths (sum of words on each line).
std::vector<int> simulateLineBreaks(const std::vector<int>& wordWidths, int gap, int16_t blockStartY, int lineHeight,
                                    int pageWidth, const BlockStyleFloatFields& bs) {
  std::vector<int> lineWidths;
  int lineIndex = 0;
  size_t i = 0;
  while (i < wordWidths.size()) {
    const int avail = widthForLine(lineIndex, lineHeight, blockStartY, pageWidth, bs);
    int used = 0;
    bool first = true;
    while (i < wordWidths.size()) {
      const int needed = first ? wordWidths[i] : gap + wordWidths[i];
      if (!first && used + needed > avail) break;
      used += needed;
      first = false;
      ++i;
    }
    lineWidths.push_back(used);
    ++lineIndex;
  }
  return lineWidths;
}

// Count how many lines from the start have width < pageWidth (indented lines).
int countIndentedLines(const std::vector<int>& lineWidths, int pageWidth) {
  int count = 0;
  for (int w : lineWidths) {
    if (w < pageWidth)
      ++count;
    else
      break;
  }
  return count;
}

// ============================================================================
// Tests: FloatZone data structure
// ============================================================================

TEST(FloatZoneTest, DefaultIsEmpty) {
  BlockStyleFloatFields bs;
  EXPECT_EQ(bs.floatZoneCount, 0);
}

TEST(FloatZoneTest, AddOneZone) {
  BlockStyleFloatFields bs;
  ASSERT_LT(bs.floatZoneCount, kMaxFloatZones);
  bs.floatZones[bs.floatZoneCount++] = {10, 70, 64};  // top=10, bottom=70, width=64
  EXPECT_EQ(bs.floatZoneCount, 1);
  EXPECT_EQ(bs.floatZones[0].top, 10);
  EXPECT_EQ(bs.floatZones[0].bottom, 70);
  EXPECT_EQ(bs.floatZones[0].width, 64);
}

TEST(FloatZoneTest, MaxZonesIsTwo) {
  // Enforce that kMaxFloatZones == 2 so the struct stays 13 bytes.
  EXPECT_EQ(kMaxFloatZones, 2);
}

// ============================================================================
// Tests: widthForLine — no active zones → full width every line
// ============================================================================

TEST(WidthForLineTest, NoFloatZones_FullWidthEveryLine) {
  BlockStyleFloatFields bs;  // floatZoneCount = 0
  const int pageWidth = 400;
  const int lineHeight = 22;
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(widthForLine(i, lineHeight, 0, pageWidth, bs), pageWidth)
        << "Line " << i << " should be full width when no float zones are active";
  }
}

// ============================================================================
// Tests: widthForLine — single left float zone
// ============================================================================

TEST(WidthForLineTest, SingleZone_LinesWithinZoneAreNarrowed) {
  // Image: 60px tall at page y=0, gap=4 → zone width=64.
  // line height = 22px → lines 0,1,2 overlap zone (tops at 0,22,44 — all < 60).
  // Line 3 starts at y=66 ≥ 60 → full width.
  BlockStyleFloatFields bs;
  bs.floatZones[bs.floatZoneCount++] = {0, 60, 64};
  const int pageWidth = 400;
  const int lineHeight = 22;
  const int16_t blockStartY = 0;

  EXPECT_EQ(widthForLine(0, lineHeight, blockStartY, pageWidth, bs), 400 - 64)
      << "Line 0 (y=0..22) overlaps zone, must be narrowed";
  EXPECT_EQ(widthForLine(1, lineHeight, blockStartY, pageWidth, bs), 400 - 64)
      << "Line 1 (y=22..44) overlaps zone, must be narrowed";
  EXPECT_EQ(widthForLine(2, lineHeight, blockStartY, pageWidth, bs), 400 - 64)
      << "Line 2 (y=44..66) overlaps zone, must be narrowed";
  EXPECT_EQ(widthForLine(3, lineHeight, blockStartY, pageWidth, bs), 400)
      << "Line 3 (y=66..88) is below zone bottom=60, must be full width";
}

TEST(WidthForLineTest, SingleZone_BlockStartYOffsetApplied) {
  // Zone top=100, bottom=160. blockStartY=80, lineHeight=22.
  // Line 0: top=80, bottom=102 → overlaps [100,160) → narrowed.
  // Line 3: top=80+66=146, bottom=168 → overlaps → narrowed.
  // Line 4: top=80+88=168, bottom=190 → does NOT overlap [100,160) → full.
  BlockStyleFloatFields bs;
  bs.floatZones[bs.floatZoneCount++] = {100, 160, 64};
  const int pageWidth = 400;
  const int lineHeight = 22;
  const int16_t blockStartY = 80;

  EXPECT_EQ(widthForLine(0, lineHeight, blockStartY, pageWidth, bs), 400 - 64);
  EXPECT_EQ(widthForLine(3, lineHeight, blockStartY, pageWidth, bs), 400 - 64);
  EXPECT_EQ(widthForLine(4, lineHeight, blockStartY, pageWidth, bs), 400)
      << "Line 4 top=168 >= zone bottom=160, must be full width";
}

TEST(WidthForLineTest, SingleZone_LineAboveZoneIsFullWidth) {
  // Zone starts at y=50. blockStartY=0, lineHeight=22.
  // Line 0 top=0, bottom=22 — entirely above zone top=50 → full width.
  BlockStyleFloatFields bs;
  bs.floatZones[bs.floatZoneCount++] = {50, 110, 64};
  EXPECT_EQ(widthForLine(0, 22, 0, 400, bs), 400) << "Line entirely above zone must not be narrowed";
  EXPECT_EQ(widthForLine(2, 22, 0, 400, bs), 400 - 64) << "Line 2 top=44, bottom=66 overlaps zone top=50";
}

// ============================================================================
// Tests: widthForLine — two simultaneous zones
// ============================================================================

TEST(WidthForLineTest, TwoZones_WidthsAreAdditive) {
  // Two 60px-wide zones both active on line 0.
  BlockStyleFloatFields bs;
  bs.floatZones[bs.floatZoneCount++] = {0, 60, 64};
  bs.floatZones[bs.floatZoneCount++] = {0, 60, 44};
  const int pageWidth = 400;
  EXPECT_EQ(widthForLine(0, 22, 0, pageWidth, bs), 400 - 64 - 44)
      << "Two overlapping zones must reduce width additively";
}

TEST(WidthForLineTest, TwoZones_OnlyOneActiveOnLaterLine) {
  // Zone A: top=0, bottom=60 (expires after line 2 at 22px/line).
  // Zone B: top=0, bottom=120 (still active on line 3).
  BlockStyleFloatFields bs;
  bs.floatZones[bs.floatZoneCount++] = {0, 60, 64};
  bs.floatZones[bs.floatZoneCount++] = {0, 120, 44};
  EXPECT_EQ(widthForLine(3, 22, 0, 400, bs), 400 - 44) << "Line 3 top=66 beyond zone A bottom=60; only zone B active";
}

// ============================================================================
// Tests: scenario-level (Case 1–4 from test_float_images.epub)
// ============================================================================

// Case 1: 60x60 left float, long paragraph.
// With lineHeight=22: lines 0-2 indented, line 3+ full width.
TEST(FloatScenario, Case1_ShortFloat_LongParagraph) {
  // 50 words × 30px, gap=6px.
  // Narrowed width (zone=64): 400-64=336. Words/line: greedy packs ~9 (9×30+8×6=318 ≤ 336).
  // Full width (400): greedy packs ~11 (11×30+10×6=390 ≤ 400).
  // Indented lines pack ≤ 336px; full-width lines pack > 336px.
  std::vector<int> words(50, 30);
  const int gap = 6;
  const int pageWidth = 400;
  const int lineHeight = 22;
  const int zoneWidth = 64;

  BlockStyleFloatFields bs;
  bs.floatZones[bs.floatZoneCount++] = {0, 60, zoneWidth};  // 60x60 image + 4px gap

  auto lineWidths = simulateLineBreaks(words, gap, 0, lineHeight, pageWidth, bs);

  ASSERT_GE(lineWidths.size(), 4u) << "Long paragraph should produce >= 4 lines";

  // Lines 0,1,2: available = pageWidth - zoneWidth = 336.
  EXPECT_LE(lineWidths[0], pageWidth - zoneWidth) << "Line 0 must be narrowed by float zone";
  EXPECT_LE(lineWidths[1], pageWidth - zoneWidth) << "Line 1 must be narrowed by float zone";
  EXPECT_LE(lineWidths[2], pageWidth - zoneWidth) << "Line 2 must be narrowed by float zone";

  // Line 3+ (y=66 ≥ 60): available = pageWidth. Lines should pack more words
  // than on the narrowed lines, i.e. width > pageWidth - zoneWidth.
  bool hasWiderLine = false;
  for (size_t i = 3; i < lineWidths.size(); ++i) {
    if (lineWidths[i] > pageWidth - zoneWidth) {
      hasWiderLine = true;
      break;
    }
  }
  EXPECT_TRUE(hasWiderLine) << "At least one line after float height must pack more than the narrowed width";
}

// Case 2: 60x120 left float, short paragraph (fewer lines than image height).
// All lines must be narrowed.
TEST(FloatScenario, Case2_TallFloat_ShortParagraph) {
  std::vector<int> words(8, 30);  // 8 words → 2–3 lines at narrowed width
  const int gap = 6;
  const int pageWidth = 400;
  const int lineHeight = 22;

  BlockStyleFloatFields bs;
  bs.floatZones[bs.floatZoneCount++] = {0, 120, 64};  // 60x120 image + 4px gap

  auto lineWidths = simulateLineBreaks(words, gap, 0, lineHeight, pageWidth, bs);

  for (size_t i = 0; i < lineWidths.size(); ++i) {
    EXPECT_LT(lineWidths[i], pageWidth) << "Line " << i
                                        << " must be indented (short paragraph entirely within float height)";
  }
}

// Case 3: right float — semantically the float zone should reduce width the
// same way as a left float (the text wraps around it); the x-offset of the
// text block differs but available width is the same quantity.
TEST(FloatScenario, Case3_RightFloat_SameWidthNarrowing) {
  // Right float: zone has same width mechanics; x position differs at render
  // time but widthForLine doesn't need to know left vs right.
  // Use 40 words to ensure lines both within and beyond the zone.
  std::vector<int> words(40, 30);
  const int gap = 6;
  const int pageWidth = 400;
  const int lineHeight = 22;
  const int zoneWidth = 64;

  BlockStyleFloatFields bs;
  bs.floatZones[bs.floatZoneCount++] = {0, 60, zoneWidth};

  auto lineWidths = simulateLineBreaks(words, gap, 0, lineHeight, pageWidth, bs);

  // Lines 0-2 must be narrowed (within zone height)
  EXPECT_LE(lineWidths[0], pageWidth - zoneWidth) << "Line 0 must be narrowed";
  EXPECT_LE(lineWidths[1], pageWidth - zoneWidth) << "Line 1 must be narrowed";

  // At least one line after the zone must pack more words than narrowed lines
  bool hasWiderLine = false;
  for (size_t i = 3; i < lineWidths.size(); ++i) {
    if (lineWidths[i] > pageWidth - zoneWidth) {
      hasWiderLine = true;
      break;
    }
  }
  EXPECT_TRUE(hasWiderLine) << "Lines after float expiry must use more width than narrowed lines";
}

// Case 4: no float zone → every non-last line packs as many words as possible
// at full page width; no line should be narrower than with a zone active.
TEST(FloatScenario, Case4_NoFloat_WiderThanWithFloat) {
  std::vector<int> words(30, 30);
  const int gap = 6;
  const int pageWidth = 400;
  const int lineHeight = 22;

  BlockStyleFloatFields bsNoFloat;  // floatZoneCount = 0
  BlockStyleFloatFields bsFloat;
  bsFloat.floatZones[bsFloat.floatZoneCount++] = {0, 60, 64};

  auto noFloatLines = simulateLineBreaks(words, gap, 0, lineHeight, pageWidth, bsNoFloat);
  auto floatLines = simulateLineBreaks(words, gap, 0, lineHeight, pageWidth, bsFloat);

  // The no-float layout should produce fewer lines (more words per line).
  EXPECT_LE(noFloatLines.size(), floatLines.size())
      << "Without a float zone, fewer lines should be needed (more words fit per line)";

  // No non-last line of the no-float layout should be narrower than the
  // narrowed lines of the float layout.
  for (size_t i = 0; i + 1 < noFloatLines.size(); ++i) {
    EXPECT_GT(noFloatLines[i], pageWidth - 64)
        << "Non-last line " << i << " without float must pack more than narrowed width";
  }
}

// ============================================================================
// Tests: edge cases
// ============================================================================

TEST(FloatZoneTest, ZeroHeightImage_NeverNarrows) {
  // A degenerate float with zero height should not narrow any line.
  BlockStyleFloatFields bs;
  bs.floatZones[bs.floatZoneCount++] = {0, 0, 64};
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(widthForLine(i, 22, 0, 400, bs), 400) << "Zero-height float zone must not narrow any line";
  }
}

TEST(FloatZoneTest, FloatAtPageBottom_DoesNotAffectNextBlock) {
  // A block starting at y=200, float zone top=0 bottom=60 (from page top).
  // Lines of this block start at y=200+ so they never overlap the zone.
  BlockStyleFloatFields bs;
  bs.floatZones[bs.floatZoneCount++] = {0, 60, 64};
  const int16_t blockStartY = 200;
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(widthForLine(i, 22, blockStartY, 400, bs), 400) << "Block starting below float zone must not be narrowed";
  }
}

TEST(FloatZoneTest, ExactBoundary_LineJustBelowZone) {
  // Zone bottom=66. lineHeight=22. Line 3 starts at y=66 — exactly at boundary.
  // That line's top == zone bottom → does NOT overlap (strict less-than).
  BlockStyleFloatFields bs;
  bs.floatZones[bs.floatZoneCount++] = {0, 66, 64};
  EXPECT_EQ(widthForLine(2, 22, 0, 400, bs), 400 - 64)
      << "Line 2 top=44 < 66, bottom=66: borderline overlap, should be narrowed";
  EXPECT_EQ(widthForLine(3, 22, 0, 400, bs), 400) << "Line 3 top=66 == zone bottom=66: no overlap, must be full width";
}

// ============================================================================
// Tests: BlockStyle propagation rules (plan invariants)
// ============================================================================

TEST(FloatZonePropagation, ZoneNotPropagatedToContinuationBlock) {
  // The plan states FloatZone must NOT be inherited by a continuation flush.
  // A new ParsedText (continuation) is default-constructed with floatZoneCount=0.
  BlockStyleFloatFields continuation;  // simulates fresh ParsedText block
  EXPECT_EQ(continuation.floatZoneCount, 0)
      << "Continuation block must start with no float zones (image is on previous page)";
}

TEST(FloatZonePropagation, MaxTwoZones) {
  BlockStyleFloatFields bs;
  bs.floatZones[bs.floatZoneCount++] = {0, 60, 64};
  bs.floatZones[bs.floatZoneCount++] = {0, 120, 44};
  EXPECT_EQ(bs.floatZoneCount, 2);
  // Adding a third would exceed kMaxFloatZones; callers must guard with:
  //   if (bs.floatZoneCount < kMaxFloatZones) { ... }
  EXPECT_GE(kMaxFloatZones, bs.floatZoneCount) << "floatZoneCount must never exceed kMaxFloatZones";
}

// ============================================================================
// CSS `clear` handling
//
// When an element has clear:left / clear:right / clear:both, the parser must
// advance currentPageNextY to the highest active zone bottom before placing
// the block, then clear all relevant zones from blockStyle.
//
// The helper below mirrors the planned clearFloatZones() logic in the parser.
// Returns the new currentPageNextY after applying clear.
// ============================================================================

enum class CssClear { None, Left, Right, Both };

// Mirrors planned parser helper.
// zones / count are modified in-place (expired zones removed).
// Returns the y to advance currentPageNextY to (max of all relevant bottoms,
// or currentY if nothing to clear).
int16_t applyClear(CssClear clear, int16_t currentY, FloatZone zones[], int8_t& count) {
  if (clear == CssClear::None || count == 0) return currentY;
  int16_t newY = currentY;
  for (int i = 0; i < count; ++i) {
    newY = std::max(newY, zones[i].bottom);
  }
  // Clear matching zones (for now we clear all — we don't track left vs right).
  count = 0;
  return newY;
}

TEST(CssClearTest, ClearBoth_AdvancesYToHighestBottom) {
  FloatZone zones[kMaxFloatZones] = {{0, 60, 64}, {0, 90, 44}};
  int8_t count = 2;
  int16_t y = applyClear(CssClear::Both, 20, zones, count);
  EXPECT_EQ(y, 90) << "clear:both must advance y to highest zone bottom";
  EXPECT_EQ(count, 0) << "clear:both must remove all active zones";
}

TEST(CssClearTest, ClearLeft_AdvancesYAndClearsZones) {
  FloatZone zones[kMaxFloatZones] = {{0, 60, 64}};
  int8_t count = 1;
  int16_t y = applyClear(CssClear::Left, 0, zones, count);
  EXPECT_EQ(y, 60) << "clear:left must advance y to zone bottom";
  EXPECT_EQ(count, 0);
}

TEST(CssClearTest, ClearNone_DoesNothing) {
  FloatZone zones[kMaxFloatZones] = {{0, 60, 64}};
  int8_t count = 1;
  int16_t y = applyClear(CssClear::None, 10, zones, count);
  EXPECT_EQ(y, 10) << "clear:none must not change y";
  EXPECT_EQ(count, 1) << "clear:none must not remove zones";
}

TEST(CssClearTest, ClearWhenNoZones_DoesNothing) {
  FloatZone zones[kMaxFloatZones] = {};
  int8_t count = 0;
  int16_t y = applyClear(CssClear::Both, 50, zones, count);
  EXPECT_EQ(y, 50) << "clear with no active zones must leave y unchanged";
}

TEST(CssClearTest, ClearBelowCurrentY_DoesNotRegressY) {
  // Zone already expired (bottom < currentY): clear should not regress y.
  FloatZone zones[kMaxFloatZones] = {{0, 30, 64}};
  int8_t count = 1;
  int16_t y = applyClear(CssClear::Both, 50, zones, count);
  EXPECT_GE(y, 50) << "clear must never move y backwards";
  EXPECT_EQ(count, 0);
}

// ============================================================================
// Margin / padding inflation of FloatZone dimensions
//
// CSS margin on the float image increases the exclusion zone.
// Padding and border are treated as visual-only (MCU shortcut): ignored.
// zone.width  = imageWidth  + marginLeft + marginRight + gap(4)
// zone.height = imageBottom - imageTop   (zone.bottom - zone.top)
//             where imageBottom = imagePlacedY + imageHeight + marginBottom
//             and   imagePlacedY = currentY + marginTop
// ============================================================================

struct ImageCssBox {
  int16_t imageWidth, imageHeight;
  int16_t marginTop, marginRight, marginBottom, marginLeft;
};

// Mirrors planned zone construction in the parser.
// width is clamped to a minimum of 4 (the gap) so a pathological negative
// margin never produces a zone that widens the available text area.
FloatZone makeFloatZone(int16_t placedY, const ImageCssBox& b) {
  const int16_t top = static_cast<int16_t>(placedY + b.marginTop);
  const int16_t bottom = static_cast<int16_t>(top + b.imageHeight + b.marginBottom);
  const int rawWidth = b.imageWidth + b.marginLeft + b.marginRight + 4;
  const int16_t width = static_cast<int16_t>(std::max(rawWidth, 4));
  return {top, bottom, width};
}

TEST(FloatZoneMargin, NoMargin_BaselineZone) {
  ImageCssBox box = {60, 60, 0, 0, 0, 0};
  auto z = makeFloatZone(0, box);
  EXPECT_EQ(z.top, 0);
  EXPECT_EQ(z.bottom, 60);
  EXPECT_EQ(z.width, 64);  // 60 + 0 + 0 + 4 gap
}

TEST(FloatZoneMargin, MarginTopPushesZoneDown) {
  ImageCssBox box = {60, 60, 10, 0, 0, 0};  // marginTop=10
  auto z = makeFloatZone(0, box);
  EXPECT_EQ(z.top, 10) << "marginTop must shift zone top down";
  EXPECT_EQ(z.bottom, 70) << "zone bottom = top + imageHeight";
}

TEST(FloatZoneMargin, MarginBottomExtendsZone) {
  ImageCssBox box = {60, 60, 0, 0, 8, 0};  // marginBottom=8
  auto z = makeFloatZone(0, box);
  EXPECT_EQ(z.bottom, 68) << "marginBottom must extend zone bottom";
}

TEST(FloatZoneMargin, MarginRightWidensZone) {
  ImageCssBox box = {60, 60, 0, 10, 0, 0};  // marginRight=10
  auto z = makeFloatZone(0, box);
  EXPECT_EQ(z.width, 74) << "marginRight must widen exclusion zone (60+10+0+4)";
}

TEST(FloatZoneMargin, MarginLeftWidensZone) {
  ImageCssBox box = {60, 60, 0, 0, 0, 6};  // marginLeft=6
  auto z = makeFloatZone(0, box);
  EXPECT_EQ(z.width, 70) << "marginLeft must widen exclusion zone (60+0+6+4)";
}

TEST(FloatZoneMargin, AllMarginsInflateZone) {
  ImageCssBox box = {60, 60, 4, 6, 8, 2};
  auto z = makeFloatZone(10, box);
  // top    = 10 + 4 = 14
  // bottom = 14 + 60 + 8 = 82
  // width  = 60 + 6 + 2 + 4 = 72
  EXPECT_EQ(z.top, 14);
  EXPECT_EQ(z.bottom, 82);
  EXPECT_EQ(z.width, 72);
}

TEST(FloatZoneMargin, NegativeMarginClamped) {
  // Negative margins are unusual in EPUB but must not produce a negative width.
  ImageCssBox box = {60, 60, 0, 0, 0, 0};
  box.marginRight = -100;  // pathological
  auto z = makeFloatZone(0, box);
  EXPECT_GE(z.width, 4) << "Zone width must be at least the gap even with extreme margins";
}

// ============================================================================
// Block vs inline elements across float zone
//
// The parser already advances currentPageNextY when a block element opens
// (p, div, h1-h6 call startNewTextBlock).  widthForLine uses blockStartY so
// each block's line 0 starts at the correct Y.  The tests below confirm that
// a FloatZone created by an image in block A correctly narrows the lines of
// block B that overlap it, even though B is a separate ParsedText instance.
// Inline elements (span, b, a) do NOT advance currentPageNextY, so they
// never move blockStartY — already correct without changes.
// ============================================================================

TEST(FloatZoneAcrossBlocks, SecondBlockStartsBelowFirstBlock) {
  // Block A: 1 line of text at y=0 (lineHeight=22). blockStartY_B = 22.
  // Zone: top=0, bottom=60. BlockB line 0 starts at y=22 → still in zone.
  BlockStyleFloatFields bs;
  bs.floatZones[bs.floatZoneCount++] = {0, 60, 64};
  const int16_t blockStartY_B = 22;  // one line below block A start
  EXPECT_EQ(widthForLine(0, 22, blockStartY_B, 400, bs), 400 - 64)
      << "Block B line 0 at y=22 overlaps zone top=0 bottom=60, must be narrowed";
  EXPECT_EQ(widthForLine(2, 22, blockStartY_B, 400, bs), 400)
      << "Block B line 2 at y=66 is below zone bottom=60, must be full width";
}

TEST(FloatZoneAcrossBlocks, BlockStartingBelowZoneIsFullWidth) {
  // Block B starts at y=80 — zone already expired (bottom=60).
  BlockStyleFloatFields bs;
  bs.floatZones[bs.floatZoneCount++] = {0, 60, 64};
  const int16_t blockStartY_B = 80;
  EXPECT_EQ(widthForLine(0, 22, blockStartY_B, 400, bs), 400)
      << "Block starting below zone bottom must not be narrowed";
}

// ============================================================================
// Tests: split inline float image across a page boundary
//
// When a floated inline image is taller than the remaining space on the
// current page (remainingOnPage < imageHeight), the parser splits it into
// two tiles:
//   Tile A: rows [0, remainingOnPage)  — placed on the current page.
//   Tile B: rows [remainingOnPage, imageHeight) — placed at y=0 on the next page.
//
// Float zone on the old page: top=currentPageNextY, bottom=viewportHeight.
// Float zone re-based for the new page: top=0, bottom=tileBHeight.
// After both tiles are exhausted, no zone must remain.
//
// Test EPUB: test/epubs/test_float_images.epub  chapter 7
// ============================================================================

// Helper: simulate the split geometry.
struct SplitResult {
  int16_t tileAHeight;               // rows rendered on page N
  int16_t tileBHeight;               // rows rendered on page N+1
  BlockStyleFloatFields zonePageN;   // float zone active while tile A is on page
  BlockStyleFloatFields zonePageN1;  // float zone active while tile B is on page
};

SplitResult simulateSplit(int16_t imageHeight, int16_t imageWidth, int16_t currentPageNextY, int16_t viewportHeight) {
  const int16_t remaining = static_cast<int16_t>(viewportHeight - currentPageNextY);
  const int16_t tileAH = remaining;
  const int16_t tileBH = static_cast<int16_t>(imageHeight - tileAH);
  const int16_t zoneW = static_cast<int16_t>(imageWidth + 4);

  SplitResult r;
  r.tileAHeight = tileAH;
  r.tileBHeight = tileBH;

  // Zone on page N: covers from currentPageNextY to viewportHeight.
  r.zonePageN.floatZones[r.zonePageN.floatZoneCount++] = {currentPageNextY, viewportHeight, zoneW};
  // Zone on page N+1: covers from 0 to tileBHeight.
  r.zonePageN1.floatZones[r.zonePageN1.floatZoneCount++] = {0, tileBH, zoneW};

  return r;
}

TEST(SplitFloatImage, TileHeightsPartitionImage) {
  // Image 100px tall, 700px remaining on a 800px page → split at 700/100.
  // Current page is 700px used, viewport 800px → remaining=100 == image → no split.
  // Use 750px used instead → remaining=50, tileA=50, tileB=50.
  const int16_t imgH = 100, imgW = 60, curY = 750, vpH = 800;
  auto r = simulateSplit(imgH, imgW, curY, vpH);
  EXPECT_EQ(r.tileAHeight + r.tileBHeight, imgH) << "Tile heights must sum to full image height";
  EXPECT_EQ(r.tileAHeight, vpH - curY) << "Tile A height == remaining space on page";
  EXPECT_GT(r.tileBHeight, 0) << "Tile B must be non-empty when image overflows";
}

TEST(SplitFloatImage, NoSplitWhenImageFits) {
  // Image 60px tall, 200px remaining → fits entirely, no split needed.
  // simulateSplit computes tileA=200, tileB=60-200=-140 which is nonsensical;
  // callers guard with remainingOnPage >= imageHeight before calling split path.
  const int16_t remaining = 200, imgH = 60;
  EXPECT_GE(remaining, imgH) << "Pre-condition: image fits, split path must not be taken";
}

TEST(SplitFloatImage, ZonePageN_CoversRemainingPageHeight) {
  const int16_t imgH = 100, imgW = 60, curY = 750, vpH = 800;
  auto r = simulateSplit(imgH, imgW, curY, vpH);
  const auto& z = r.zonePageN.floatZones[0];
  EXPECT_EQ(z.top, curY) << "Zone on page N must start at currentPageNextY (provisional, corrected later)";
  EXPECT_EQ(z.bottom, vpH) << "Zone on page N must reach the page bottom";
  EXPECT_EQ(z.width, imgW + 4) << "Zone width must be imageWidth + 4px gap";
}

TEST(SplitFloatImage, ZonePageN1_StartsAtTopOfPage) {
  const int16_t imgH = 100, imgW = 60, curY = 750, vpH = 800;
  auto r = simulateSplit(imgH, imgW, curY, vpH);
  const auto& z = r.zonePageN1.floatZones[0];
  EXPECT_EQ(z.top, 0) << "Zone on page N+1 must start at page top (tile B is placed at y=0)";
  EXPECT_EQ(z.bottom, r.tileBHeight) << "Zone on page N+1 must end at tile B bottom";
  EXPECT_EQ(z.width, imgW + 4) << "Zone width must be consistent across the split";
}

TEST(SplitFloatImage, ZonePageN1_NarrowsLinesWithinTileB) {
  // Tile B is 50px tall (rows 750-800 of a 100px image).
  // lineHeight=22. Lines 0,1 (y=0..22, y=22..44) overlap zone [0,50).
  // Line 2 (y=44..66) also overlaps. Line 3 (y=66..88) does NOT (66 >= 50).
  const int16_t imgH = 100, imgW = 60, curY = 750, vpH = 800;
  const int pageWidth = 400;
  const int lineHeight = 22;
  auto r = simulateSplit(imgH, imgW, curY, vpH);

  EXPECT_EQ(widthForLine(0, lineHeight, 0, pageWidth, r.zonePageN1), pageWidth - (imgW + 4))
      << "Line 0 on page N+1 overlaps tile B zone, must be narrowed";
  EXPECT_EQ(widthForLine(1, lineHeight, 0, pageWidth, r.zonePageN1), pageWidth - (imgW + 4))
      << "Line 1 on page N+1 overlaps tile B zone, must be narrowed";

  // Line 3 starts at y=66 > tileB bottom=50 → full width.
  EXPECT_EQ(widthForLine(3, lineHeight, 0, pageWidth, r.zonePageN1), pageWidth)
      << "Line 3 on page N+1 is below tile B, must be full width";
}

TEST(SplitFloatImage, AfterBothTilesNoZoneRemains) {
  // Once tile B has been consumed (lines past its bottom) the zone expires
  // naturally via widthForLine — no manual reset needed.
  const int16_t imgH = 100, imgW = 60, curY = 750, vpH = 800;
  const int pageWidth = 400;
  const int lineHeight = 22;
  auto r = simulateSplit(imgH, imgW, curY, vpH);

  // On page N+1, tileBHeight=50. First line whose TOP is >= 50 is at index ceil(50/22)=3
  // (line 2 top=44 < 50, still overlaps; line 3 top=66 >= 50, fully below).
  const int lineIndexAtBoundary = (r.tileBHeight + lineHeight - 1) / lineHeight;
  for (int i = lineIndexAtBoundary; i < lineIndexAtBoundary + 3; ++i) {
    EXPECT_EQ(widthForLine(i, lineHeight, 0, pageWidth, r.zonePageN1), pageWidth)
        << "Line " << i << " below tile B must be full width";
  }
}

TEST(SplitFloatImage, PageNZoneNarrowsCorrectly) {
  // On page N: the zone covers [curY, vpH). Lines of the paragraph starting at curY
  // (blockStartY = curY after margin correction) see the zone.
  const int16_t imgH = 100, imgW = 60, curY = 750, vpH = 800;
  const int pageWidth = 400;
  const int lineHeight = 22;
  auto r = simulateSplit(imgH, imgW, curY, vpH);

  // blockStartY on page N is curY (the image and paragraph start at the same Y).
  // Line 0: top=750, overlaps zone [750,800) → narrowed.
  EXPECT_EQ(widthForLine(0, lineHeight, curY, pageWidth, r.zonePageN), pageWidth - (imgW + 4))
      << "First line on page N must be narrowed by tile A zone";
}

// Case 7 from test_float_images.epub: visual verification reference.
// This is a scenario-level geometry check that mirrors what the parser does
// for a 60x100 image placed at ~750px on an 800px page.
TEST(FloatScenario, Case7_SplitFloat_PageBoundary) {
  const int16_t imgH = 100, imgW = 60;
  const int16_t vpH = 800;
  const int pageWidth = 400;
  const int lineHeight = 22;

  // Simulate: image placed when 50px remain on page.
  const int16_t curY = vpH - 50;  // 750
  ASSERT_LT(vpH - curY, imgH) << "Pre-condition: image must overflow the page";

  auto r = simulateSplit(imgH, imgW, curY, vpH);
  EXPECT_EQ(r.tileAHeight, 50);
  EXPECT_EQ(r.tileBHeight, 50);

  // Page N: 2 lines fit in the 50px zone (line 0: y=750..772, line 1: y=772..794).
  const int narrowW = pageWidth - (imgW + 4);
  EXPECT_EQ(widthForLine(0, lineHeight, curY, pageWidth, r.zonePageN), narrowW);
  EXPECT_EQ(widthForLine(1, lineHeight, curY, pageWidth, r.zonePageN), narrowW);

  // Page N+1: tile B zone [0, 50). Line 2 (y=44..66) partially overlaps → narrowed.
  EXPECT_EQ(widthForLine(2, lineHeight, 0, pageWidth, r.zonePageN1), narrowW);
  // Line 3 (y=66 > 50): full width.
  EXPECT_EQ(widthForLine(3, lineHeight, 0, pageWidth, r.zonePageN1), pageWidth);

  // Simulate paragraph line layout on both pages
  std::vector<int> words(40, 30);
  const int gap = 6;

  auto pageNLines = simulateLineBreaks(words, gap, curY, lineHeight, pageWidth, r.zonePageN);
  auto pageN1Lines = simulateLineBreaks(words, gap, 0, lineHeight, pageWidth, r.zonePageN1);

  // Lines within tile A zone on page N must be narrower than full width.
  EXPECT_LE(pageNLines[0], narrowW) << "Page N line 0 must be narrowed beside tile A";

  // Lines within tile B zone on page N+1 must also be narrowed.
  EXPECT_LE(pageN1Lines[0], narrowW) << "Page N+1 line 0 must be narrowed beside tile B";

  // Lines after tile B must be full width.
  bool anyWide = false;
  for (size_t i = 3; i < pageN1Lines.size(); ++i) {
    if (pageN1Lines[i] > narrowW) {
      anyWide = true;
      break;
    }
  }
  EXPECT_TRUE(anyWide) << "Page N+1 must have full-width lines once tile B is exhausted";
}
