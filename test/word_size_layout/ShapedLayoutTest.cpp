// Ours (writing modes step 2): ParsedText shapes its Indic tokens ("How shaping runs" steps 2-6 and 10). Words
// are shaped through a FixtureShapingFace over the TrueType Devanagari subset, installed through the stub renderer's
// seam; the expected records are the oracle's, ot::shape at the same size (ottest::shapeAsStored). The forms the tokens
// take on their lines, piece by piece (steps 7-9), are the ShapedLine suite's.
#include <Arduino.h>
#include <BidiUtils.h>
#include <BufferedFileIO.h>
#include <BuildArena.h>
#include <EpdFontData.h>
#include <GlyphRun.h>
#include <OtShaper.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "ArabicNumberExpected.h"
#include "Epub/HeapGrowth.h"
#include "Epub/ParsedText.h"
#include "Epub/ShapedStore.h"
#include "Epub/ShapingHeapCheck.h"
#include "Epub/WordShapeCache.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "FixtureShapingFace.h"
#include "GfxRenderer.h"
#include "IndicReorder.h"
#include "IndicScripts.h"
#include "IndicShaper.h"
#include "OtShaperTestHelpers.h"
#include "TextShaping.h"

namespace {

using ottest::StoredGlyph;

const std::string kHindi = "हिन्दी";
const std::string kPatrika = "पत्रिका";
const std::string kPrarthana = "प्रार्थना";
const std::string kGyan = "ज्ञान";
const std::string kSanskritam = "संस्कृतम्";
const std::string kKa = "\xE0\xA4\x95";              // क
const std::string kLa = "\xE0\xA4\xB2";              // ल
const std::string kMa = "\xE0\xA4\xAE";              // म
const std::string kIMatra = "\xE0\xA4\xBF";          // ि
const std::string kKi = kKa + kIMatra;               // कि
const std::string kHi = "\xE0\xA4\xB9\xE0\xA4\xBF";  // हि
const std::string kNdi = "न्दी";                      // the rest of हिन्दी
const std::string kEmDash = "\xE2\x80\x94";          // —
const std::string kCantillation = "\xD6\x91";        // U+0591, a Hebrew accent
constexpr uint32_t kPpem = 1280;                     // 20 px a em
constexpr int kGlyph = GfxRenderer::GLYPH_W;         // the stub's code point, and so a hyphen's, width
constexpr int kSpace = GfxRenderer::SPACE_W;

// The face the seam hands out, at the ppem it reports; null: no font shapes. `refusals`: how many asks answer null
// first, saying so, as a face refused its memory does; `refusedStyle`: a style every ask of which is refused so (-1:
// none).
struct Seam {
  static inline ShapingFace* face = nullptr;
  static inline uint32_t ppem = kPpem;
  static inline int refusals = 0;
  static inline int refusedStyle = -1;
  static inline int asks = 0;
  static ShapingFace* source(int /*fontId*/, const EpdFontFamily::Style style, uint32_t* ppem26_6, bool* refused) {
    ++asks;
    if (refusals > 0 || style == refusedStyle) {
      if (refusals > 0) --refusals;
      if (refused != nullptr) *refused = true;
      return nullptr;
    }
    if (face != nullptr) *ppem26_6 = ppem;
    return face;
  }
};

// The largest free block the layout's heap checks read (the shim's ESP.getMaxAllocHeap()), for a scope. Nothing is
// refused by it: only the checks see it, so a test makes every other container already hold what its pass needs.
class LargestFreeBlock {
 public:
  explicit LargestFreeBlock(const uint32_t bytes) : saved_(ESP.getMaxAllocHeap()) { ESP.setMaxAllocHeap(bytes); }
  ~LargestFreeBlock() { ESP.setMaxAllocHeap(saved_); }
  LargestFreeBlock(const LargestFreeBlock&) = delete;
  LargestFreeBlock& operator=(const LargestFreeBlock&) = delete;

 private:
  uint32_t saved_;
};

struct Word {
  std::string text;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  bool glued = false;
  uint32_t offset = 0;
};

// Suppress the automatic one-em paragraph indent so width math in tests stays simple.
BlockStyle noIndentStyle() {
  BlockStyle bs;
  bs.textIndent = 0;
  bs.textIndentDefined = true;
  return bs;
}

void add(ParsedText& text, const std::vector<Word>& words) {
  for (const Word& w : words) text.addWord(w.text, w.style, false, w.glued, 100, w.offset);
}

using Lines = std::vector<std::unique_ptr<TextBlock>>;

Lines layOut(ParsedText& text, const GfxRenderer& renderer, const int width, const bool includeLastLine = true) {
  Lines lines;
  text.layoutAndExtractLines(
      renderer, 1, static_cast<uint16_t>(width),
      [&lines](std::unique_ptr<TextBlock> line, bool, bool) {
        lines.push_back(std::move(line));
        return ParsedText::LineProcessResult::Accepted;
      },
      includeLastLine);
  return lines;
}

std::string bytesOf(const TextBlock& line) {
  FsFile file = HalFile::forReadWrite();
  {
    serialization::BufferedFileWriter writer(file);
    EXPECT_TRUE(line.serialize(writer));
    EXPECT_TRUE(writer.flush());
  }
  EXPECT_TRUE(file.seek(0));
  std::string bytes(file.size(), '\0');
  EXPECT_EQ(static_cast<int>(bytes.size()), file.read(bytes.data(), bytes.size()));
  return bytes;
}

std::vector<uint32_t> codepoints(const std::string& text) { return ottest::utf32(text.c_str()); }

// The line's logical text: its words' texts, joined.
std::string lineText(const TextBlock& line) {
  std::string text;
  for (uint16_t i = 0; i < line.wordCount(); ++i) text += line.wordText(i);
  return text;
}

// The syllables of `text`, cut where indic::syllableBreakAllowed allows (the fallback breaks' rule).
std::vector<std::string> syllables(const std::string& text) {
  std::vector<std::string> out;
  uint32_t previous = 0;
  for (size_t pos = 0; pos < text.size();) {
    const size_t at = pos;
    const uint32_t cp = TextShaping::decodeAt(text, pos);
    if (out.empty() || indic::syllableBreakAllowed(previous, cp)) out.emplace_back();
    out.back().append(text, at, pos - at);
    previous = cp;
  }
  return out;
}

class ShapedLayout : public testing::Test {
 protected:
  void SetUp() override {
    TextShaping::releaseBuildMemory();
    face = std::make_unique<FixtureShapingFace>(readFont(TTF_FIXTURE_DIR, "NotoSansDevanagari-Shaping-Regular.ttf"));
    Seam::face = face.get();
    Seam::ppem = kPpem;
    Seam::refusals = 0;
    Seam::refusedStyle = -1;
    Seam::asks = 0;
    GfxRenderer::shapingFaceSource = &Seam::source;
  }
  void TearDown() override {
    GfxRenderer::shapingFaceSource = nullptr;
    Seam::face = nullptr;
    TextShaping::releaseBuildMemory();
  }

  // The oracle: `text` shaped by ot::shape alone, in the script's default language, at `ppem`.
  std::vector<StoredGlyph> oracle(const std::string& text, const uint32_t ppem = kPpem) {
    const ot::Plan* plan = face->plan(ot::Script::Devanagari, nullptr);
    const ot::Face* shaping = face->current();
    if (plan == nullptr || shaping == nullptr) {
      ADD_FAILURE() << "the fixture face does not shape";
      return {};
    }
    return ottest::shapeAsStored(*shaping, *plan, text.c_str(), ppem);
  }
  // How far the oracle's records move the pen, as glyphRunWidth measures an unscaled run.
  int shapedPx(const std::string& text, const uint32_t ppem = kPpem) {
    const std::vector<StoredGlyph> run = oracle(text, ppem);
    EXPECT_FALSE(run.empty()) << text;
    int px = 0;
    for (const StoredGlyph& g : run) px += fp4::toPixel(g.advance12_4);
    return px;
  }

  // The seam's face from now on: the TrueType fixture `file`, for the rest of the test.
  FixtureShapingFace& useFont(const char* file) {
    other = std::make_unique<FixtureShapingFace>(readFont(TTF_FIXTURE_DIR, file));
    Seam::face = other.get();
    return *other;
  }
  // The oracle on another face and script.
  static std::vector<StoredGlyph> oracleOn(FixtureShapingFace& shaping, const ot::Script script,
                                           const std::string& text) {
    const ot::Plan* plan = shaping.plan(script, nullptr);
    const ot::Face* current = shaping.current();
    if (plan == nullptr || current == nullptr) {
      ADD_FAILURE() << "the face does not shape";
      return {};
    }
    return ottest::shapeAsStored(*current, *plan, text.c_str(), kPpem);
  }
  static int advancePx(const std::vector<StoredGlyph>& run) {
    int px = 0;
    for (const StoredGlyph& g : run) px += fp4::toPixel(g.advance12_4);
    return px;
  }

  std::unique_ptr<FixtureShapingFace> face;
  std::unique_ptr<FixtureShapingFace> other;
  GfxRenderer renderer;
  bool latch = false;
};

}  // namespace

TEST_F(ShapedLayout, BreaksOnTheShapedWidths) {
  const int w1 = shapedPx(kHindi);
  const int w2 = shapedPx(kPatrika);
  // Laid out as code points the two words take 13 of the stub's 10 px glyphs: they would not fit.
  ASSERT_LT(w1 + w2, 13 * kGlyph);
  const int fits = w1 + kSpace + w2;
  for (const int width : {fits, fits - 1}) {
    ParsedText text(false, false, noIndentStyle());
    add(text, {{kHindi}, {kPatrika}});
    EXPECT_EQ(width == fits ? 1u : 2u, layOut(text, renderer, width).size()) << width;
  }
}

TEST_F(ShapedLayout, ADashJoinedTokenShapesEachPartAsItsOwnRun) {
  // The dash is no letter of the script and no shared code point, so the run scan cuts at it: each part is a run of
  // its own, and the dash a Text piece measured as today. Cut at its runs' edges on the line, each half is a word the
  // dictionary can select.
  const std::string dashed = kKa + kEmDash + kLa;
  const int fits = shapedPx(kKa) + kGlyph + shapedPx(kLa) + kSpace + shapedPx(kHindi);
  for (const int width : {fits, fits - 1}) {
    ParsedText text(false, false, noIndentStyle());
    add(text, {{dashed}, {kHindi}});
    EXPECT_EQ(width == fits ? 1u : 2u, layOut(text, renderer, width).size()) << width;
  }
  EXPECT_EQ(2 * 3, renderer.prefetchedRuns);  // two Glyphs pieces and हिन्दी's, in each of the two layouts
}

TEST_F(ShapedLayout, ATokenKeptForTheNextPassIsNotShapedAgain) {
  const int width = shapedPx(kHindi);  // one word a line
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kHindi}, {kHindi}, {kHindi}});
  EXPECT_EQ(2u, layOut(text, renderer, width, /*includeLastLine=*/false).size());
  EXPECT_EQ(3u, text.tokensShaped());
  EXPECT_EQ(1u, layOut(text, renderer, width).size());
  EXPECT_EQ(3u, text.tokensShaped());  // the kept token's records were the ones the first pass made
}

TEST_F(ShapedLayout, AKeptTokenIsShapedAgainWhenAGluedNeighbourArrives) {
  // क ends the first pass; म, glued in another style, starts a syllable of its own, so the two stay apart and each is
  // shaped with the other as context: क's records from the first pass had none.
  const int width = shapedPx(kHindi);
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kHindi}, {kKa}});
  EXPECT_EQ(1u, layOut(text, renderer, width, /*includeLastLine=*/false).size());
  EXPECT_EQ(2u, text.tokensShaped());
  add(text, {{kMa, EpdFontFamily::BOLD, true}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(2u, lines[0]->wordCount());
  EXPECT_EQ(kKa, lines[0]->wordText(0));
  EXPECT_EQ(kMa, lines[0]->wordText(1));
  EXPECT_EQ(4u, text.tokensShaped());  // क again, and म
}

TEST_F(ShapedLayout, AGroupAcrossTheFlushIsMergedWhole) {
  // The parser's 96-token flush can end a pass inside a word: क is kept for the next pass, which brings its i-matra.
  const int width = shapedPx(kHindi);
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kHindi}, {kKa}});
  const Lines first = layOut(text, renderer, width, /*includeLastLine=*/false);
  ASSERT_EQ(1u, first.size());
  EXPECT_EQ(kHindi, first[0]->wordText(0));
  add(text, {{kIMatra, EpdFontFamily::BOLD, true}});
  const Lines last = layOut(text, renderer, width);
  ASSERT_EQ(1u, last.size());
  ASSERT_EQ(1u, last[0]->wordCount());
  EXPECT_EQ(kKi, last[0]->wordText(0));
  EXPECT_EQ(EpdFontFamily::REGULAR, last[0]->wordStyle(0));
}

TEST_F(ShapedLayout, RecordsOfAnotherSizeAreNotReused) {
  // Review Focus 1: the reader's size changes between two slices of a build.
  const int width = shapedPx(kHindi);
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kHindi}, {kHindi}, {kHindi}});
  EXPECT_EQ(2u, layOut(text, renderer, width, /*includeLastLine=*/false).size());
  EXPECT_EQ(3u, text.tokensShaped());
  Seam::ppem = 1600;
  EXPECT_EQ(1u, layOut(text, renderer, 400).size());
  EXPECT_EQ(4u, text.tokensShaped());  // the kept token, shaped again at 25 px
}

TEST_F(ShapedLayout, MergesASyllableSplitByAStyle) {
  // क<b>ि</b>: the glue falls inside a syllable, so the two are one token, shaped as one run and drawn in क's style
  // (deviation 7). The merged token keeps क's visible offset.
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kHindi, EpdFontFamily::REGULAR, false, 0},
             {kKa, EpdFontFamily::REGULAR, false, 7},
             {kIMatra, EpdFontFamily::BOLD, true, 10}});
  const int width = shapedPx(kHindi);  // हिन्दी, then कि on a line of its own
  const Lines lines = layOut(text, renderer, width);
  ASSERT_EQ(2u, lines.size());
  ASSERT_EQ(1u, lines[1]->wordCount());
  EXPECT_EQ(kKi, lines[1]->wordText(0));
  EXPECT_EQ(EpdFontFamily::REGULAR, lines[1]->wordStyle(0));
  EXPECT_EQ(7u, text.lastLineVisibleOffset());
}

TEST_F(ShapedLayout, ASpanSplitOfOneStyleIsByteForByteOneToken) {
  ParsedText split(false, false, noIndentStyle());
  add(split, {{kHi}, {kNdi, EpdFontFamily::REGULAR, true}});
  ParsedText whole(false, false, noIndentStyle());
  add(whole, {{kHindi}});
  const Lines splitLines = layOut(split, renderer, 400);
  const Lines wholeLines = layOut(whole, renderer, 400);
  ASSERT_EQ(1u, splitLines.size());
  ASSERT_EQ(1u, wholeLines.size());
  EXPECT_EQ(1u, splitLines[0]->wordCount());
  EXPECT_EQ(bytesOf(*wholeLines[0]), bytesOf(*splitLines[0]));
}

TEST_F(ShapedLayout, AVerticalLineReordersInsteadOfShaping) {
  // A vertical line's load refuses glyph forms, so its Indic words are never shaped: no face is asked for, and each
  // takes IndicReorder's form.
  ParsedText text(false, false, noIndentStyle());
  text.setVertical(true);
  text.setShapingLatch(&latch);
  add(text, {{kHindi}, {kPatrika}});
  EXPECT_FALSE(layOut(text, renderer, 400).empty());
  EXPECT_EQ(2u, text.tokensShaped());
  EXPECT_EQ(0, face->planBuilds());
  EXPECT_EQ(0, renderer.prefetchedRuns);
  EXPECT_FALSE(latch);
}

TEST_F(ShapedLayout, AtTheFreeHeapFloorAParagraphShapesNothingAndLatches) {
  // The shaped store keeps heap_growth::kShapedFreeFloor free: at it, the store takes nothing, every token is laid out
  // as its code points, and the paragraph latches shaping degraded. The largest block is ample throughout.
  const uint32_t savedFree = ESP.getFreeHeap();
  Lines lines;
  {
    ParsedText text(false, false, noIndentStyle());
    text.setShapingLatch(&latch);
    add(text, {{kHindi}, {kPatrika}});
    ESP.setFreeHeap(static_cast<uint32_t>(heap_growth::kShapedFreeFloor));
    lines = layOut(text, renderer, 400);
    ESP.setFreeHeap(savedFree);
  }
  ASSERT_EQ(1u, lines.size());
  EXPECT_FALSE(lines[0]->hasGlyphForm(0));
  EXPECT_FALSE(lines[0]->hasGlyphForm(1));
  EXPECT_TRUE(latch);

  // With the heap back, the same paragraph shapes.
  latch = false;
  ParsedText again(false, false, noIndentStyle());
  again.setShapingLatch(&latch);
  add(again, {{kHindi}, {kPatrika}});
  lines = layOut(again, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  EXPECT_TRUE(lines[0]->hasGlyphForm(0));
  EXPECT_FALSE(latch);
}

TEST_F(ShapedLayout, APlanTheBuildsFloorRefusesLaysTheTokensOutAsCodePointsAndLatches) {
  // A build's ShapingHeapCheck puts the plan behind the same floor: with 2 KB free above it, the store's growths (a
  // 1 KB chunk the most) fit and the plan (the builder's 3.1 KB scratch) does not. No plan is built, every token is
  // laid out as its code points, and the paragraph latches. The largest block is ample throughout.
  const uint32_t savedFree = ESP.getFreeHeap();
  const auto aboveTheFloor = [] { ESP.setFreeHeap(static_cast<uint32_t>(heap_growth::kShapedFreeFloor + 2048)); };
  Lines lines;
  {
    const ShapingHeapCheck build;
    ParsedText text(false, false, noIndentStyle());
    text.setShapingLatch(&latch);
    add(text, {{kHindi}, {kPatrika}});
    aboveTheFloor();
    lines = layOut(text, renderer, 400);
    ESP.setFreeHeap(savedFree);
  }
  ASSERT_EQ(1u, lines.size());
  EXPECT_FALSE(lines[0]->hasGlyphForm(0));
  EXPECT_FALSE(lines[0]->hasGlyphForm(1));
  EXPECT_EQ(kHindi, lines[0]->wordText(0));
  EXPECT_TRUE(latch);
  EXPECT_EQ(0, face->planBuilds());

  // Outside a build (UI text) no check is installed: the same heap builds the plan, and the paragraph shapes.
  latch = false;
  ParsedText again(false, false, noIndentStyle());
  again.setShapingLatch(&latch);
  add(again, {{kHindi}, {kPatrika}});
  aboveTheFloor();
  lines = layOut(again, renderer, 400);
  ESP.setFreeHeap(savedFree);
  ASSERT_EQ(1u, lines.size());
  EXPECT_TRUE(lines[0]->hasGlyphForm(0));
  EXPECT_FALSE(latch);
  EXPECT_EQ(1, face->planBuilds());
}

TEST_F(ShapedLayout, ANoBreakSpaceIsNeverMerged) {
  // The parser's no-break space is a glued " " token of the word's style: it stays a space, never part of a word.
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kHindi}, {" ", EpdFontFamily::REGULAR, true}, {kPatrika, EpdFontFamily::REGULAR, true}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(3u, lines[0]->wordCount());
  EXPECT_EQ(kHindi, lines[0]->wordText(0));
  EXPECT_STREQ(" ", lines[0]->wordText(1));
  EXPECT_EQ(kPatrika, lines[0]->wordText(2));
}

TEST_F(ShapedLayout, AnEmphasisMarkIsPartOfTheStyle) {
  // हि<span style="text-emphasis">न्दी</span>: the glue falls between syllables, and the second part's mark is a style
  // of its own, so the two stay apart and each keeps its mark; with the same mark they are one span split.
  for (const uint8_t mark : {uint8_t{1}, uint8_t{0}}) {
    ParsedText text(false, false, noIndentStyle());
    text.addWord(kHi, EpdFontFamily::REGULAR);
    text.addWord(kNdi, EpdFontFamily::REGULAR, false, true, 100, 0, 0, mark);
    const Lines lines = layOut(text, renderer, 400);
    ASSERT_EQ(1u, lines.size());
    EXPECT_EQ(mark != 0 ? 2u : 1u, lines[0]->wordCount()) << int{mark};
  }
}

TEST_F(ShapedLayout, ANeighbourOfAMergedTokenIsShapedAgain) {
  // म and क (bold, glued) are kept apart and kept for the next pass, which brings क's i-matra: क and ि merge, so म's
  // glued neighbour, its context, is now कि.
  const int width = shapedPx(kHindi);
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kHindi}, {kMa}, {kKa, EpdFontFamily::BOLD, true}});
  EXPECT_EQ(1u, layOut(text, renderer, width, /*includeLastLine=*/false).size());
  EXPECT_EQ(3u, text.tokensShaped());
  add(text, {{kIMatra, EpdFontFamily::BOLD, true}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(2u, lines[0]->wordCount());
  EXPECT_EQ(kMa, lines[0]->wordText(0));
  EXPECT_EQ(kKi, lines[0]->wordText(1));
  EXPECT_EQ(5u, text.tokensShaped());  // कि, and म again with it as its context
}

TEST_F(ShapedLayout, AFaceIsResolvedOncePerPass) {
  // Every key and every call of a pass reads one answer per font and style: a face refused at the first ask (its
  // memory) leaves the whole pass laid out as a font that cannot shape is, so no entry is stored under a key that says
  // otherwise. That is not what the font draws, so the refusal latches (invariant 6).
  Seam::refusals = 1;
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{kHindi}, {kPatrika}, {kKi}});
  EXPECT_EQ(1u, layOut(text, renderer, 400).size());
  EXPECT_EQ(1, Seam::asks);
  EXPECT_EQ(3u, text.tokensShaped());
  EXPECT_EQ(0, renderer.prefetchedRuns);  // reordered, as without a face: no glyph run
  EXPECT_TRUE(latch);

  // The next paragraph asks again, and shapes.
  text.reset(noIndentStyle());
  add(text, {{kHindi}});
  EXPECT_EQ(1u, layOut(text, renderer, 400).size());
  EXPECT_EQ(2, Seam::asks);
  EXPECT_EQ(1, renderer.prefetchedRuns);
}

TEST_F(ShapedLayout, AFaceRefusedForAStyleOfLatinWordsLatchesNothing) {
  // Only a token that holds Indic text asks for a face, so a refusal for a style whose words are all Latin is never
  // seen: the paragraph's Hindi words shape in their own style, and nothing changed.
  Seam::refusedStyle = EpdFontFamily::BOLD;
  {
    ParsedText text(false, false, noIndentStyle());
    text.setShapingLatch(&latch);
    add(text, {{kHindi}, {"Latin", EpdFontFamily::BOLD}, {kPatrika}});
    EXPECT_EQ(1u, layOut(text, renderer, 400).size());
    EXPECT_EQ(1, Seam::asks);  // the regular face, once
    EXPECT_EQ(2, renderer.prefetchedRuns);
    EXPECT_FALSE(latch);
  }
  // The control: a Hindi word in bold asks for the refused face, and latches.
  {
    ParsedText text(false, false, noIndentStyle());
    text.setShapingLatch(&latch);
    add(text, {{kHindi}, {kPatrika, EpdFontFamily::BOLD}});
    EXPECT_EQ(1u, layOut(text, renderer, 400).size());
    EXPECT_TRUE(latch);
  }
}

TEST_F(ShapedLayout, ACandidateThatCannotBeShapedLatches) {
  // Ruling 2: every run laid out as code points because it could not be shaped latches, a forced split's candidate
  // too, though no split follows it. हिन्दी is shaped and kept by the first pass; the second, short of memory, measures
  // its one candidate (हि-) as code points, 30 px, which the 25 px column cannot hold.
  constexpr int kColumn = 25;
  ASSERT_GT(shapedPx(kHindi), kColumn);
  BuildArena arena(64);
  ASSERT_TRUE(arena.valid());
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  text.setLineArena(&arena);
  add(text, {{kHindi}});
  EXPECT_TRUE(layOut(text, renderer, 400, /*includeLastLine=*/false).empty());
  EXPECT_FALSE(latch);
  TextShaping::releaseBuildMemory();
  Lines lines;
  {
    const AllocationRefusal refused(4096);
    lines = layOut(text, renderer, kColumn);
  }
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(1u, lines[0]->wordCount());  // not split
  EXPECT_EQ(1u, text.tokensShaped());    // the token's records were the first pass's
  EXPECT_TRUE(latch);
}

TEST_F(ShapedLayout, AMergedTokenStaysWithinItsCap) {
  // Two words of the parser's length (it cuts at 200 B) merge whole; a run of one style past 512 B stays in tokens,
  // each shaped with its neighbour as context, so neither a call's block nor the breaker's candidates grow with it.
  std::string ka60;
  for (int i = 0; i < 60; ++i) ka60 += kKa;  // 180 B
  std::string ka100;
  for (int i = 0; i < 100; ++i) ka100 += kKa;  // 300 B
  {
    ParsedText text(false, false, noIndentStyle());
    add(text, {{ka60}, {ka60, EpdFontFamily::REGULAR, true}});
    const Lines lines = layOut(text, renderer, 4000);
    ASSERT_EQ(1u, lines.size());
    EXPECT_EQ(1u, lines[0]->wordCount());
  }
  {
    ParsedText text(false, false, noIndentStyle());
    add(text, {{ka100}, {ka100, EpdFontFamily::REGULAR, true}});
    const Lines lines = layOut(text, renderer, 4000);
    ASSERT_EQ(1u, lines.size());
    EXPECT_EQ(2u, lines[0]->wordCount());
    EXPECT_EQ(2u, text.tokensShaped());
  }
  {
    // Glue inside a syllable merges past the cap: kept apart, the i-matra would be shaped without its consonant.
    const std::string matraLed = kIMatra + ka100.substr(0, ka100.size() - kKa.size());  // 300 B
    ParsedText text(false, false, noIndentStyle());
    add(text, {{ka100}, {matraLed, EpdFontFamily::BOLD, true}});
    const Lines lines = layOut(text, renderer, 4000);
    ASSERT_EQ(1u, lines.size());
    ASSERT_EQ(1u, lines[0]->wordCount());
    EXPECT_EQ(ka100 + matraLed, lines[0]->wordText(0));
  }
}

TEST_F(ShapedLayout, TheDivineNameGluedToAMarkIsDrawnAsHeAndGeresh) {
  // BidiLine.ANameSplitFromItsMarkFallsBackInTheLinePass's words, as a page brings them: the accent in a span of its
  // own, glued to the name. ParsedText merges the pair first, so the line pass sees one token and orders the line.
  ParsedText text(false, false, noIndentStyle());
  const std::string name = "יְהוָה";
  add(text, {{"וַיֹּאמֶר"}, {name}, {kCantillation, EpdFontFamily::REGULAR, true}, {"אֶל"}, {"מֹשֶׁה"}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(4u, line.wordCount());
  EXPECT_TRUE(line.hasLogicalOrder());
  bool found = false;
  for (uint16_t i = 0; i < line.wordCount(); ++i) {
    if (line.wordText(i) != name + kCantillation) continue;
    found = true;
    EXPECT_EQ(codepoints(line.wordDrawText(i)), (std::vector<uint32_t>{0x05F3, 0x05D4}));
  }
  EXPECT_TRUE(found);
}

TEST_F(ShapedLayout, AForcedSplitFallsBetweenSyllables) {
  // Review Focus 3: a compound wider than its column. Each part is measured as it is shaped, its hyphen included. A
  // line holds one part (its run, and its hyphen as a piece of its own).
  const std::string compound = kHindi + kPatrika + kPrarthana + kGyan + kSanskritam;
  ASSERT_EQ(14u, syllables(compound).size());
  constexpr int kColumn = 60;
  ASSERT_GT(shapedPx(compound), kColumn);
  ParsedText text(false, false, noIndentStyle());
  add(text, {{compound}});
  const Lines lines = layOut(text, renderer, kColumn);
  ASSERT_GE(lines.size(), 2u);

  std::string joined;
  for (size_t k = 0; k < lines.size(); ++k) {
    std::string part = lineText(*lines[k]);
    const bool last = k + 1 == lines.size();
    if (!last) {
      ASSERT_EQ('-', part.back()) << k;
      part.pop_back();
    }
    const uint32_t first = codepoints(part).front();
    EXPECT_FALSE(indic::isDependentSign(first) || first == 0x200C || first == 0x200D) << k << ": " << part;
    // The text from this line on is what was left of the compound: it is split only when its shaped width is wider
    // than the column (each part is shaped again as the token it becomes).
    const std::string left = compound.substr(joined.size());
    joined += part;
    if (last) {
      EXPECT_LE(shapedPx(left), kColumn) << k << ": " << left;
      continue;
    }
    EXPECT_GT(shapedPx(left), kColumn) << k << ": " << left;
    // The part fits with its hyphen, and the next syllable would not: the breaker took the widest prefix the shaped
    // widths allow, not what the code points' widths would.
    EXPECT_LE(shapedPx(part) + kGlyph, kColumn) << k << ": " << part;
    const std::string rest = compound.substr(joined.size());
    const std::vector<std::string> restSyllables = syllables(rest);
    const std::string longer = part + restSyllables.front();
    if (codepoints(rest).size() - codepoints(restSyllables.front()).size() >= 2) {
      EXPECT_GT(shapedPx(longer) + kGlyph, kColumn) << k << ": " << longer;
    }
  }
  EXPECT_EQ(compound, joined);
}

TEST_F(ShapedLayout, ARefusedCallLaysTheTokenOutAsCodePointsAndLatches) {
  // The spec's review focus 2: no room in the arena (64 B) and none on the heap for the call's block.
  const int fits = shapedPx(kHindi) + kSpace + shapedPx(kPatrika);
  ASSERT_LT(fits, 13 * kGlyph + kSpace);
  BuildArena arena(64);
  ASSERT_TRUE(arena.valid());
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  text.setLineArena(&arena);
  add(text, {{kHindi}, {kPatrika}});
  Lines lines;
  {
    const AllocationRefusal refused(4096);
    lines = layOut(text, renderer, fits);
  }
  EXPECT_EQ(2u, lines.size());  // the code points' 135 px do not fit
  EXPECT_TRUE(latch);
  EXPECT_EQ(2u, text.tokensShaped());

  // The next paragraph, with memory, shapes them.
  text.reset(noIndentStyle());
  add(text, {{kHindi}, {kPatrika}});
  EXPECT_EQ(1u, layOut(text, renderer, fits).size());
  EXPECT_EQ(4u, text.tokensShaped());
}

// The heap checks before the shaped layout's containers grow. Each test lays out a warm-up paragraph first, so that
// every container but the one under test already holds what the next pass needs; that pass then runs with a largest
// free block too small for the one growth.

TEST_F(ShapedLayout, AMergeTheHeapRefusesKeepsTheTokensApartAndLatches) {
  // हि and न्दी in one style, glued, are one token (ASpanSplitOfOneStyleIsByteForByteOneToken), whose text grows हि's
  // string: 18 B past its 15, a block of 47 B with the allocator's slack. Refused, the two stay apart, each shaped with
  // the other as context.
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{kHindi}, {kPatrika}, {kPrarthana}, {kGyan}});
  ASSERT_EQ(1u, layOut(text, renderer, 400).size());
  ASSERT_FALSE(latch);
  text.reset(noIndentStyle());
  add(text, {{kHi}, {kNdi, EpdFontFamily::REGULAR, true}});
  Lines lines;
  {
    const LargestFreeBlock block(32);
    lines = layOut(text, renderer, 400);
  }
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(2u, lines[0]->wordCount());
  EXPECT_EQ(kHi, lines[0]->wordText(0));
  EXPECT_EQ(kNdi, lines[0]->wordText(1));
  EXPECT_TRUE(latch);
}

TEST_F(ShapedLayout, ATokenTheHeapHasNoRoomToMeasureIsLaidOutAsCodePoints) {
  // Without a face a token is one Reordered piece, measured from a copy of its form. The warm-up's form, कि's, fits the
  // copy's 15 B; the next token's, 48 B, asks for a block of 65 B. Refused, the token is laid out as its code points.
  Seam::face = nullptr;
  std::string kis;
  for (int k = 0; k < 8; ++k) kis += kKi;
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{kKi}});
  ASSERT_EQ(1u, layOut(text, renderer, 400).size());
  ASSERT_FALSE(latch);
  text.reset(noIndentStyle());
  add(text, {{kis}});
  Lines lines;
  {
    const LargestFreeBlock block(48);
    lines = layOut(text, renderer, 4000);
  }
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(1u, lines[0]->wordCount());
  EXPECT_FALSE(lines[0]->hasDisplayForm(0));
  EXPECT_EQ(kis, lines[0]->wordDrawText(0));
  EXPECT_TRUE(latch);
}

TEST_F(ShapedLayout, ASplitTheHeapHasNoRoomForIsNotMadeAndLatches) {
  // AForcedSplitFallsBetweenSyllables's compound copies each candidate prefix into one string, grown once to the word
  // and a hyphen. The warm-up lays the compound out whole, so no candidate was copied; the split asks for the string,
  // which the heap refuses: the compound is not split.
  const std::string compound = kHindi + kPatrika + kPrarthana + kGyan + kSanskritam;
  constexpr int kColumn = 60;
  ASSERT_GT(shapedPx(compound), kColumn);
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{compound}});
  ASSERT_EQ(1u, layOut(text, renderer, 4000).size());
  ASSERT_FALSE(latch);
  text.reset(noIndentStyle());
  add(text, {{compound}});
  Lines lines;
  {
    const LargestFreeBlock block(48);
    lines = layOut(text, renderer, kColumn);
  }
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(1u, lines[0]->wordCount());
  EXPECT_EQ(compound, lineText(*lines[0]));
  EXPECT_TRUE(latch);
}

TEST_F(ShapedLayout, ACandidateTheHeapHasNoRoomToMeasureLatches) {
  // हि, forty Latin letters and हिन्दी are one token of three pieces: the letters a Text piece of 40 B, measured from a
  // copy that grows to them. A candidate cut where the letters end is हि, the letters and a hyphen: its Text piece is
  // 41 B, past the copy's 40, and the heap refuses the 97 B it asks for. That candidate is measured as code points, as
  // one that could not be shaped is, and latches. The warm-up's split, of a longer compound of glyph runs alone, leaves
  // the candidates' string the room this word needs. The column is a pixel short of the word: every shorter candidate
  // fits, so the search, which stops a kind at its first prefix too wide, reaches that one.
  const std::string letters(40, 'a');
  const std::string word = kHi + letters + kHindi;
  const std::string compound = kHindi + kPatrika + kPrarthana + kGyan + kSanskritam;
  ASSERT_GT(compound.size(), word.size());
  const int kColumn = shapedPx(kHi) + 40 * GfxRenderer::GLYPH_W + shapedPx(kHindi) - 1;
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{compound}});
  ASSERT_GE(layOut(text, renderer, 60).size(), 2u);
  ASSERT_FALSE(latch);
  text.reset(noIndentStyle());
  add(text, {{word}});
  Lines lines;
  {
    const LargestFreeBlock block(64);  // the token's own copy (57 B) fits; the candidate's does not
    lines = layOut(text, renderer, kColumn);
  }
  ASSERT_GE(lines.size(), 2u);  // split at a candidate that was measured
  EXPECT_EQ('-', lineText(*lines[0]).back());
  EXPECT_TRUE(latch);
}

TEST_F(ShapedLayout, AFontThatCannotShapeBuildsNoPlan) {
  // Review Focus 5: the words take IndicReorder's form, and nothing is latched, so the spine is not rebuilt at each
  // open.
  {
    Seam::face = nullptr;  // a .cpfont or built-in font
    ParsedText text(false, false, noIndentStyle());
    text.setShapingLatch(&latch);
    add(text, {{kKi}, {kHindi}});
    EXPECT_EQ(1u, layOut(text, renderer, 400).size());
    EXPECT_EQ(2u, text.tokensShaped());
    EXPECT_FALSE(latch);
  }
  {
    // A TrueType family without the script: Devanagari text under the Bengali face.
    FixtureShapingFace bengali(ottest::readFixture("NotoSansBengali-Regular.layout"));
    Seam::face = &bengali;
    ParsedText text(false, false, noIndentStyle());
    text.setShapingLatch(&latch);
    add(text, {{kKi}, {kHindi}});
    EXPECT_EQ(1u, layOut(text, renderer, 400).size());
    EXPECT_EQ(2u, text.tokensShaped());
    EXPECT_EQ(0, bengali.planBuilds());
    EXPECT_FALSE(latch);
    Seam::face = nullptr;
  }
}

TEST_F(ShapedLayout, TheGlyphsAreWarmedOncePerShaping) {
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kHindi}, {kPatrika}, {kKa + kEmDash + kLa}});
  EXPECT_TRUE(layOut(text, renderer, 400, /*includeLastLine=*/false).empty());
  EXPECT_EQ(4, renderer.prefetchedRuns);  // हिन्दी, पत्रिका, and क and ल
  EXPECT_EQ(1u, layOut(text, renderer, 400).size());
  EXPECT_EQ(4, renderer.prefetchedRuns);  // the second pass reused them, warm
}

TEST_F(ShapedLayout, ABidiParagraphShapesItsIndicWords) {
  // A paragraph that needs bidi shapes its Indic words (PR 4b): the line pass cuts each token at its pieces' edges, so
  // the Hindi word beside the Hebrew one is a glyph word as it is in a paragraph without bidi.
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{"שלום"}, {kHindi}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  EXPECT_TRUE(text.shapingStateHeld());
  EXPECT_EQ(1u, text.tokensShaped());
  EXPECT_FALSE(latch);
  bool found = false;
  for (uint16_t i = 0; i < lines[0]->wordCount(); ++i) {
    if (lines[0]->wordText(i) != kHindi) continue;
    found = true;
    ASSERT_TRUE(lines[0]->hasGlyphForm(i));
    EXPECT_EQ(shapedPx(kHindi), glyphRunAdvance(lines[0]->glyphForm(i), false));
    EXPECT_FALSE(lines[0]->hasDisplayForm(i));
  }
  EXPECT_TRUE(found);
}

TEST_F(ShapedLayout, ALatinParagraphMakesNoShapingState) {
  ParsedText text(false, false, noIndentStyle());
  add(text, {{"hello"}, {"world"}});
  EXPECT_EQ(1u, layOut(text, renderer, 400).size());
  EXPECT_FALSE(text.shapingStateHeld());
  EXPECT_EQ(0u, text.tokensShaped());
  EXPECT_EQ(0, renderer.prefetchedRuns);
}

// ---- The build's word cache (WordShapeCache) ----

namespace {
// The lines' bytes, as the section file holds them.
std::vector<std::string> bytesOfLines(const Lines& lines) {
  std::vector<std::string> out;
  for (const auto& line : lines) out.push_back(bytesOf(*line));
  return out;
}
}  // namespace

TEST_F(ShapedLayout, TheWordCacheShapesARepeatedWordOnce) {
  const std::vector<Word> words = {{kHindi}, {kPatrika}, {kHindi}, {kPrarthana}, {kPatrika}, {kHindi}};
  ParsedText plain(false, false, noIndentStyle());
  add(plain, words);
  const std::vector<std::string> expected = bytesOfLines(layOut(plain, renderer, 200));
  EXPECT_EQ(6u, plain.tokensShaped());
  const int prefetched = renderer.prefetchedRuns;

  BuildArena arena(52272);
  WordShapeCache cache;
  ASSERT_TRUE(cache.arm(arena, WordShapeCache::kWordCacheBytes));
  ParsedText text(false, false, noIndentStyle());
  text.setWordCache(&cache);
  add(text, words);
  EXPECT_EQ(expected, bytesOfLines(layOut(text, renderer, 200)));
  EXPECT_EQ(3u, text.tokensShaped());  // each word once
  EXPECT_EQ(3u, cache.stats().misses);
  EXPECT_EQ(3u, cache.stats().inserts);
  EXPECT_EQ(3u, cache.stats().hits);
  EXPECT_EQ(2 * prefetched, renderer.prefetchedRuns);  // a replayed word's glyphs are warmed as a shaped one's

  // The next paragraph shapes none of them.
  text.reset(noIndentStyle());
  add(text, words);
  EXPECT_EQ(expected, bytesOfLines(layOut(text, renderer, 200)));
  EXPECT_EQ(3u, text.tokensShaped());
  EXPECT_EQ(9u, cache.stats().hits);
  EXPECT_EQ(0u, cache.stats().bypassed);
}

TEST_F(ShapedLayout, AGluedTokenIsAlwaysShaped) {
  // क<b>म</b>ल twice: three tokens kept apart, each shaped with its neighbours as context, so never looked up.
  const std::vector<Word> words = {{kKa}, {kMa, EpdFontFamily::BOLD, true}, {kLa, EpdFontFamily::REGULAR, true},
                                   {kKa}, {kMa, EpdFontFamily::BOLD, true}, {kLa, EpdFontFamily::REGULAR, true}};
  ParsedText plain(false, false, noIndentStyle());
  add(plain, words);
  const std::vector<std::string> expected = bytesOfLines(layOut(plain, renderer, 400));

  BuildArena arena(52272);
  WordShapeCache cache;
  ASSERT_TRUE(cache.arm(arena, WordShapeCache::kWordCacheBytes));
  ParsedText text(false, false, noIndentStyle());
  text.setWordCache(&cache);
  add(text, words);
  EXPECT_EQ(expected, bytesOfLines(layOut(text, renderer, 400)));
  EXPECT_EQ(plain.tokensShaped(), text.tokensShaped());
  EXPECT_EQ(6u, text.tokensShaped());
  EXPECT_EQ(0u, cache.stats().hits + cache.stats().misses + cache.stats().bypassed + cache.stats().inserts +
                    cache.stats().oversize);
}

TEST_F(ShapedLayout, AReplayTheStoreRefusesIsShapedAfresh) {
  BuildArena arena(52272);
  WordShapeCache cache;
  ASSERT_TRUE(cache.arm(arena, WordShapeCache::kWordCacheBytes));
  {
    ParsedText warm(false, false, noIndentStyle());
    warm.setWordCache(&cache);
    add(warm, {{kHindi}});
    layOut(warm, renderer, 400);
    ASSERT_EQ(1u, cache.stats().inserts);
  }
  // The store's chunks refused: the replay of हिन्दी stops at its first record. It is shaped afresh, which the same
  // refusal degrades, as it does without the cache: the same lines, the same latch.
  const auto refusedLayout = [&](WordShapeCache* wordCache, bool* latched, uint32_t* shaped) {
    ParsedText text(false, false, noIndentStyle());
    text.setShapingLatch(latched);
    text.setWordCache(wordCache);
    add(text, {{kHindi}, {kPatrika}});
    Lines lines;
    {
      const AllocationRefusal refused(ShapedStore::kChunkBytes, ShapedStore::kChunkBytes);
      lines = layOut(text, renderer, 400);
    }
    *shaped = text.tokensShaped();
    return bytesOfLines(lines);
  };
  bool latchedPlain = false, latchedCached = false;
  uint32_t shapedPlain = 0, shapedCached = 0;
  const std::vector<std::string> plain = refusedLayout(nullptr, &latchedPlain, &shapedPlain);
  const std::vector<std::string> cached = refusedLayout(&cache, &latchedCached, &shapedCached);
  EXPECT_EQ(plain, cached);
  EXPECT_TRUE(latchedPlain);
  EXPECT_EQ(latchedPlain, latchedCached);
  EXPECT_EQ(2u, shapedPlain);
  EXPECT_EQ(shapedPlain, shapedCached);  // हिन्दी shaped although the cache held it
  EXPECT_EQ(0u, cache.stats().hits);
  EXPECT_EQ(3u, cache.stats().misses);  // the warm-up's, the refused replay, पत्रिका
}

namespace {

const std::string kComma = ",";
const std::string kSoftHyphen = "\xC2\xAD";
const std::string kBengaliKa = "\xE0\xA6\x95";              // ক
const std::string kBengaliKe = "\xE0\xA6\x95\xE0\xA7\x87";  // কে
const std::string kBangla = "বাংলা";                        // IndicReorder leaves it as it is
const std::string kBengaliHindi = "হিন্দি";                  // whose i-kars IndicReorder moves before their consonants

// A glyph form's records, as the oracle gives them.
std::vector<StoredGlyph> recordsOf(const GlyphRun& run) {
  std::vector<StoredGlyph> out;
  size_t pos = 0;
  for (uint16_t i = 0; i < run.count; ++i) {
    GlyphRecord record;
    size_t read = 0;
    if (!glyph_run::decode(run.records + pos, run.bytes - pos, &record, &read)) break;
    pos += read;
    out.push_back({record.glyph, record.advance, record.dx, record.dy});
  }
  return out;
}

int advanceOf(const TextBlock& line, const uint16_t i) { return glyphRunAdvance(line.glyphForm(i), false); }

std::unique_ptr<TextBlock> readBack(const std::string& bytes) {
  FsFile file = HalFile::forReadWrite();
  file.write(bytes.data(), bytes.size());
  if (!file.seek(0)) return nullptr;
  serialization::BufferedFileReader reader(file);
  return TextBlock::deserialize(reader);
}

std::string reordered(const std::string& text) {
  std::string out;
  EXPECT_TRUE(indicReorderForDisplay(text.c_str(), out)) << text;
  return out;
}

// The pieces on the line (steps 7-9), on ShapedLayout's fixture.
class ShapedLine : public ShapedLayout {
 protected:
  // What ot::shape gives `text` with `before` and `after` as its context, as a glued neighbour kept apart is, on
  // `shaping` in the script's default language at kPpem.
  static std::vector<StoredGlyph> withContext(FixtureShapingFace& shaping, const ot::Script script,
                                              const std::string& before, const std::string& text,
                                              const std::string& after) {
    const ot::Plan* plan = shaping.plan(script, nullptr);
    const ot::Face* face = shaping.current();
    if (plan == nullptr || face == nullptr) {
      ADD_FAILURE() << "the face does not shape";
      return {};
    }
    const auto contextBefore = static_cast<unsigned>(codepoints(before).size());
    const auto contextAfter = static_cast<unsigned>(codepoints(after).size());
    const std::vector<uint32_t> cps = codepoints(before + text + after);
    ottest::BoundBuffer bound(256);
    ot::Scale scale;
    scale.set(static_cast<int32_t>(kPpem), (kPpem + 32) >> 6, face->upem());
    if (!ot::shape(*face, scale, *plan, cps.data(), static_cast<unsigned>(cps.size()), bound.buffer,
                   ot::RunContext{contextBefore, contextAfter, false})) {
      ADD_FAILURE() << "ot::shape failed: " << text;
      return {};
    }
    std::vector<StoredGlyph> out;
    for (unsigned i = 0; i < bound.buffer.len(); ++i) {
      out.push_back({bound.buffer.info[i].codepoint, (bound.buffer.pos[i].xAdvance + 2) >> 2,
                     ottest::roundPixels(bound.buffer.pos[i].xOffset),
                     -ottest::roundPixels(bound.buffer.pos[i].yOffset)});
    }
    return out;
  }
};

}  // namespace

TEST_F(ShapedLine, AWordThatIsOneRunStaysOneWord) {
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kHindi}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(1u, lines[0]->wordCount());
  // Step 9: drawn as its records, its logical text kept, through the section file too.
  const std::unique_ptr<TextBlock> restored = readBack(bytesOf(*lines[0]));
  for (const TextBlock* line : {lines[0].get(), restored.get()}) {
    ASSERT_NE(nullptr, line);
    ASSERT_TRUE(line->hasGlyphForm(0));
    EXPECT_EQ(oracle(kHindi), recordsOf(line->glyphForm(0)));
    EXPECT_EQ(kHindi, line->wordText(0));
    EXPECT_EQ(kHindi, line->wordDrawText(0));
    EXPECT_FALSE(line->hasDisplayForm(0));
  }
}

TEST_F(ShapedLine, PunctuationAfterAWordIsAGluedPiece) {
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kPatrika}, {kHindi + kComma}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(3u, line.wordCount());
  EXPECT_EQ(kHindi, line.wordText(1));
  EXPECT_FALSE(line.wordContinues(1));  // the token's own boundary: a space before it
  EXPECT_EQ(oracle(kHindi), recordsOf(line.glyphForm(1)));
  EXPECT_EQ(kComma, line.wordText(2));
  EXPECT_TRUE(line.wordContinues(2));
  EXPECT_EQ(line.wordXpos(1) + advanceOf(line, 1), line.wordXpos(2));
  EXPECT_FALSE(line.hasGlyphForm(2));
  EXPECT_FALSE(line.hasDisplayForm(2));
}

TEST_F(ShapedLine, ATokenOfTwoScriptsBecomesTwoPieces) {
  // The Devanagari face draws the first run. The second is of a script it does not map: IndicReorder's form when that
  // changes it, else its text.
  for (const std::string& bengali : {kBengaliHindi, kBangla}) {
    ParsedText text(false, false, noIndentStyle());
    add(text, {{kHindi + bengali}});
    const Lines lines = layOut(text, renderer, 400);
    ASSERT_EQ(1u, lines.size()) << bengali;
    const TextBlock& line = *lines[0];
    ASSERT_EQ(2u, line.wordCount()) << bengali;
    EXPECT_EQ(oracle(kHindi), recordsOf(line.glyphForm(0)));
    EXPECT_EQ(bengali, line.wordText(1));
    EXPECT_TRUE(line.wordContinues(1));
    EXPECT_EQ(line.wordXpos(0) + advanceOf(line, 0), line.wordXpos(1));
    EXPECT_FALSE(line.hasGlyphForm(1));
    const bool reorders = bengali == kBengaliHindi;
    EXPECT_EQ(reorders, line.hasDisplayForm(1)) << bengali;
    EXPECT_EQ(reorders ? reordered(bengali) : bengali, line.wordDrawText(1));
  }
}

TEST_F(ShapedLine, JustificationStretchesTokensNotPieces) {
  // The first of two lines is justified: the gap between its tokens takes what the line lacks, its pieces stay where
  // their widths put them.
  const int hindi = shapedPx(kHindi);
  const int natural = hindi + kGlyph + kSpace + shapedPx(kPatrika);
  constexpr int kSpare = 4;
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kHindi + kComma}, {kPatrika}, {kHindi}, {kPatrika}});
  const Lines lines = layOut(text, renderer, natural + kSpare);
  ASSERT_EQ(2u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(3u, line.wordCount());
  EXPECT_EQ(line.wordXpos(0) + hindi, line.wordXpos(1));
  EXPECT_EQ(line.wordXpos(1) + kGlyph + kSpace + kSpare, line.wordXpos(2));
}

TEST_F(ShapedLine, AnEmphasisRunCoversThePiecesOfItsTokens) {
  // The run's token follows a token of two pieces: it starts at the line's third word.
  ParsedText text(false, false, noIndentStyle());
  text.addWord(kHindi + kComma, EpdFontFamily::REGULAR);
  text.addWord(kHindi + kComma, EpdFontFamily::REGULAR, false, false, 100, 0, 0, /*textEmphasis=*/1);
  text.addWord(kPatrika, EpdFontFamily::REGULAR);
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(5u, line.wordCount());
  ASSERT_EQ(1u, line.annotationCount());
  const TextBlock::Annotation run = line.annotation(0);
  EXPECT_EQ(TextBlock::AnnotationKind::Emphasis, run.kind);
  EXPECT_EQ(2, run.firstWord);
  EXPECT_EQ(2, run.wordCount);
}

TEST_F(ShapedLine, ARubyCoversThePiecesOfItsBase) {
  // The base follows a token of two pieces: its ruby starts at the line's third word.
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kHindi + kComma}, {kHindi + kComma}, {kPatrika}});
  ASSERT_TRUE(text.addRubyGroup(1, 1, "hindi"));
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(5u, line.wordCount());
  ASSERT_EQ(1u, line.annotationCount());
  const TextBlock::Annotation ruby = line.annotation(0);
  EXPECT_EQ(TextBlock::AnnotationKind::Ruby, ruby.kind);
  EXPECT_EQ(2, ruby.firstWord);
  EXPECT_EQ(2, ruby.wordCount);
  EXPECT_EQ("hindi", ruby.text);
}

TEST_F(ShapedLine, ALineWithoutAFormKeepsItsBytes) {
  // In a Hindi paragraph a line of Latin words is laid out as every line was before: its bytes are those the same
  // words give in a Latin paragraph.
  const int width = std::max(shapedPx(kHindi), 5 * kGlyph + kSpace);
  ASSERT_GT(shapedPx(kHindi) + kSpace + 3 * kGlyph, width);  // हिन्दी takes a line of its own
  ParsedText hindi(false, false, noIndentStyle());
  add(hindi, {{kHindi}, {"abc"}});
  hindi.addWord("de", EpdFontFamily::ITALIC, false, false, 80);
  const Lines hindiLines = layOut(hindi, renderer, width);
  ASSERT_EQ(2u, hindiLines.size());
  EXPECT_TRUE(hindiLines[0]->hasGlyphForm(0));

  ParsedText latin(false, false, noIndentStyle());
  add(latin, {{"abc"}});
  latin.addWord("de", EpdFontFamily::ITALIC, false, false, 80);
  const Lines latinLines = layOut(latin, renderer, width);
  ASSERT_EQ(1u, latinLines.size());
  EXPECT_EQ(bytesOf(*latinLines[0]), bytesOf(*hindiLines[1]));
}

TEST_F(ShapedLine, GluedPiecesTakeNoKerning) {
  // Step 8: the engine kerns by code point, and the shaper kerned inside its runs. Every pair kerns 3 px here.
  renderer.kerningPx = 3;
  ParsedText text(false, false, noIndentStyle());
  // हिन्दी, then » glued in bold (another style between syllables: a token of its own), then क and म, glued in bold;
  // then «, हिन्दी glued in bold, and », glued in regular.
  add(text, {{kHindi + kComma},
             {"»", EpdFontFamily::BOLD, true},
             {kKa},
             {kMa, EpdFontFamily::BOLD, true},
             {"«"},
             {kHindi, EpdFontFamily::BOLD, true},
             {"»", EpdFontFamily::REGULAR, true}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(8u, line.wordCount());
  // The pieces of one token touch.
  EXPECT_EQ(line.wordXpos(0) + advanceOf(line, 0), line.wordXpos(1));
  // Two tokens glued at a Text piece and a word of code points: their code points kern.
  EXPECT_EQ(line.wordXpos(1) + kGlyph + renderer.kerningPx, line.wordXpos(2));
  // Two tokens glued across a Glyphs edge: no kerning.
  ASSERT_TRUE(line.hasGlyphForm(3));
  ASSERT_TRUE(line.hasGlyphForm(4));
  EXPECT_EQ(line.wordXpos(3) + advanceOf(line, 3), line.wordXpos(4));
  // A Glyphs edge on one side alone: no kerning either, after the code points and before them.
  ASSERT_FALSE(line.hasGlyphForm(5));
  ASSERT_TRUE(line.hasGlyphForm(6));
  ASSERT_FALSE(line.hasGlyphForm(7));
  EXPECT_EQ(line.wordXpos(5) + kGlyph, line.wordXpos(6));
  EXPECT_EQ(line.wordXpos(6) + advanceOf(line, 6), line.wordXpos(7));
}

TEST_F(ShapedLine, TheLinesTokenCountIsItsTokens) {
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kHindi + kComma}, {kPatrika}});
  Lines lines;
  std::vector<uint32_t> tokens;
  text.layoutAndExtractLines(renderer, 1, 400, [&](std::unique_ptr<TextBlock> line, bool, bool) {
    tokens.push_back(text.lastLineTokenCount());
    lines.push_back(std::move(line));
    return ParsedText::LineProcessResult::Accepted;
  });
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(3u, lines[0]->wordCount());
  EXPECT_EQ(std::vector<uint32_t>{2}, tokens);
}

TEST_F(ShapedLine, ASoftHyphenInsideATokenIsNoWordOfItsOwn) {
  {
    // The run scan cuts at it, and its Text piece is of no width: the runs on either side are glued pieces.
    ParsedText text(false, false, noIndentStyle());
    add(text, {{"हिन्" + kSoftHyphen + "दी"}});
    const Lines lines = layOut(text, renderer, 400);
    ASSERT_EQ(1u, lines.size());
    const TextBlock& line = *lines[0];
    ASSERT_EQ(2u, line.wordCount());
    EXPECT_EQ("हिन्", std::string(line.wordText(0)));
    EXPECT_EQ("दी", std::string(line.wordText(1)));
    EXPECT_TRUE(line.hasGlyphForm(0));
    EXPECT_TRUE(line.hasGlyphForm(1));
    EXPECT_TRUE(line.wordContinues(1));
    EXPECT_EQ(line.wordXpos(0) + advanceOf(line, 0), line.wordXpos(1));
  }
  {
    // Without a face the token is one Reordered piece, drawn as its form: a soft hyphen is no more drawn in it than in
    // a word's text.
    Seam::face = nullptr;
    ParsedText text(false, false, noIndentStyle());
    add(text, {{kKi + kSoftHyphen + kKa}});
    const Lines lines = layOut(text, renderer, 400);
    ASSERT_EQ(1u, lines.size());
    const TextBlock& line = *lines[0];
    ASSERT_EQ(1u, line.wordCount());
    EXPECT_EQ(kKi + kKa, line.wordText(0));
    EXPECT_TRUE(line.hasDisplayForm(0));
    EXPECT_EQ(reordered(kKi + kKa), line.wordDrawText(0));
  }
}

TEST_F(ShapedLine, AStyleChangeBetweenSyllablesIsShapedWithItsNeighbour) {
  // क<b>म</b>: the glue falls between syllables, so the two stay words of their own styles (deviation 7), each shaped
  // with the other as its context.
  {
    ParsedText text(false, false, noIndentStyle());
    add(text, {{kKa}, {kMa, EpdFontFamily::BOLD, true}});
    const Lines lines = layOut(text, renderer, 400);
    ASSERT_EQ(1u, lines.size());
    const TextBlock& line = *lines[0];
    ASSERT_EQ(2u, line.wordCount());
    EXPECT_EQ(EpdFontFamily::REGULAR, line.wordStyle(0));
    EXPECT_EQ(EpdFontFamily::BOLD, line.wordStyle(1));
    EXPECT_EQ(withContext(*face, ot::Script::Devanagari, "", kKa, kMa), recordsOf(line.glyphForm(0)));
    EXPECT_EQ(withContext(*face, ot::Script::Devanagari, kKa, kMa, ""), recordsOf(line.glyphForm(1)));
  }
  {
    // ক<b>কে</b> on the Bengali face: after a letter the e-kar takes no word-start form, so the context shows.
    FixtureShapingFace bengali(ottest::readFixture("NotoSansBengali-Regular.layout"));
    Seam::face = &bengali;
    ParsedText text(false, false, noIndentStyle());
    add(text, {{kBengaliKa}, {kBengaliKe, EpdFontFamily::BOLD, true}});
    const Lines lines = layOut(text, renderer, 400);
    ASSERT_EQ(1u, lines.size());
    const TextBlock& line = *lines[0];
    ASSERT_EQ(2u, line.wordCount());
    const std::vector<StoredGlyph> glued = withContext(bengali, ot::Script::Bengali, kBengaliKa, kBengaliKe, "");
    ASSERT_NE(withContext(bengali, ot::Script::Bengali, "", kBengaliKe, ""), glued);
    EXPECT_EQ(glued, recordsOf(line.glyphForm(1)));
    Seam::face = nullptr;
  }
}

TEST_F(ShapedLine, AWordWiderThanTheLineSplitsBetweenSyllables) {
  // Review Focus 3: each part is a glyph form of its own text's shaping; its hyphen is a glued piece of code points.
  const std::string compound = kHindi + kPatrika + kPrarthana + kGyan + kSanskritam;
  ParsedText text(false, false, noIndentStyle());
  add(text, {{compound}});
  const Lines lines = layOut(text, renderer, 60);
  ASSERT_GE(lines.size(), 2u);
  std::string joined;
  for (size_t k = 0; k < lines.size(); ++k) {
    const TextBlock& line = *lines[k];
    const bool last = k + 1 == lines.size();
    ASSERT_EQ(last ? 1u : 2u, line.wordCount()) << k;
    const std::string part = line.wordText(0);
    ASSERT_TRUE(line.hasGlyphForm(0)) << k;
    EXPECT_EQ(oracle(part), recordsOf(line.glyphForm(0))) << k << ": " << part;
    joined += part;
    if (last) continue;
    EXPECT_STREQ("-", line.wordText(1)) << k;
    EXPECT_TRUE(line.wordContinues(1)) << k;
    EXPECT_FALSE(line.hasGlyphForm(1)) << k;
    EXPECT_FALSE(line.hasDisplayForm(1)) << k;
    EXPECT_EQ(line.wordXpos(0) + advanceOf(line, 0), line.wordXpos(1)) << k;
  }
  EXPECT_EQ(compound, joined);
}

TEST_F(ShapedLine, ABurmesePhraseIsCutAtTheLineEndBetweenSyllables) {
  // The Myanmar design's PR 3, with hyphenation off: a phrase that does not fit at the line's end is cut there before
  // a syllable, with no hyphen, at the widest prefix its shaped width allows (shapedPrefixWidth), and each part is the
  // glyph form of its own text shaped alone, as the token it becomes.
  FixtureShapingFace& burmese = useFont("NotoSansMyanmar-Book-Regular.ttf");
  const auto shaped = [&](const std::string& text) { return oracleOn(burmese, ot::Script::Myanmar, text); };
  const std::string first = "လူတိုင်း";
  const std::string phrase = "အခြေခံအုတ်မြစ်ဖြစ်သောကြောင့်လည်းကောင်း၊";
  const int room = advancePx(shaped(phrase)) / 2;
  const int width = advancePx(shaped(first)) + kSpace + room;
  ParsedText text(false, false, noIndentStyle());
  add(text, {{first}, {phrase}});
  const Lines lines = layOut(text, renderer, width);
  ASSERT_EQ(2u, lines.size());
  ASSERT_EQ(2u, lines[0]->wordCount());
  ASSERT_EQ(1u, lines[1]->wordCount());
  const std::string part = lines[0]->wordText(1);
  const std::string rest = lines[1]->wordText(0);
  EXPECT_EQ(phrase, part + rest);

  std::vector<size_t> breaks;
  for (const Hyphenator::BreakInfo& b : Hyphenator::myanmarLineEndBreaks(phrase)) breaks.push_back(b.byteOffset);
  const auto at = std::find(breaks.begin(), breaks.end(), part.size());
  ASSERT_NE(breaks.end(), at) << "not cut before a syllable: " << part;
  EXPECT_LE(advancePx(shaped(part)), room) << part;
  ASSERT_NE(breaks.end(), at + 1);
  EXPECT_GT(advancePx(shaped(phrase.substr(0, *(at + 1)))), room) << "a longer prefix fits";

  ASSERT_TRUE(lines[0]->hasGlyphForm(1));
  ASSERT_TRUE(lines[1]->hasGlyphForm(0));
  EXPECT_EQ(shaped(part), recordsOf(lines[0]->glyphForm(1)));
  EXPECT_EQ(shaped(rest), recordsOf(lines[1]->glyphForm(0)));
}

TEST_F(ShapedLine, ASuperscriptIsShapedAtItsDrawnSize) {
  // Records are in drawn pixels: a superscript at 50% is shaped at half the ppem; SUP halves nothing more.
  ASSERT_NE(oracle(kHindi, kPpem), oracle(kHindi, kPpem / 2));
  ParsedText text(false, false, noIndentStyle());
  text.addWord(kPatrika, EpdFontFamily::REGULAR);
  text.addWord(kHindi, EpdFontFamily::SUP, false, false, 50);
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(2u, line.wordCount());
  EXPECT_EQ(oracle(kPatrika), recordsOf(line.glyphForm(0)));
  EXPECT_EQ(oracle(kHindi, kPpem / 2), recordsOf(line.glyphForm(1)));
}

TEST_F(ShapedLine, AFontThatCannotShapeReordersAndLatchesNothing) {
  // Review Focus 5: a .cpfont or built-in font, then a TrueType family without the script. The token is one word
  // drawn as IndicReorder's form.
  FixtureShapingFace bengali(ottest::readFixture("NotoSansBengali-Regular.layout"));
  for (ShapingFace* shaping : {static_cast<ShapingFace*>(nullptr), static_cast<ShapingFace*>(&bengali)}) {
    Seam::face = shaping;
    latch = false;
    ParsedText text(false, false, noIndentStyle());
    text.setShapingLatch(&latch);
    add(text, {{kKi}});
    const Lines lines = layOut(text, renderer, 400);
    ASSERT_EQ(1u, lines.size());
    const TextBlock& line = *lines[0];
    ASSERT_EQ(1u, line.wordCount());
    EXPECT_FALSE(line.hasGlyphForm(0));
    EXPECT_EQ(kKi, line.wordText(0));
    EXPECT_EQ(kIMatra + kKa, line.wordDrawText(0));
    EXPECT_FALSE(latch);
  }
  EXPECT_EQ(0, bengali.planBuilds());
  Seam::face = nullptr;
}

TEST_F(ShapedLine, AVerticalLineReadsBackWithItsReorderedForms) {
  // A vertical line's load refuses glyph forms: its Indic words are drawn as IndicReorder's forms, which it loads.
  ParsedText text(false, false, noIndentStyle());
  text.setVertical(true);
  add(text, {{kKi}, {kHindi}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_FALSE(lines.empty());
  bool reordered = false;
  for (const auto& line : lines) {
    line->setVertical(true);  // as the page's builder sets it
    const std::unique_ptr<TextBlock> restored = readBack(bytesOf(*line));
    ASSERT_NE(nullptr, restored);
    EXPECT_TRUE(restored->isVertical());
    for (uint16_t i = 0; i < restored->wordCount(); ++i) {
      EXPECT_FALSE(restored->hasGlyphForm(i)) << restored->wordText(i);
      reordered = reordered || restored->hasDisplayForm(i);
      if (restored->wordText(i) == kKi) EXPECT_EQ(kIMatra + kKa, restored->wordDrawText(i));
    }
  }
  EXPECT_TRUE(reordered);
}

TEST_F(ShapedLine, AFaceReleasedBetweenTwoPassesShapesAfresh) {
  // Review Focus 1: the font set reloads between two slices of a build. The token kept for the last pass reuses its
  // records, which need no plan; the token added since is shaped with the new face.
  const int width = std::max(shapedPx(kHindi), shapedPx(kPatrika));  // one word a line
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kHindi}, {kPatrika}, {kHindi}});
  const Lines first = layOut(text, renderer, width, /*includeLastLine=*/false);
  ASSERT_EQ(2u, first.size());
  EXPECT_EQ(3u, text.tokensShaped());

  auto fresh =
      std::make_unique<FixtureShapingFace>(readFont(TTF_FIXTURE_DIR, "NotoSansDevanagari-Shaping-Regular.ttf"));
  Seam::face = fresh.get();
  face = std::move(fresh);  // the first pass's face is destroyed
  ASSERT_EQ(0, face->planBuilds());

  add(text, {{kPatrika}});
  const Lines last = layOut(text, renderer, 400);
  ASSERT_EQ(1u, last.size());
  ASSERT_EQ(2u, last[0]->wordCount());
  EXPECT_EQ(4u, text.tokensShaped());  // पत्रिका alone: the kept हिन्दी's records are the first pass's
  EXPECT_EQ(1, face->planBuilds());

  for (const Lines* lines : {&first, &last}) {
    for (const auto& line : *lines) {
      for (uint16_t i = 0; i < line->wordCount(); ++i) {
        ASSERT_TRUE(line->hasGlyphForm(i)) << line->wordText(i);
        EXPECT_EQ(oracle(line->wordText(i)), recordsOf(line->glyphForm(i))) << line->wordText(i);
      }
    }
  }
}

TEST_F(ShapedLine, ALineTheHeapHasNoRoomForKeepsItsBytesAndLatches) {
  // The warm-up, one word a line, leaves the piece vectors room for one piece; the next line holds three, and the heap
  // refuses their growth: the line keeps its bytes, its tokens drawn as their code points.
  const int width = std::max(shapedPx(kHindi), shapedPx(kPatrika));
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{kHindi}, {kPatrika}, {kHindi}});
  ASSERT_EQ(3u, layOut(text, renderer, width).size());
  ASSERT_FALSE(latch);
  text.reset(noIndentStyle());
  add(text, {{kHindi}, {kPatrika}, {kHindi}});
  Lines lines;
  {
    const LargestFreeBlock block(32);
    lines = layOut(text, renderer, 400);
  }
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(3u, lines[0]->wordCount());
  for (uint16_t i = 0; i < 3; ++i) EXPECT_FALSE(lines[0]->hasGlyphForm(i)) << i;
  EXPECT_EQ(kPatrika, lines[0]->wordDrawText(1));
  EXPECT_TRUE(latch);
}

TEST_F(ShapedLine, AFormCopyTheHeapHasNoRoomForKeepsTheLinesBytes) {
  // Without a face a token is one Reordered piece; a soft hyphen in it is drawn from a copy of the form without it
  // (ASoftHyphenInsideATokenIsNoWordOfItsOwn). The warm-up, a longer token without one, leaves every other container
  // the room; the copy of the next token's 44 B form asks for a block of 61 B, which the heap refuses: the line keeps
  // its bytes.
  Seam::face = nullptr;
  std::string eight;
  for (int k = 0; k < 8; ++k) eight += kKi;
  std::string six;
  for (int k = 0; k < 6; ++k) six += kKi;
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{eight}});
  ASSERT_EQ(1u, layOut(text, renderer, 4000).size());
  ASSERT_FALSE(latch);
  text.reset(noIndentStyle());
  add(text, {{six + kSoftHyphen + kKi}});
  Lines lines;
  {
    const LargestFreeBlock block(48);
    lines = layOut(text, renderer, 4000);
  }
  ASSERT_EQ(1u, lines.size());
  ASSERT_EQ(1u, lines[0]->wordCount());
  EXPECT_FALSE(lines[0]->hasDisplayForm(0));
  EXPECT_EQ(six + kKi, lines[0]->wordDrawText(0));
  EXPECT_TRUE(latch);
}

// ---- Bidi paragraphs (writing modes step 2, PR 4b): Arabic words, and Indic words beside right-to-left text, shaped
// on the lines the bidi pass lays out. Arabic is shaped over the Naskh fixture, through the same seam ----

namespace {

const std::string kBeh = "\xD8\xA8";                            // U+0628 beh
const std::string kTeh = "\xD8\xAA";                            // U+062A teh
const std::string kAlef = "\xD8\xA7";                           // U+0627 alef
const std::string kLam = "\xD9\x84";                            // U+0644 lam
const std::string kKaf = "\xD9\x83";                            // U+0643 kaf
const std::string kFatha = "\xD9\x8E";                          // U+064E fatha, a mark
const std::string kKitab = "\xD9\x83\xD8\xAA\xD8\xA7\xD8\xA8";  // kaf teh alef beh: kitab, a book, one run
const std::string kArabicComma = "\xD8\x8C";                    // U+060C: no run member
const std::string kRlm = "\xE2\x80\x8F";                        // U+200F, a bidi control
const std::string kLeftGuillemet = "\xC2\xAB";                  // U+00AB, mirrored
const std::string kRightGuillemet = "\xC2\xBB";                 // U+00BB, mirrored
constexpr const char* kNaskh = "NotoNaskhArabic-Shaping-Regular.ttf";
constexpr const char* kRtlTest = "ArabicRtlTest-Regular.ttf";  // synthetic: its rlig makes lam-alef one ligature

// The index of the line's word whose text is `text`; -1: none.
int wordWithText(const TextBlock& line, const std::string& text) {
  for (uint16_t i = 0; i < line.wordCount(); ++i) {
    if (text == line.wordText(i)) return i;
  }
  return -1;
}

}  // namespace

TEST_F(ShapedLayout, AnArabicWordOnAnRtlLineIsOneGlyphWord) {
  // Kitab is one run: one Glyphs piece, on a line that runs right to left. It is drawn as its records, which are in
  // visual order already (the engine gives an Arabic run last letter first), its logical text kept, through the section
  // file too.
  FixtureShapingFace& naskh = useFont(kNaskh);
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{kKitab}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const std::vector<StoredGlyph> expected = oracleOn(naskh, ot::Script::Arabic, kKitab);
  ASSERT_GT(expected.size(), 1u);
  const std::unique_ptr<TextBlock> restored = readBack(bytesOf(*lines[0]));
  for (const TextBlock* line : {lines[0].get(), restored.get()}) {
    ASSERT_NE(nullptr, line);
    ASSERT_EQ(1u, line->wordCount());
    ASSERT_TRUE(line->hasGlyphForm(0));
    EXPECT_EQ(expected, recordsOf(line->glyphForm(0)));
    EXPECT_EQ(kKitab, line->wordText(0));
    EXPECT_FALSE(line->hasDisplayForm(0));
    EXPECT_EQ(400, line->wordXpos(0) + advancePx(expected));  // the line starts at the right
  }
  EXPECT_EQ(1u, text.tokensShaped());
  EXPECT_FALSE(latch);
}

TEST_F(ShapedLayout, AnAyahSignIsDrawnAroundItsNumberLeftOfTheWord) {
  // Beh, then the end of ayah with the verse number 12, in a paragraph that runs right to left. The sign and its digits
  // are numbers, on a level above the letters, so their word reads left of beh, and it is one glyph word: the sign
  // drawn around the digits, through the section file too. The glyphs are HarfBuzz's (ArabicNumberExpected.h, at
  // another size); at this size the engine's run is the record (two digits make one unit, so its order stands).
  FixtureShapingFace& numbers = useFont(kFontNotoNaskhArabicNumbersRegular);
  const std::string ayah12 = "\xDB\x9D\xD9\xA1\xD9\xA2";  // U+06DD U+0661 U+0662
  std::vector<uint16_t> harfbuzzGlyphs;
  for (const ArabicShapingExpectation& word : kArabicNumberShaping) {
    if (ayah12 != word.utf8) continue;
    for (unsigned i = 0; i < word.count; ++i) harfbuzzGlyphs.push_back(word.glyphs[i].gid);
  }
  ASSERT_EQ(3u, harfbuzzGlyphs.size());  // the sign and the two digits inside it
  const std::vector<StoredGlyph> expected = oracleOn(numbers, ot::Script::Arabic, ayah12);
  ASSERT_EQ(3u, expected.size());
  for (size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(harfbuzzGlyphs[i], expected[i].gid);
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{kBeh}, {ayah12}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const std::unique_ptr<TextBlock> restored = readBack(bytesOf(*lines[0]));
  for (const TextBlock* line : {lines[0].get(), restored.get()}) {
    ASSERT_NE(nullptr, line);
    ASSERT_EQ(2u, line->wordCount());
    EXPECT_EQ(ayah12, line->wordText(0));
    ASSERT_TRUE(line->hasGlyphForm(0));
    EXPECT_EQ(expected, recordsOf(line->glyphForm(0)));
    EXPECT_EQ(kBeh, line->wordText(1));
    EXPECT_LT(line->wordXpos(0), line->wordXpos(1));
  }
  EXPECT_FALSE(latch);
}

TEST_F(ShapedLayout, AWordWithItsCommaIsTwoTouchingWords) {
  // The Arabic comma is outside the run: the token is cut at the run's edge into two glued pieces. In visual order the
  // comma comes first, left of the word, and the two touch.
  FixtureShapingFace& naskh = useFont(kNaskh);
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kKitab + kArabicComma}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(2u, line.wordCount());
  EXPECT_EQ(kArabicComma, line.wordText(0));
  EXPECT_FALSE(line.hasGlyphForm(0));
  EXPECT_EQ(kKitab, line.wordText(1));
  ASSERT_TRUE(line.hasGlyphForm(1));
  EXPECT_EQ(oracleOn(naskh, ot::Script::Arabic, kKitab), recordsOf(line.glyphForm(1)));
  EXPECT_TRUE(line.wordContinues(1));  // glued to its left neighbour
  EXPECT_EQ(line.wordXpos(0) + kGlyph, line.wordXpos(1));
  EXPECT_EQ(400, line.wordXpos(1) + advanceOf(line, 1));
  EXPECT_EQ(1, line.logicalPosition(0));
  EXPECT_EQ(0, line.logicalPosition(1));
}

TEST_F(ShapedLayout, GluedPiecesTakeNoKerning) {
  // Step 8 on a bidi line: the gap between two glued pieces kerns the code points that face each other on the line,
  // unless either piece is a glyph run (the shaper kerned inside it). Every pair kerns 3 px here. Right to left: the
  // bold guillemet glued after the comma is drawn first (mirrored), then the comma, then its word; a space, then the
  // other word with its bracket (mirrored) right of it.
  useFont(kNaskh);
  renderer.kerningPx = 3;
  ParsedText text(false, false, noIndentStyle());
  add(text, {{"(" + kKitab}, {kKitab + kArabicComma}, {kRightGuillemet, EpdFontFamily::BOLD, true}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(5u, line.wordCount());
  EXPECT_EQ(kRightGuillemet, line.wordText(0));
  EXPECT_EQ(kLeftGuillemet, line.wordDrawText(0));
  EXPECT_EQ(kArabicComma, line.wordText(1));
  ASSERT_TRUE(line.hasGlyphForm(2));
  ASSERT_TRUE(line.hasGlyphForm(3));
  EXPECT_EQ("(", std::string(line.wordText(4)));
  EXPECT_FALSE(line.hasGlyphForm(4));
  EXPECT_EQ(line.wordXpos(0) + kGlyph + renderer.kerningPx, line.wordXpos(1));  // two code points: they kern
  EXPECT_EQ(line.wordXpos(1) + kGlyph, line.wordXpos(2));                       // a glyph run on the right
  EXPECT_EQ(line.wordXpos(2) + advanceOf(line, 2) + kSpace, line.wordXpos(3));
  EXPECT_EQ(line.wordXpos(3) + advanceOf(line, 3), line.wordXpos(4));  // a glyph run on the left
}

TEST_F(ShapedLayout, APlainTokenOfABidiParagraphIsMeasuredOnce) {
  // The width pass measures each token's form; the placed line took each piece's display form to the renderer again,
  // the same bytes for a plain token. Now the width pass's figure stands: three plain tokens, three measurements,
  // and the line as before.
  static int advances = 0;
  advances = 0;
  GfxRenderer::onTextAdvance = [] { advances++; };
  ParsedText text(false, false, noIndentStyle());
  add(text, {{"שלום"}, {"עולם"}, {"abc"}});
  const Lines lines = layOut(text, renderer, 400);
  GfxRenderer::onTextAdvance = nullptr;
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(3u, line.wordCount());
  EXPECT_TRUE(line.hasLogicalOrder());
  EXPECT_EQ(3, advances);
}

TEST_F(ShapedLayout, ALineWhoseTokensTheHeapRefusesIsLaidOutWithoutBidi) {
  // The line pass's token vector grows behind the heap check (ParsedText::layOutBidiLine): refused, the line is laid
  // out without bidi, as when the pass has no workspace. A Hebrew paragraph: it needs bidi and shapes nothing, so the
  // refusal is the pass's alone and latches nothing.
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{"שלום"}, {"עולם"}, {"abc"}});
  Lines lines;
  {
    const LargestFreeBlock none(0);
    lines = layOut(text, renderer, 400);
  }
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(3u, line.wordCount());
  EXPECT_FALSE(line.hasLogicalOrder());
  EXPECT_EQ(std::string("שלום"), line.wordText(0));  // logical order, as laid out without bidi
  EXPECT_EQ(std::string("abc"), line.wordText(2));
  EXPECT_FALSE(latch);
  EXPECT_EQ(0u, text.tokensShaped());
}

TEST_F(ShapedLayout, ALentArenaHoldsTheLinesTokensForThePass) {
  // With a lent arena the line's tokens come with the pass's workspace and go with it (ParsedText::layOutBidiLine):
  // the arena's peak shows them, and the layout is the one the heap path gives. A Hebrew paragraph of two lines.
  const std::vector<Word> words = {{"שלום"}, {"עולם"}, {"abc"}, {"שלום"}, {"עולם"}, {"def"}};
  ParsedText onHeap(false, false, noIndentStyle());
  add(onHeap, words);
  const Lines heapLines = layOut(onHeap, renderer, 150);
  ASSERT_GE(heapLines.size(), 1u);

  BuildArena arena(32 * 1024);
  ASSERT_TRUE(arena.valid());
  ParsedText lent(false, false, noIndentStyle());
  lent.setLineArena(&arena);
  add(lent, words);
  arena.markPeak();
  const Lines arenaLines = layOut(lent, renderer, 150);
  EXPECT_GE(arena.peakSinceMark(),
            BidiUtils::kWorkspaceBytes + sizeof(BidiLine::Scratch) + 3 * sizeof(BidiLine::Token));
  ASSERT_EQ(heapLines.size(), arenaLines.size());
  for (size_t l = 0; l < heapLines.size(); l++) {
    ASSERT_EQ(heapLines[l]->wordCount(), arenaLines[l]->wordCount());
    EXPECT_EQ(heapLines[l]->hasLogicalOrder(), arenaLines[l]->hasLogicalOrder());
    for (size_t w = 0; w < heapLines[l]->wordCount(); w++) {
      EXPECT_EQ(std::string(heapLines[l]->wordText(w)), std::string(arenaLines[l]->wordText(w)));
    }
  }
}

TEST_F(ShapedLayout, AParagraphWithLongLinesGivesTheLinePassBuffersBackAtItsEnd) {
  // 140 one-letter tokens on one line take the fallback and grow the line pass's buffers past kKeptLineBytes: the
  // paragraph's end gives them back. A prose paragraph's stay below it and are kept.
  const std::vector<Word> many(140, Word{"א"});
  ParsedText text(false, false, noIndentStyle());
  add(text, many);
  const Lines lines = layOut(text, renderer, 4000);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_GT(text.keptLineBytes(), ParsedText::kKeptLineBytes);
  text.releaseOversizedLayoutBuffers();
  EXPECT_EQ(text.keptLineBytes(), 0u);

  ParsedText prose(false, false, noIndentStyle());
  add(prose, {{"שלום"}, {"עולם"}, {"abc"}});
  const Lines proseLines = layOut(prose, renderer, 400);
  ASSERT_EQ(1u, proseLines.size());
  const size_t kept = prose.keptLineBytes();
  EXPECT_GT(kept, 0u);
  EXPECT_LE(kept, ParsedText::kKeptLineBytes);
  prose.releaseOversizedLayoutBuffers();
  EXPECT_EQ(prose.keptLineBytes(), kept);
}

TEST_F(ShapedLayout, AParagraphWithABigStoreGivesItBackAtItsEnd) {
  // 400 shaped tokens hold an index past kKeptStoreBytes: the paragraph's end releases the store. A short
  // paragraph's store stays, its chunks the next paragraph's.
  useFont(kNaskh);
  const std::vector<Word> many(400, Word{kKitab});
  ParsedText text(false, false, noIndentStyle());
  add(text, many);
  const Lines lines = layOut(text, renderer, 4000);
  ASSERT_GE(lines.size(), 1u);
  EXPECT_GT(text.tokensShaped(), 0u);
  EXPECT_GT(text.keptStoreBytes(), ParsedText::kKeptStoreBytes);
  text.releaseOversizedLayoutBuffers();
  EXPECT_EQ(text.keptStoreBytes(), 0u);

  ParsedText prose(false, false, noIndentStyle());
  add(prose, {{kKitab}, {kKitab}, {kKitab}});
  layOut(prose, renderer, 400);
  const size_t kept = prose.keptStoreBytes();
  EXPECT_GT(kept, 0u);
  EXPECT_LE(kept, ParsedText::kKeptStoreBytes);
  prose.releaseOversizedLayoutBuffers();
  EXPECT_EQ(prose.keptStoreBytes(), kept);
}

TEST_F(ShapedLayout, JustificationStretchesTokensNotPieces) {
  // On a justified right-to-left line the gap between the tokens takes what the line lacks; the comma stays touching
  // its word.
  FixtureShapingFace& naskh = useFont(kNaskh);
  const int kitab = advancePx(oracleOn(naskh, ot::Script::Arabic, kKitab));
  const int natural = kitab + kGlyph + kSpace + kitab;
  constexpr int kSpare = 4;
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kKitab + kArabicComma}, {kKitab}, {kKitab}, {kKitab}});
  const Lines lines = layOut(text, renderer, natural + kSpare);
  ASSERT_EQ(2u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(3u, line.wordCount());
  EXPECT_EQ(0, line.wordXpos(0));
  EXPECT_EQ(line.wordXpos(0) + kitab + kSpace + kSpare, line.wordXpos(1));
  EXPECT_EQ(kArabicComma, line.wordText(1));
  EXPECT_EQ(line.wordXpos(1) + kGlyph, line.wordXpos(2));
  EXPECT_EQ(natural + kSpare, line.wordXpos(2) + kitab);
}

TEST_F(ShapedLayout, AWithoutBidiLineDrawsOrDegrades) {
  // The heap refuses the line pass its Scratch (and there is no arena): every line is laid out without bidi, its tokens
  // in logical order. A paragraph that shapes still draws its tokens as their pieces, a Text piece without its
  // controls; nothing is drawn as code points at a glyph run's width.
  FixtureShapingFace& naskh = useFont(kNaskh);
  BidiLine::releaseBuildMemory();
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{kKitab + kRlm + kArabicComma}, {kKitab}});
  Lines lines;
  {
    const AllocationRefusal refused(sizeof(BidiLine::Scratch), sizeof(BidiLine::Scratch));
    lines = layOut(text, renderer, 400);
  }
  EXPECT_FALSE(BidiLine::heapScratchHeld());
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(3u, line.wordCount());
  const std::vector<StoredGlyph> expected = oracleOn(naskh, ot::Script::Arabic, kKitab);
  ASSERT_TRUE(line.hasGlyphForm(0));
  EXPECT_EQ(expected, recordsOf(line.glyphForm(0)));
  EXPECT_EQ(kArabicComma, line.wordDrawText(1));  // the RLM is not drawn
  EXPECT_FALSE(line.hasGlyphForm(1));
  ASSERT_TRUE(line.hasGlyphForm(2));
  EXPECT_FALSE(line.hasLogicalOrder());
  EXPECT_TRUE(line.wordContinues(1));
  EXPECT_EQ(line.wordXpos(0) + advancePx(expected), line.wordXpos(1));
  EXPECT_EQ(line.wordXpos(1) + kGlyph + kSpace, line.wordXpos(2));
  EXPECT_EQ(2u, text.tokensShaped());
  EXPECT_FALSE(latch);
}

TEST_F(ShapedLayout, TheBidiScratchAndACallsBlockNeverShareTheArena) {
  // Risk 2: a paragraph that needs bidi borrows the line pass's workspace (BidiUtils' and a Scratch) from the lent
  // arena while it measures its tokens. Its tokens are shaped before that, each call's block the arena's for the call
  // alone, so the arena's peak up to the first line is the larger of the two blocks, never their sum.
  useFont(kNaskh);
  const std::string longest = kKitab + kKitab + kArabicComma;
  size_t callBytes = 0;
  {
    BuildArena probe(16 * 1024);
    ASSERT_TRUE(probe.valid());
    IndicShaper::CallMemory memory(&probe, longest.size());
    ASSERT_TRUE(memory.valid());
    callBytes = probe.used();
  }
  const size_t workspaceBytes = BidiUtils::kWorkspaceBytes + sizeof(BidiLine::Scratch);
  BuildArena arena(52272);
  ASSERT_TRUE(arena.valid());
  ParsedText text(false, false, noIndentStyle());
  text.setLineArena(&arena);
  text.setShapingLatch(&latch);
  size_t peak = 0;
  text.setBeforeLineHook([&](uint8_t, bool) {
    if (peak == 0) peak = arena.highWater();
  });
  add(text, {{kKitab}, {longest}, {kBeh + kAlef}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(3u, text.tokensShaped());
  EXPECT_FALSE(TextShaping::buildMemoryHeld());  // every call's block was the arena's
  EXPECT_GE(peak, workspaceBytes);               // and so was the workspace
  EXPECT_GE(peak, callBytes);
  EXPECT_LT(peak, workspaceBytes + callBytes);
  EXPECT_FALSE(latch);
}

TEST_F(ShapedLayout, AnArabicParagraphWithoutAFaceKeepsItsBytes) {
  // My call 5: an Arabic paragraph shapes only where one of its Arabic tokens has a face. In a bitmap or built-in font
  // it makes no ShapingState, merges no mark-only span (the merge is a shaped group's) and cuts no token: its line is
  // step 1's, each token the line pass's pieces of its code points. With a face, the same words merge and are cut.
  const std::vector<Word> words = {{kBeh}, {kFatha, EpdFontFamily::BOLD, true}, {kKitab + kArabicComma}};
  Seam::face = nullptr;
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, words);
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  EXPECT_FALSE(text.shapingStateHeld());
  EXPECT_EQ(0u, text.tokensShaped());
  EXPECT_FALSE(latch);
  const TextBlock& line = *lines[0];
  for (uint16_t i = 0; i < line.wordCount(); ++i) EXPECT_FALSE(line.hasGlyphForm(i)) << i;
  EXPECT_GE(wordWithText(line, kFatha), 0);                 // a token of its own
  EXPECT_GE(wordWithText(line, kKitab + kArabicComma), 0);  // one piece

  useFont(kNaskh);
  ParsedText shaped(false, false, noIndentStyle());
  add(shaped, words);
  const Lines shapedLines = layOut(shaped, renderer, 400);
  ASSERT_EQ(1u, shapedLines.size());
  EXPECT_GE(wordWithText(*shapedLines[0], kBeh + kFatha), 0);
  EXPECT_GE(wordWithText(*shapedLines[0], kKitab), 0);
  EXPECT_GE(wordWithText(*shapedLines[0], kArabicComma), 0);
}

TEST_F(ShapedLine, AnRtlWordsLookupTextIsInReadingOrder) {
  // Kaf, a bold teh, then alef beh: three tokens kept apart (a style change between joined letters), each shaped with
  // its neighbours as context, touching. A right-to-left line stores its pieces in visual order, so the word's pieces
  // joined as stored read alef beh teh kaf; joined by their logical positions, as the dictionary joins a word's glued
  // pieces (my call 6), kitab.
  useFont(kNaskh);
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kKaf}, {kTeh, EpdFontFamily::BOLD, true}, {kAlef + kBeh, EpdFontFamily::REGULAR, true}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(3u, line.wordCount());
  EXPECT_TRUE(line.hasLogicalOrder());
  std::string stored;
  std::vector<std::string> byPosition(3);
  for (uint16_t i = 0; i < 3; ++i) {
    EXPECT_TRUE(line.hasGlyphForm(i)) << i;
    if (i > 0) {
      EXPECT_TRUE(line.wordContinues(i)) << i;
      EXPECT_EQ(line.wordXpos(i - 1) + advanceOf(line, i - 1), line.wordXpos(i)) << i;
    }
    stored += line.wordText(i);
    ASSERT_LT(line.logicalPosition(i), 3);
    byPosition[line.logicalPosition(i)] = line.wordText(i);
  }
  EXPECT_EQ(kAlef + kBeh + kTeh + kKaf, stored);
  EXPECT_EQ(kKitab, byPosition[0] + byPosition[1] + byPosition[2]);
}

TEST_F(ShapedLine, AnEmptyConsumedPieceIsLeftOffTheLine) {
  // Lam, then a bold alef, in the synthetic font: its rlig makes them one ligature, which the lam's token keeps, and
  // the alef's token keeps no glyph, an empty Glyphs piece. TextBlock would draw a word without a glyph form as its
  // text, the alef again over the ligature: the piece is left off the line, on a line the bidi pass lays out and on one
  // laid out without it. The token's bytes, its offset and its place among the tokens stay.
  useFont(kRtlTest);
  const std::vector<Word> words = {{kBeh}, {kLam}, {kAlef, EpdFontFamily::BOLD, true}};
  const auto expectLeftOff = [&](const TextBlock& line) {
    ASSERT_EQ(2u, line.wordCount());
    EXPECT_LT(wordWithText(line, kAlef), 0);
    const int lam = wordWithText(line, kLam);
    ASSERT_GE(lam, 0);
    ASSERT_TRUE(line.hasGlyphForm(static_cast<uint16_t>(lam)));
    EXPECT_EQ(1u, line.glyphForm(static_cast<uint16_t>(lam)).count);  // the ligature
    EXPECT_TRUE(line.hasGlyphForm(static_cast<uint16_t>(1 - lam)));
  };
  {
    ParsedText text(false, false, noIndentStyle());
    text.setShapingLatch(&latch);
    add(text, words);
    const Lines lines = layOut(text, renderer, 400);
    ASSERT_EQ(1u, lines.size());
    expectLeftOff(*lines[0]);
    EXPECT_EQ(3u, text.tokensShaped());
    EXPECT_FALSE(latch);
  }
  {
    // Without the line pass (the heap refuses its Scratch): drawn as its pieces in logical order, the alef's left off.
    BidiLine::releaseBuildMemory();
    ParsedText text(false, false, noIndentStyle());
    add(text, words);
    Lines lines;
    {
      const AllocationRefusal refused(sizeof(BidiLine::Scratch), sizeof(BidiLine::Scratch));
      lines = layOut(text, renderer, 400);
    }
    ASSERT_EQ(1u, lines.size());
    expectLeftOff(*lines[0]);
  }
  {
    // A column narrower than the ligature: the breaker's fallback puts the lam alone on a line, and the alef starts the
    // next. Accepted (the ruling): the alef's letter is in the lam's ligature, so its own line draws nothing at all.
    ParsedText text(false, false, noIndentStyle());
    add(text, {{kLam}, {kAlef, EpdFontFamily::BOLD, true}});
    const Lines lines = layOut(text, renderer, 1);
    ASSERT_EQ(1u, lines.size());
    ASSERT_EQ(1u, lines[0]->wordCount());
    EXPECT_EQ(kLam, lines[0]->wordText(0));
    EXPECT_EQ(1u, lines[0]->glyphForm(0).count);
  }
  {
    // An Indic token too, in a pathological font whose ccmp deletes KA: ma, then a bold ka shaped with ma as context,
    // which keeps no glyph. Left off without bidi and beside a Hebrew word.
    uint32_t ka = 0;
    ASSERT_NE(nullptr, face->current());
    ASSERT_TRUE(face->current()->nominalGlyph(0x0915, &ka));
    GsubFace deleting(readFont(TTF_FIXTURE_DIR, "NotoSansDevanagari-Shaping-Regular.ttf"),
                      deletingGsub(static_cast<uint16_t>(ka)));
    Seam::face = &deleting;
    for (const bool bidi : {false, true}) {
      ParsedText text(false, false, noIndentStyle());
      if (bidi) add(text, {{"שלום"}});
      add(text, {{kMa}, {kKa, EpdFontFamily::BOLD, true}});
      const Lines lines = layOut(text, renderer, 400);
      ASSERT_EQ(1u, lines.size()) << bidi;
      const TextBlock& line = *lines[0];
      EXPECT_EQ(bidi ? 2u : 1u, line.wordCount()) << bidi;
      EXPECT_LT(wordWithText(line, kKa), 0) << bidi;
      const int ma = wordWithText(line, kMa);
      ASSERT_GE(ma, 0) << bidi;
      EXPECT_TRUE(line.hasGlyphForm(static_cast<uint16_t>(ma))) << bidi;
    }
    Seam::face = nullptr;
  }
}

TEST_F(ShapedLine, ASegmentOfSeveralPiecesIsDrawnAsItsCodePoints) {
  // The mandatory guard: beh teh, then a Hebrew qamats, which no Arabic run takes, so the token is a Glyphs piece and a
  // Text piece. Across the cut the mark takes a slot of its own and is drawn right after its base, the teh: the run's
  // segment gives two line pieces, around the mark. Its glyphs drawn on each would be drawn twice: the segment is
  // drawn as the line pass's code points instead, each once.
  useFont(kNaskh);
  const std::string qamats = "\xD6\xB8";  // U+05B8
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{kBeh + kTeh + qamats}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(3u, line.wordCount());
  std::string drawn;
  for (uint16_t i = 0; i < line.wordCount(); ++i) {
    EXPECT_FALSE(line.hasGlyphForm(i)) << i;
    drawn += line.wordDrawText(i);
  }
  EXPECT_EQ(kTeh + qamats + kBeh, drawn);  // visual order, each code point once
  EXPECT_EQ(1u, text.tokensShaped());
  EXPECT_FALSE(latch);  // the line pass's own split, not a want of memory: nothing to rebuild for
}

TEST_F(ShapedLine, AShapedTokenTheLineCannotBeCutForIsItsCodePoints) {
  // No room for the line's cut points: the token is laid out uncut, one piece at its level, and drawn as its code
  // points from the line pass's form, not as its run (which would stand for the whole token, its brackets lost), and
  // the build is latched. The warm-up leaves every other container the room the next line needs; it held one cut.
  useFont(kNaskh);
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{kKitab + kArabicComma}, {kKitab}, {kKitab}});
  ASSERT_EQ(1u, layOut(text, renderer, 400).size());
  ASSERT_FALSE(latch);
  text.reset(noIndentStyle());
  const std::string token = "(" + kKitab + ")" + kArabicComma;  // three pieces: two cuts
  add(text, {{token}});
  Lines lines;
  {
    const LargestFreeBlock block(18);  // two cuts ask for 20 B
    lines = layOut(text, renderer, 400);
  }
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(1u, line.wordCount());
  EXPECT_FALSE(line.hasGlyphForm(0));
  EXPECT_EQ(token, line.wordText(0));
  EXPECT_TRUE(latch);
}

TEST_F(ShapedLayout, AHebrewParagraphWithArabicDigitsInATtfShapesNothing) {
  // A Hebrew paragraph needs bidi, and its Arabic-Indic digits hold bytes of the Arabic blocks; in a face that maps beh
  // the pass used to turn shaping on, take a shaping call per digit token and merge its glued groups. No Arabic run
  // can start there (the digits, the number signs and the Arabic comma are no run members), so it is laid out as
  // without a face: no ShapingState, nothing shaped, the span split of a word and its digits still two words.
  const std::string digits = "\xD9\xA1\xD9\xA9\xD9\xA4\xD9\xA8";  // U+0661 0669 0664 0668: 1948
  const std::vector<Word> hebrew = {{"שלום"}, {"ab"}, {digits, EpdFontFamily::REGULAR, true}, {kArabicComma}, {"עולם"}};
  Seam::face = nullptr;
  ParsedText plain(false, false, noIndentStyle());
  add(plain, hebrew);
  const std::vector<std::string> expected = bytesOfLines(layOut(plain, renderer, 400));
  ASSERT_EQ(1u, expected.size());

  useFont(kNaskh);
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, hebrew);
  const Lines lines = layOut(text, renderer, 400);
  EXPECT_EQ(expected, bytesOfLines(lines));
  EXPECT_FALSE(text.shapingStateHeld());
  EXPECT_EQ(0u, text.tokensShaped());
  EXPECT_FALSE(latch);
  ASSERT_EQ(1u, lines.size());
  EXPECT_GE(wordWithText(*lines[0], "ab"), 0);
  EXPECT_GE(wordWithText(*lines[0], digits), 0);

  // A letter of the blocks still turns it on.
  ParsedText arabic(false, false, noIndentStyle());
  add(arabic, {{"שלום"}, {kKitab}, {digits}});
  layOut(arabic, renderer, 400);
  EXPECT_TRUE(arabic.shapingStateHeld());
}

TEST(ArabicScripts, ARunMemberIsALetterAMarkOrArabicPunctuationNotADigit) {
  EXPECT_FALSE(arabic::holdsRunMember("\xD9\xA1\xD9\xA9", 4));  // U+0661 0669: digits
  EXPECT_FALSE(arabic::holdsRunMember("\xD8\x8C", 2));          // U+060C: the Arabic comma
  EXPECT_FALSE(arabic::holdsRunMember("abc", 3));
  EXPECT_TRUE(arabic::holdsRunMember("1\xD8\xA8", 3));  // U+0628 beh after a digit
  EXPECT_TRUE(arabic::holdsRunMember("\xD8\x9F", 2));   // U+061F: the Arabic question mark (AL)
  EXPECT_TRUE(arabic::holdsRunMember("\xD9\x8E", 2));   // U+064E fatha alone
  EXPECT_FALSE(arabic::holdsRunMember("\xD8", 1));      // a cut sequence
}

TEST_F(ShapedLayout, ALatinParagraphWithArabicDigitsInATtfIsUnchanged) {
  // Review fix 1: arabic::holdsArabic passes the Arabic-Indic digits and the Arabic comma, which no Arabic run takes
  // and which make no paragraph need bidi (AN, CS). In a TrueType face that holds them, a Latin paragraph with them is
  // laid out as master lays it out (as without a face): no ShapingState, nothing shaped, and no merge, so the span
  // split of a word and its digits stays two words. In a Hindi paragraph, which shapes, they are no shaped group
  // either.
  const std::string digits = "\xD9\xA1\xD9\xA9\xD9\xA4\xD9\xA8";  // U+0661 0669 0664 0668: 1948
  const std::vector<Word> latin = {{"year"}, {"ab"}, {digits, EpdFontFamily::REGULAR, true}, {kArabicComma}};
  Seam::face = nullptr;
  ParsedText plain(false, false, noIndentStyle());
  add(plain, latin);
  const std::vector<std::string> expected = bytesOfLines(layOut(plain, renderer, 400));
  ASSERT_EQ(1u, expected.size());

  useFont(kNaskh);
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, latin);
  const Lines lines = layOut(text, renderer, 400);
  EXPECT_EQ(expected, bytesOfLines(lines));
  EXPECT_FALSE(text.shapingStateHeld());
  EXPECT_EQ(0u, text.tokensShaped());
  EXPECT_FALSE(latch);
  ASSERT_EQ(1u, lines.size());
  EXPECT_GE(wordWithText(*lines[0], "ab"), 0);
  EXPECT_GE(wordWithText(*lines[0], digits), 0);

  Seam::face = face.get();  // the Devanagari face
  ParsedText hindi(false, false, noIndentStyle());
  add(hindi, {{kHindi}, {"ab"}, {digits, EpdFontFamily::REGULAR, true}});
  const Lines hindiLines = layOut(hindi, renderer, 400);
  ASSERT_EQ(1u, hindiLines.size());
  EXPECT_TRUE(hindiLines[0]->hasGlyphForm(0));
  EXPECT_GE(wordWithText(*hindiLines[0], "ab"), 0);
  EXPECT_GE(wordWithText(*hindiLines[0], digits), 0);
  EXPECT_EQ(1u, hindi.tokensShaped());  // the Hindi word alone
}

TEST_F(ShapedLayout, AnArabicParagraphInAFaceWithoutArabicIsUnshaped) {
  // Fable review fix 3: a TrueType face that does not map beh (arabic::kProbe) shapes no Arabic run: each would come
  // back as Text after a call block and a run scan, and the paragraph's same-style glued tokens and mark-only spans
  // would merge all the same. Such a paragraph is laid out as without a face: no ShapingState, nothing shaped or
  // merged, its bytes the unshaped path's. The Japanese TrueType fixture maps Latin and no Arabic.
  const std::vector<Word> words = {{kBeh},
                                   {kFatha, EpdFontFamily::BOLD, true},
                                   {kKitab},
                                   {kKitab, EpdFontFamily::REGULAR, true},
                                   {kKitab + kArabicComma}};
  Seam::face = nullptr;
  ParsedText plain(false, false, noIndentStyle());
  add(plain, words);
  const std::vector<std::string> expected = bytesOfLines(layOut(plain, renderer, 400));
  ASSERT_EQ(1u, expected.size());

  FixtureShapingFace& latin = useFont("NotoSansJP-TrueType.ttf");
  const ot::Face* const mapped = latin.current();
  ASSERT_NE(nullptr, mapped);
  uint32_t glyph = 0;
  ASSERT_TRUE(mapped->nominalGlyph('a', &glyph));
  ASSERT_FALSE(mapped->nominalGlyph(0x0628, &glyph));
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, words);
  EXPECT_EQ(expected, bytesOfLines(layOut(text, renderer, 400)));
  EXPECT_FALSE(text.shapingStateHeld());
  EXPECT_EQ(0u, text.tokensShaped());
  EXPECT_FALSE(latch);

  // Indic text is formed in that face all the same (reordered, the face mapping no Devanagari either).
  ParsedText hindi(false, false, noIndentStyle());
  add(hindi, {{kHindi}, {kKitab}, {kKitab, EpdFontFamily::REGULAR, true}});
  ASSERT_EQ(1u, layOut(hindi, renderer, 400).size());
  EXPECT_TRUE(hindi.shapingStateHeld());

  // The control: in a face that maps beh, the same paragraph shapes, and its glued tokens merge.
  useFont(kNaskh);
  ParsedText shaped(false, false, noIndentStyle());
  add(shaped, words);
  const Lines shapedLines = layOut(shaped, renderer, 400);
  ASSERT_EQ(1u, shapedLines.size());
  EXPECT_TRUE(shaped.shapingStateHeld());
  EXPECT_GE(wordWithText(*shapedLines[0], kBeh + kFatha), 0);
  EXPECT_GE(wordWithText(*shapedLines[0], kKitab + kKitab), 0);

  // A face that cannot be mapped now cannot say: the paragraph shapes as before, and its token degrades (latched).
  useFont(kNaskh).mapped = false;
  bool unmappedLatch = false;
  ParsedText unmapped(false, false, noIndentStyle());
  unmapped.setShapingLatch(&unmappedLatch);
  add(unmapped, {{kKitab}});
  ASSERT_EQ(1u, layOut(unmapped, renderer, 400).size());
  EXPECT_TRUE(unmapped.shapingStateHeld());
  EXPECT_TRUE(unmappedLatch);
}

TEST_F(ShapedLayout, ALatinWordForceSplitInAHindiParagraphKeepsTheStoreInStep) {
  // Review fix 3: a word wider than the column is split by the fallback breaks. In a paragraph that shapes, its
  // remainder gets a store entry though it is not shaped, so the shaped tokens after it find their own entries and keep
  // their glyph forms; one short, each would look up its neighbour's and be drawn as its code points.
  const std::string latin(30, 'x');  // 300 px
  constexpr int kColumn = 100;
  ASSERT_LT(shapedPx(kHindi), kColumn);
  ASSERT_LT(shapedPx(kPatrika), kColumn);
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{kHindi}, {latin}, {kHindi}, {kPatrika}});
  const Lines lines = layOut(text, renderer, kColumn);
  int split = 0;
  int shaped = 0;
  std::string joined;
  for (const auto& line : lines) {
    for (uint16_t i = 0; i < line->wordCount(); ++i) {
      const std::string word = line->wordText(i);
      if (word == kHindi || word == kPatrika) {
        ++shaped;
        EXPECT_TRUE(line->hasGlyphForm(i)) << word;
      } else if (word.find('x') != std::string::npos) {
        ++split;
        joined += word.back() == '-' ? word.substr(0, word.size() - 1) : word;
      }
    }
  }
  EXPECT_GE(split, 3);  // the Latin word in three parts at least
  EXPECT_EQ(latin, joined);
  EXPECT_EQ(3, shaped);
  EXPECT_FALSE(latch);
}

TEST_F(ShapedLine, AConsumedLetterIsDrawnWhenItsPartnerDegrades) {
  // Review fix 2: an empty run is left off only where the ligature that took its letter is drawn. The heap refuses the
  // line's cut points, so the lam's token (a bracket, then the lam, which keeps the lam-alef ligature) is laid out
  // uncut and drawn as its code points; the alef's token, an empty run, is then drawn as its code points too, not lost.
  // The warm-up (the same letters, without the bracket: no cut) leaves every other container the room the line needs.
  useFont(kRtlTest);
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{kBeh}, {kLam}, {kAlef, EpdFontFamily::BOLD, true}, {kBeh}});
  ASSERT_EQ(1u, layOut(text, renderer, 400).size());
  ASSERT_FALSE(latch);
  text.reset(noIndentStyle());
  add(text, {{"(" + kLam}, {kAlef, EpdFontFamily::BOLD, true}});
  Lines lines;
  {
    const LargestFreeBlock block(16);  // one cut asks for 18 B
    lines = layOut(text, renderer, 400);
  }
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  for (uint16_t i = 0; i < line.wordCount(); ++i) EXPECT_FALSE(line.hasGlyphForm(i)) << i;
  const int alef = wordWithText(line, kAlef);
  ASSERT_GE(alef, 0);
  EXPECT_EQ(kAlef, line.wordDrawText(static_cast<uint16_t>(alef)));
  EXPECT_TRUE(latch);

  // The control: with room for the cut, the lam draws the ligature and the alef is left off.
  text.reset(noIndentStyle());
  add(text, {{"(" + kLam}, {kAlef, EpdFontFamily::BOLD, true}});
  const Lines formed = layOut(text, renderer, 400);
  ASSERT_EQ(1u, formed.size());
  EXPECT_LT(wordWithText(*formed[0], kAlef), 0);
  const int lam = wordWithText(*formed[0], kLam);
  ASSERT_GE(lam, 0);
  EXPECT_TRUE(formed[0]->hasGlyphForm(static_cast<uint16_t>(lam)));
}

TEST_F(ShapedLine, AConsumedLetterDrawnWithoutTheLinePassTakesItsWidth) {
  // Fable review fix 2: on a line laid out without the line pass (the heap refuses its Scratch), a consumed letter
  // whose partner degraded is drawn as its code points at their width, so the piece after it does not overlap it. The
  // lam's style has no face (refused), so its token is its code points and draws no ligature; the bold alef glued to
  // it, shaped with the lam as context, keeps an empty run, then its comma (a Text piece).
  useFont(kRtlTest);
  Seam::refusedStyle = EpdFontFamily::REGULAR;
  BidiLine::releaseBuildMemory();
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kLam}, {kAlef + kArabicComma, EpdFontFamily::BOLD, true}});
  Lines lines;
  {
    const AllocationRefusal refused(sizeof(BidiLine::Scratch), sizeof(BidiLine::Scratch));
    lines = layOut(text, renderer, 400);
  }
  EXPECT_FALSE(BidiLine::heapScratchHeld());
  ASSERT_EQ(1u, lines.size());
  const TextBlock& line = *lines[0];
  ASSERT_EQ(3u, line.wordCount());  // the lam, the alef and the comma, in logical order
  EXPECT_FALSE(line.hasLogicalOrder());
  for (uint16_t i = 0; i < line.wordCount(); ++i) EXPECT_FALSE(line.hasGlyphForm(i)) << i;
  const int alef = wordWithText(line, kAlef);
  const int comma = wordWithText(line, kArabicComma);
  ASSERT_GE(alef, 0);
  ASSERT_GE(comma, 0);
  EXPECT_EQ(kAlef, line.wordDrawText(static_cast<uint16_t>(alef)));
  EXPECT_EQ(line.wordXpos(static_cast<uint16_t>(alef)) + kGlyph, line.wordXpos(static_cast<uint16_t>(comma)));
}
