// Ours (writing modes step 2, PR 3): ParsedText shapes its Indic tokens ("How shaping runs" steps 2-6 and 10). Words
// are shaped through a FixtureShapingFace over the TrueType Devanagari subset, installed through the stub renderer's
// seam; the expected records are the oracle's, ot::shape at the same size (ottest::shapeAsStored). The forms the tokens
// take on their lines are the ShapedLine suite's.
#include <BuildArena.h>
#include <EpdFontData.h>
#include <OtShaper.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/TextBlock.h"
#include "FixtureShapingFace.h"
#include "GfxRenderer.h"
#include "IndicScripts.h"
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

// The face the seam hands out, at the ppem it reports; null: no font shapes.
struct Seam {
  static inline ShapingFace* face = nullptr;
  static inline uint32_t ppem = kPpem;
  static ShapingFace* source(int /*fontId*/, EpdFontFamily::Style /*style*/, uint32_t* ppem26_6) {
    if (face != nullptr) *ppem26_6 = ppem;
    return face;
  }
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
  EXPECT_TRUE(line.serialize(file));
  EXPECT_TRUE(file.seek(0));
  std::string bytes(file.size(), '\0');
  EXPECT_EQ(static_cast<int>(bytes.size()), file.read(bytes.data(), bytes.size()));
  return bytes;
}

std::vector<uint32_t> codepoints(const std::string& text) { return ottest::utf32(text.c_str()); }

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

  std::unique_ptr<FixtureShapingFace> face;
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
  // Review Focus 3: a compound wider than its column. Each part is measured as it is shaped, its hyphen included.
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
    ASSERT_EQ(1u, lines[k]->wordCount()) << k;
    std::string part = lines[k]->wordText(0);
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

TEST_F(ShapedLayout, ABidiParagraphShapesNothingYet) {
  // PR 4 owns the line pass's run-cut pieces: until then a paragraph that needs bidi lays its Indic words out as code
  // points.
  ParsedText text(false, false, noIndentStyle());
  text.setShapingLatch(&latch);
  add(text, {{"שלום"}, {kHindi}});
  const Lines lines = layOut(text, renderer, 400);
  ASSERT_EQ(1u, lines.size());
  EXPECT_FALSE(text.shapingStateHeld());
  EXPECT_EQ(0u, text.tokensShaped());
  EXPECT_FALSE(latch);
  bool found = false;
  for (uint16_t i = 0; i < lines[0]->wordCount(); ++i) {
    if (lines[0]->wordText(i) != kHindi) continue;
    found = true;
    EXPECT_FALSE(lines[0]->hasGlyphForm(i));
    EXPECT_EQ(kHindi, lines[0]->wordDrawText(i));
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
