// A shaped word (writing modes step 2, decision 4) is drawn by glyph ID: its line keeps the word's compact glyph
// records as a glyph form, bit 2 of the TextBlock bidi region, beside the UTF-8 display forms of bit 0 and sharing
// their offsets. A glyph form is 0xFF, a u16 glyph count, the records, then 0x00. The word's text stays its logical
// text. Against the fixed-width shim of this directory: every codepoint 10 px, and runs measured as glyphRunAdvance.
#include <BufferedFileIO.h>
#include <GfxRenderer.h>
#include <GlyphRun.h>
#include <Memory.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Epub/TextEmphasis.h"
#include "Epub/blocks/TextBlock.h"

namespace {

constexpr int kFontId = 1;
// What TextBlock::serialize writes after the arena: the alignment byte, the size multiplier, the heading font.
constexpr size_t kStyleTrailerBytes = sizeof(CssTextAlign) + sizeof(float) + sizeof(int32_t);

constexpr std::string_view kHindi = "हिन्दी";
constexpr std::string_view kKi = "कि";
// कि's vowel sign before its consonant, as a font that cannot shape draws it.
constexpr std::string_view kKiDrawn = "िक";

// A run's records, encoded as a section keeps them.
struct EncodedRun {
  std::vector<uint8_t> bytes;
  uint16_t count = 0;
  GlyphRun view() const { return GlyphRun{bytes.data(), count, static_cast<uint16_t>(bytes.size())}; }
};

EncodedRun encodeRun(const std::vector<GlyphRecord>& records) {
  EncodedRun run;
  for (const GlyphRecord& record : records) {
    uint8_t out[6];
    size_t written = 0;
    if (!glyph_run::encode(record, out, sizeof(out), &written)) return {};
    run.bytes.insert(run.bytes.end(), out, out + written);
    run.count++;
  }
  return run;
}

// हिन्दी as a shaper might give it: five glyphs, the third moved by an offset. In whole pixels the advances are 10, 5,
// 8, 0 and 13 (36 px) drawn one by one, and 35 px accumulated: neither is the 60 px of its six code points.
EncodedRun hindiRun() {
  return encodeRun(
      {{0x0123, 160, 0, 0}, {0x0045, 72, 0, 0}, {0x0067, 120, -3, 2}, {0x0089, 0, 0, 0}, {0x00AB, 200, 0, 0}});
}

// The count, then "glyph:advance[:dx,dy]" for each record; a cut record or bytes left over are said so.
std::string describe(const GlyphRun& run) {
  std::string out = "n=" + std::to_string(run.count);
  size_t pos = 0;
  for (uint16_t i = 0; i < run.count; i++) {
    GlyphRecord record;
    size_t read = 0;
    if (!glyph_run::decode(run.records + pos, run.bytes - pos, &record, &read)) return out + " <cut>";
    pos += read;
    out += " " + std::to_string(record.glyph) + ":" + std::to_string(record.advance);
    if (record.dx != 0 || record.dy != 0) out += ":" + std::to_string(record.dx) + "," + std::to_string(record.dy);
  }
  if (pos != run.bytes) out += " <+" + std::to_string(run.bytes - pos) + " bytes>";
  return out;
}

struct LineSpec {
  std::vector<std::string_view> words = {};
  std::vector<GlyphRun> glyphs = {};              // empty: none
  std::vector<std::string_view> forms = {};       // empty: none
  std::vector<uint8_t> order = {};                // empty: stored order
  std::vector<uint8_t> sizes = {};                // empty: every word at 100%
  std::vector<EpdFontFamily::Style> styles = {};  // empty: every word REGULAR
  std::vector<TextBlock::Annotation> annotations = {};
};

// The line `spec` describes, its words 50 px apart, handed over as views as the line pass hands them.
std::unique_ptr<TextBlock> line(const LineSpec& spec) {
  const size_t n = spec.words.size();
  const std::vector<EpdFontFamily::Style> styles =
      spec.styles.empty() ? std::vector<EpdFontFamily::Style>(n, EpdFontFamily::REGULAR) : spec.styles;
  std::vector<TextBlock::WordSize> sizes;
  for (const uint8_t pct : spec.sizes) sizes.push_back({pct, 0});
  std::vector<int16_t> xpos;
  for (size_t i = 0; i < n; i++) xpos.push_back(static_cast<int16_t>(i * 50));
  TextBlock::WordRange range;
  range.wordViews = spec.words.data();
  range.styles = &styles;
  range.sizes = sizes.empty() ? nullptr : &sizes;
  range.glyphForms = spec.glyphs.empty() ? nullptr : spec.glyphs.data();
  range.displayForms = spec.forms.empty() ? nullptr : spec.forms.data();
  range.logicalOrder = spec.order.empty() ? nullptr : spec.order.data();
  range.annotations = spec.annotations.empty() ? nullptr : &spec.annotations;
  range.first = 0;
  range.count = n;
  return makeUniqueNoThrow<TextBlock>(range, xpos, BlockStyle());
}

std::string bytesOf(const TextBlock& block) {
  FsFile file = HalFile::forReadWrite();
  {
    serialization::BufferedFileWriter writer(file);
    if (!block.serialize(writer) || !writer.flush()) return {};
  }
  if (!file.seek(0)) return {};
  std::string bytes(file.size(), '\0');
  file.read(bytes.data(), bytes.size());
  return bytes;
}

std::unique_ptr<TextBlock> readBack(const std::string& bytes) {
  FsFile file = HalFile::forReadWrite();
  file.write(bytes.data(), bytes.size());
  if (!file.seek(0)) return nullptr;
  serialization::BufferedFileReader reader(file);
  return TextBlock::deserialize(reader);
}

std::string octets(const std::initializer_list<int> values) {
  std::string out;
  for (const int v : values) out.push_back(static_cast<char>(v));
  return out;
}

// A serialised line without annotations or a region, given `region` as its bidi region by hand: the word count's bit 15
// set and the region's size inserted after the text size (the header is u16 words, u8 flags, u16 text size).
std::string withRegion(std::string bytes, const std::string& region) {
  bytes.insert(bytes.size() - kStyleTrailerBytes, region);
  const auto size = static_cast<uint16_t>(region.size());
  bytes.insert(5, reinterpret_cast<const char*>(&size), sizeof(size));
  bytes[1] = static_cast<char>(static_cast<uint8_t>(bytes[1]) | 0x80);
  return bytes;
}

// A region for a two-word line: `parts`, the two words' offsets, then `texts`.
std::string twoWordRegion(const uint8_t parts, const uint16_t offset0, const uint16_t offset1,
                          const std::string& texts) {
  std::string region(1, static_cast<char>(parts));
  region.append(reinterpret_cast<const char*>(&offset0), sizeof(offset0));
  region.append(reinterpret_cast<const char*>(&offset1), sizeof(offset1));
  return region + texts;
}

constexpr uint16_t kNone = 0xFFFF;
// One glyph form of one record (glyph 0x0102, advance 10 px, no offset): 0xFF, the count, the record, 0x00.
const std::string kOneRecordForm = octets({0xFF, 0x01, 0x00, 0x02, 0x01, 0xA0, 0x00, 0x00});

}  // namespace

TEST(TextBlockGlyphForms, AGlyphFormRoundTripsThroughTheFile) {
  const EncodedRun hindi = hindiRun();
  ASSERT_EQ(5, hindi.count);
  const auto block = line({.words = {kHindi, ",", kKi}, .glyphs = {hindi.view(), {}, {}}, .forms = {"", "", kKiDrawn}});
  ASSERT_TRUE(block->valid());
  const std::string bytes = bytesOf(*block);
  ASSERT_FALSE(bytes.empty());

  const auto restored = readBack(bytes);
  ASSERT_NE(nullptr, restored);
  ASSERT_EQ(3, restored->wordCount());
  EXPECT_TRUE(restored->hasGlyphForm(0));
  EXPECT_EQ(describe(hindi.view()), describe(restored->glyphForm(0)));
  EXPECT_EQ(hindi.bytes.size(), restored->glyphForm(0).bytes);
  EXPECT_FALSE(restored->hasDisplayForm(0));
  EXPECT_FALSE(restored->hasGlyphForm(1));
  EXPECT_EQ(0, restored->glyphForm(1).count);
  EXPECT_FALSE(restored->hasGlyphForm(2));
  EXPECT_STREQ(std::string(kKiDrawn).c_str(), restored->wordDrawText(2));
  EXPECT_STREQ(std::string(kHindi).c_str(), restored->wordText(0));
  EXPECT_EQ(bytes, bytesOf(*restored));
}

// Review focus 2: the dictionary, footnote links, the QR text and KOSync read a glyph word's text, never its glyphs.
TEST(TextBlockGlyphForms, AGlyphWordKeepsItsLogicalText) {
  const EncodedRun hindi = hindiRun();
  const auto block = line({.words = {kHindi}, .glyphs = {hindi.view()}});
  ASSERT_TRUE(block->valid());
  const auto restored = readBack(bytesOf(*block));
  ASSERT_NE(nullptr, restored);
  for (const TextBlock* b : {block.get(), restored.get()}) {
    EXPECT_STREQ(std::string(kHindi).c_str(), b->wordText(0));
    EXPECT_EQ(kHindi.size(), b->wordTextLen(0));
    EXPECT_STREQ(std::string(kHindi).c_str(), b->wordDrawText(0));
    EXPECT_FALSE(b->hasDisplayForm(0));
  }
}

// Every line without a shaped word keeps its bytes: no glyph forms, or runs of no glyphs (whatever their pointers say),
// write what the line wrote before.
TEST(TextBlockGlyphForms, ALineWithoutAGlyphFormKeepsItsBytes) {
  const EncodedRun hindi = hindiRun();
  const GlyphRun empty{hindi.bytes.data(), 0, static_cast<uint16_t>(hindi.bytes.size())};

  const std::string latin = bytesOf(*line({.words = {"abc", "def"}}));
  ASSERT_FALSE(latin.empty());
  EXPECT_EQ(latin, bytesOf(*line({.words = {"abc", "def"}, .glyphs = {empty, {}}})));
  EXPECT_EQ(0, static_cast<uint8_t>(latin[1]) & 0x80);

  const LineSpec hebrew{.words = {"abc", "שלום"}, .forms = {"abc", "םולש"}, .order = {1, 0}};
  const std::string hebrewBytes = bytesOf(*line(hebrew));
  ASSERT_FALSE(hebrewBytes.empty());
  LineSpec hebrewWithRuns = hebrew;
  hebrewWithRuns.glyphs = {{}, empty};
  EXPECT_EQ(hebrewBytes, bytesOf(*line(hebrewWithRuns)));
}

// Regions built by hand on a two-word line: each case breaks one rule, and the line's read fails.
TEST(TextBlockGlyphForms, LoadRejects) {
  const std::string plain = bytesOf(*line({.words = {"ab", "cd"}}));
  ASSERT_FALSE(plain.empty());
  const auto valid = readBack(withRegion(plain, twoWordRegion(0x04, 0, kNone, kOneRecordForm)));
  ASSERT_NE(nullptr, valid);  // the region every case below breaks one rule of
  EXPECT_EQ("n=1 258:160", describe(valid->glyphForm(0)));
  ASSERT_NE(nullptr, readBack(withRegion(plain, twoWordRegion(0x01, kNone, 0, octets({'x', 0x00})))));

  // Bit 2 with no glyph form: the UTF-8 form is word 1's alone.
  EXPECT_EQ(nullptr, readBack(withRegion(plain, twoWordRegion(0x05, kNone, 0, octets({'x', 0x00})))));
  // A 0xFF form without bit 2.
  EXPECT_EQ(nullptr, readBack(withRegion(plain, twoWordRegion(0x01, 0, kNone, kOneRecordForm))));
  // A record cut short: its advance field says an offset follows, and only the form's 0x00 does.
  EXPECT_EQ(nullptr,
            readBack(withRegion(
                plain, twoWordRegion(0x04, 0, kNone, octets({0xFF, 0x01, 0x00, 0x02, 0x01, 0xA0, 0x80, 0x00})))));
  // A missing 0x00: the record is followed by another byte (the texts still end with a NUL).
  EXPECT_EQ(nullptr,
            readBack(withRegion(
                plain, twoWordRegion(0x04, 0, kNone, octets({0xFF, 0x01, 0x00, 0x02, 0x01, 0xA0, 0x00, 'x', 0x00})))));
  // A count past the bytes: two glyphs promised, one there.
  EXPECT_EQ(nullptr,
            readBack(withRegion(
                plain, twoWordRegion(0x04, 0, kNone, octets({0xFF, 0x02, 0x00, 0x02, 0x01, 0xA0, 0x00, 0x00})))));
  // A glyph form of no glyphs, which no build writes.
  EXPECT_EQ(nullptr, readBack(withRegion(plain, twoWordRegion(0x04, 0, kNone, octets({0xFF, 0x00, 0x00, 0x00})))));
  // A glyph form on a vertical line (flags bit 2), which no build writes (a vertical line's Indic words take
  // IndicReorder's form); the line alone reads.
  std::string vertical = plain;
  vertical[2] = static_cast<char>(static_cast<uint8_t>(vertical[2]) | 0x04);
  ASSERT_NE(nullptr, readBack(vertical));
  EXPECT_EQ(nullptr, readBack(withRegion(vertical, twoWordRegion(0x04, 0, kNone, kOneRecordForm))));
  // Parts bit 3, still reserved.
  EXPECT_EQ(nullptr, readBack(withRegion(plain, twoWordRegion(0x0C, 0, kNone, kOneRecordForm))));
}

// The run is drawn where and at the scale drawText would have drawn the word's text, and the text is not drawn.
TEST(TextBlockGlyphForms, RenderDrawsTheRunAtTheWordsPosition) {
  const EncodedRun hindi = hindiRun();
  for (const uint8_t pct : {100, 50}) {
    LineSpec spec{.words = {"abc", kHindi}};
    if (pct != 100) spec.sizes = {100, pct};
    GfxRenderer asText;
    line(spec)->render(asText, kFontId, 7, 11);
    ASSERT_EQ(2u, asText.drawCalls.size());

    spec.glyphs = {{}, hindi.view()};
    GfxRenderer r;
    line(spec)->render(r, kFontId, 7, 11);
    ASSERT_EQ(1u, r.glyphRunDraws.size()) << "pct " << static_cast<int>(pct);
    EXPECT_EQ(kFontId, r.glyphRunDraws[0].fontId);
    EXPECT_EQ(50 + 7, r.glyphRunDraws[0].x);
    EXPECT_EQ(asText.drawCalls[1].y, r.glyphRunDraws[0].y) << "pct " << static_cast<int>(pct);
    EXPECT_EQ(5, r.glyphRunDraws[0].count);
    EXPECT_FLOAT_EQ(pct / 100.0f, r.glyphRunDraws[0].scale);
    ASSERT_EQ(1u, r.drawCalls.size());
    EXPECT_EQ("abc", r.drawCalls[0].text);
  }
}

TEST(TextBlockGlyphForms, TheBoxAndTheUnderlineAreTheRunsWidth) {
  const EncodedRun hindi = hindiRun();
  const GlyphRun run = hindi.view();
  ASSERT_EQ(36, glyphRunAdvance(run, false));
  ASSERT_EQ(35, glyphRunAdvance(run, true));
  GfxRenderer r;

  const auto block = line({.words = {"abc", kHindi}, .glyphs = {{}, run}});
  EXPECT_EQ(glyphRunAdvance(run, false), block->wordBox(r, 1, kFontId, 0, 0).width);
  const auto half = line({.words = {"abc", kHindi}, .glyphs = {{}, run}, .sizes = {100, 50}});
  EXPECT_EQ(glyphRunAdvance(run, true), half->wordBox(r, 1, kFontId, 0, 0).width);

  const auto underlined = line(
      {.words = {"abc", kHindi}, .glyphs = {{}, run}, .styles = {EpdFontFamily::REGULAR, EpdFontFamily::UNDERLINE}});
  underlined->render(r, kFontId, 7, 0);
  ASSERT_EQ(1u, r.drawLineCalls.size());
  EXPECT_EQ(50 + 7, r.drawLineCalls[0].x1);
  EXPECT_EQ(glyphRunAdvance(run, false), r.drawLineCalls[0].x2 - r.drawLineCalls[0].x1);
}

// An emphasis mark centres on each character's advance, and a glyph word's characters have none of their own: its
// marks are left out, the Latin word's are drawn.
TEST(TextBlockGlyphForms, TheEmphasisMarksSkipAGlyphWord) {
  const EncodedRun hindi = hindiRun();
  TextBlock::Annotation emphasis;
  emphasis.firstWord = 0;
  emphasis.wordCount = 2;
  emphasis.kind = TextBlock::AnnotationKind::Emphasis;
  emphasis.mark = TextEmphasis::Dot;
  const auto block = line({.words = {"abc", kHindi}, .glyphs = {{}, hindi.view()}, .annotations = {emphasis}});
  ASSERT_TRUE(block->valid());
  GfxRenderer r;
  block->render(r, kFontId, 0, 0);
  size_t marks = 0;
  for (const auto& call : r.drawCalls) marks += call.scale == 0.5f ? 1 : 0;  // the marks, at half the word's size
  EXPECT_EQ(3u, marks);
  EXPECT_EQ(1u, r.glyphRunDraws.size());
}
