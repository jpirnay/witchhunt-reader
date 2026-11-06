// Ours (writing modes step 2): ParsedText shapes its Indic tokens ("How shaping runs" steps 2-6 and 10). Words
// are shaped through a FixtureShapingFace over the TrueType Devanagari subset, installed through the stub renderer's
// seam; the expected records are the oracle's, ot::shape at the same size (ottest::shapeAsStored). The forms the tokens
// take on their lines, piece by piece (steps 7-9), are the ShapedLine suite's.
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
#include "Epub/ParsedText.h"
#include "Epub/blocks/TextBlock.h"
#include "FixtureShapingFace.h"
#include "GfxRenderer.h"
#include "IndicReorder.h"
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

// The face the seam hands out, at the ppem it reports; null: no font shapes. `refusals`: how many asks answer null
// first, as a face refused its memory does.
struct Seam {
  static inline ShapingFace* face = nullptr;
  static inline uint32_t ppem = kPpem;
  static inline int refusals = 0;
  static inline int asks = 0;
  static ShapingFace* source(int /*fontId*/, EpdFontFamily::Style /*style*/, uint32_t* ppem26_6) {
    ++asks;
    if (refusals > 0) {
      --refusals;
      return nullptr;
    }
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
  // memory) is the face of a font that cannot shape for the whole pass, so no entry is stored under a key that says
  // otherwise.
  Seam::refusals = 1;
  ParsedText text(false, false, noIndentStyle());
  add(text, {{kHindi}, {kPatrika}, {kKi}});
  EXPECT_EQ(1u, layOut(text, renderer, 400).size());
  EXPECT_EQ(1, Seam::asks);
  EXPECT_EQ(3u, text.tokensShaped());
  EXPECT_EQ(0, renderer.prefetchedRuns);  // reordered, as without a face: no glyph run

  // The next paragraph asks again, and shapes.
  text.reset(noIndentStyle());
  add(text, {{kHindi}});
  EXPECT_EQ(1u, layOut(text, renderer, 400).size());
  EXPECT_EQ(2, Seam::asks);
  EXPECT_EQ(1, renderer.prefetchedRuns);
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
  // A paragraph that needs bidi shapes nothing: the line pass's pieces are not cut at runs, so a shaped run could
  // straddle two of them. Its Indic words are laid out as code points.
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
  return TextBlock::deserialize(file);
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
