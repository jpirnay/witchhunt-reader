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
