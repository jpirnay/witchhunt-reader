// Ours (writing modes step 5): UI text with its Indic and Myanmar runs shaped over the face behind the UI face
// (lib/GfxRenderer/UiShapedText). The UI face is the built-in Inter 12 behind the shared FallbackFaceProvider, as
// CjkUiFonts attaches it; the reader's face is a test double over the shaping fixtures
// (test/fixtures/fonts), which draws glyph keys as TtfFont's faces do.
#include <BidiUtils.h>
#include <GlyphKey.h>
#include <GlyphRun.h>
#include <IndicReorder.h>
#include <IndicShaper.h>
#include <OtFace.h>
#include <TextShaping.h>
#include <TextTruncation.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "EpdFont.h"
#include "EpdFontFamily.h"
#include "FallbackFaceProvider.h"
#include "FixtureShapingFace.h"
#include "UiShapedText.h"
#include "builtinFonts/inter_ui_12_regular.h"

namespace {

constexpr auto REGULAR = EpdFontFamily::REGULAR;
constexpr const char* kDevanagariFont = "NotoSansDevanagari-Shaping-Regular.ttf";
constexpr const char* kMyanmarFont = "NotoSansMyanmar-Shaping-Regular.ttf";

// Words of the Devanagari subset (test/ttf_font/ExpectedGlyphRuns.h), named in the comments.
constexpr const char* kHindi = "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80";  // हिन्दी
constexpr const char* kPatrika = "\xE0\xA4\xAA\xE0\xA4\xA4\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA4\xBF\xE0\xA4\x95\xE0\xA4\xBE";
constexpr const char* kShri = "\xE0\xA4\xB6\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA5\x80";                 // श्री
constexpr const char* kKrishna = "\xE0\xA4\x95\xE0\xA5\x83\xE0\xA4\xB7\xE0\xA5\x8D\xE0\xA4\xA3";  // कृष्ण
constexpr const char* kGyan = "\xE0\xA4\x9C\xE0\xA5\x8D\xE0\xA4\x9E\xE0\xA4\xBE\xE0\xA4\xA8";     // ज्ञान
constexpr const char* kPrarthana =
    "\xE0\xA4\xAA\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA4\xBE\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\xA5\xE0\xA4\xA8\xE0\xA4\xBE";
constexpr const char* kSanskritam =
    "\xE0\xA4\xB8\xE0\xA4\x82\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\x95\xE0\xA5\x83\xE0\xA4\xA4\xE0\xA4\xAE\xE0\xA5\x8D";
constexpr const char* kKi = "\xE0\xA4\x95\xE0\xA4\xBF";         // कि
constexpr const char* kBengaliKi = "\xE0\xA6\x95\xE0\xA6\xBF";  // কি, which the Devanagari subset does not map
// Myanmar (test/ot_shaper/MyanmarExpected.h).
constexpr const char* kMyanmar = "\xE1\x80\x99\xE1\x80\xBC\xE1\x80\x94\xE1\x80\xBA\xE1\x80\x99\xE1\x80\xAC";  // မြန်မာ
constexpr const char* kKyaung =
    "\xE1\x80\x80\xE1\x80\xBC\xE1\x80\xB1\xE1\x80\xAC\xE1\x80\x84\xE1\x80\xB7\xE1\x80\xBA";  // ကြောင့်
constexpr const char* kHebrew = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";                          // shalom
constexpr const char* kArabic = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";                          // salam
constexpr const char* kJapanese = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";

std::string join(const std::vector<std::string>& words) {
  std::string out;
  for (const std::string& word : words) out += (out.empty() ? "" : " ") + word;
  return out;
}

// The reader's face as TtfFont::nativeFonts() hands it out behind the UI faces: 2-bit, no tables, every glyph from its
// provider, which draws glyph keys and makes its shaping face on first ask, as TtfFont does (released by
// releaseShapingFace(), TtfFontSet::releaseShapingFaces). A code point is drawn when the font's cmap maps it; a glyph's
// metrics follow from its ID, and its bitmap is ink at every pixel.
struct ReaderFace final : GlyphProvider {
  explicit ReaderFace(const char* fontFile, const uint32_t px = 26)
      : bytes(readFont(TTF_FIXTURE_DIR, fontFile)), cmap(bytes), ppem(px << 6) {
    data.is2Bit = true;
    data.provider = this;
    bitmap.assign(512, 0xFF);
    cmap.current();  // built now, so no test counts its tables against UI text
  }

  bool maps(const uint32_t cp, uint32_t* glyph) {
    const ot::Face* const tables = cmap.current();
    return tables != nullptr && tables->nominalGlyph(cp, glyph);
  }

  const EpdGlyph* missGlyph(const uint32_t cp) override {
    uint32_t glyph = 0;
    if (glyph_key::is(cp)) {
      glyph = glyph_key::glyphOf(cp);
      ++keysAsked;
    } else if (!maps(cp, &glyph)) {
      return nullptr;
    }
    EpdGlyph& record = ring[next++ % 8];
    const auto width = static_cast<uint8_t>(4 + glyph % 9);
    const auto height = static_cast<uint8_t>(8 + glyph % 7);
    record = EpdGlyph{width,
                      height,
                      static_cast<uint16_t>((width + 2) << 4),
                      1,
                      static_cast<int16_t>(height - 2),
                      glyphDataBytes(width, height, true),
                      0};
    return &record;
  }
  bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& out) const override {
    if (glyph < ring || glyph >= ring + 8) return false;
    out = bitmap.data();
    return true;
  }
  bool drawsGlyphKeys() const override { return keys; }
  ShapingFace* shapingFace(uint32_t* ppem26_6, bool* /*refused*/) override {
    *ppem26_6 = ppem;
    if (!face) {
      face = std::make_unique<FixtureShapingFace>(bytes);
      ++facesMade;
    }
    return face.get();
  }
  bool holdsShapingFace() const override { return face != nullptr; }
  void prefetchGlyphs(const uint16_t* glyphs, const size_t count) override {
    prefetched.insert(prefetched.end(), glyphs, glyphs + count);
  }
  void releaseShapingFace() { face.reset(); }

  static const EpdFontData* source(void* ctx) { return &static_cast<ReaderFace*>(ctx)->data; }

  std::vector<uint8_t> bytes;
  FixtureShapingFace cmap;  // the code points' glyphs, apart from the shaping face UI text makes and frees
  std::unique_ptr<FixtureShapingFace> face;
  uint32_t ppem;
  bool keys = true;
  int facesMade = 0;
  int keysAsked = 0;
  std::vector<uint16_t> prefetched;
  EpdFontData data{};
  EpdGlyph ring[8]{};
  uint8_t next = 0;
  std::vector<uint8_t> bitmap;
};

// The records of each Glyphs piece shapeToken writes for `text` over `face`, as a section build would shape it.
class RunsSink final : public IndicShaper::PieceSink {
 public:
  std::vector<std::vector<GlyphRecord>> runs;
  bool beginPiece(const IndicShaper::PieceKind kind, uint16_t, uint16_t) override {
    if (kind == IndicShaper::PieceKind::Glyphs) runs.emplace_back();
    glyphs_ = kind == IndicShaper::PieceKind::Glyphs;
    return true;
  }
  bool addGlyph(const GlyphRecord& record) override {
    if (glyphs_) runs.back().push_back(record);
    return true;
  }
  bool addBytes(const char*, size_t) override { return true; }

 private:
  bool glyphs_ = false;
};

std::vector<std::vector<GlyphRecord>> shapedRuns(ShapingFace& face, const uint32_t ppem26_6, const std::string& text) {
  RunsSink sink;
  {
    IndicShaper::CallMemory memory(nullptr, text.size());
    IndicShaper::Request request;
    request.face = &face;
    request.ppem26_6 = ppem26_6;
    EXPECT_EQ(IndicShaper::Outcome::Formed, IndicShaper::shapeToken(text, request, memory, sink));
  }
  TextShaping::releaseBuildMemory();
  return sink.runs;
}

std::vector<GlyphRecord> recordsOf(const GlyphRun& run) {
  std::vector<GlyphRecord> out;
  forEachGlyph(run, 0, false, [&](const GlyphRecord& record, int) { out.push_back(record); });
  return out;
}

bool sameRecords(const std::vector<GlyphRecord>& a, const std::vector<GlyphRecord>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].glyph != b[i].glyph || a[i].advance != b[i].advance || a[i].dx != b[i].dx || a[i].dy != b[i].dy) {
      return false;
    }
  }
  return true;
}

class UiShapedTextTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ui = inter_ui_12_regular;
    ui.provider = &shared;
    shared.setPrimaryFace(&inter_ui_12_regular);
  }
  void TearDown() override {
    ui_shaping::endPass();
    TextShaping::releaseBuildMemory();
  }

  void readFrom(ReaderFace& reader) { shared.setReaderFace(&ReaderFace::source, &reader); }

  // drawText's order: the string begins, then it is shaped.
  const ui_shaping::Shaped* shape(const std::string& text) {
    family.beginText(text.c_str(), REGULAR);
    return ui_shaping::shape(family, REGULAR, text.c_str());
  }
  int width(const std::string& text) { return ui_shaping::width(family, REGULAR, text.c_str()); }
  std::string truncated(const std::string& text, const int maxWidth) {
    family.beginText(text.c_str(), REGULAR);
    std::string out;
    EXPECT_TRUE(ui_shaping::truncate(family, REGULAR, text.c_str(), maxWidth, out)) << text;
    return out;
  }
  // The reordered ink box today's UI measures a string by.
  int unshapedWidth(const std::string& text) {
    std::string holder;
    family.beginText(text.c_str(), REGULAR);
    int w = 0;
    int h = 0;
    family.getTextDimensions(ui_shaping::reordered(text.c_str(), holder), &w, &h, REGULAR);
    return w;
  }

  FallbackFaceProvider shared;
  EpdFontData ui{};
  EpdFont font{&ui};
  EpdFontFamily family{&font};
};

// --- What is shaped ---

TEST_F(UiShapedTextTest, ADevanagariTitleHasTheGlyphsOfTheSectionBuildsShaping) {
  ReaderFace reader(kDevanagariFont);
  readFrom(reader);
  const std::string title = join({kShri, kKrishna, kPrarthana, kSanskritam});
  const ui_shaping::Shaped* const shaped = shape(title);
  ASSERT_NE(shaped, nullptr);
  const auto expected = shapedRuns(*reader.face, reader.ppem, title);
  std::vector<std::vector<GlyphRecord>> runs;
  std::string text;
  for (const ui_shaping::Segment& segment : *shaped) {
    if (segment.glyphs) {
      runs.push_back(recordsOf(segment.run));
    } else {
      text += segment.text;
    }
  }
  ASSERT_EQ(expected.size(), 4u);
  ASSERT_EQ(runs.size(), expected.size());
  for (size_t i = 0; i < runs.size(); ++i) EXPECT_TRUE(sameRecords(runs[i], expected[i])) << "run " << i;
  EXPECT_EQ(text, "   ");  // the spaces between the words, through the codepoint path
  EXPECT_EQ(shaped->source(), &reader.data);
}

TEST_F(UiShapedTextTest, AMyanmarTitleHasTheGlyphsOfTheSectionBuildsShaping) {
  ReaderFace reader(kMyanmarFont);
  readFrom(reader);
  const std::string title = join({kMyanmar, kKyaung});
  const ui_shaping::Shaped* const shaped = shape(title);
  ASSERT_NE(shaped, nullptr);
  const auto expected = shapedRuns(*reader.face, reader.ppem, title);
  ASSERT_EQ(expected.size(), 2u);
  size_t run = 0;
  for (const ui_shaping::Segment& segment : *shaped) {
    if (!segment.glyphs) continue;
    ASSERT_LT(run, expected.size());
    EXPECT_TRUE(sameRecords(recordsOf(segment.run), expected[run])) << "run " << run;
    ++run;
  }
  EXPECT_EQ(run, expected.size());
}

TEST_F(UiShapedTextTest, LatinBetweenTheRunsIsTheUiFacesText) {
  ReaderFace reader(kDevanagariFont);
  readFrom(reader);
  const std::string title = std::string(kHindi) + ": The Hindi Magazine";
  const ui_shaping::Shaped* const shaped = shape(title);
  ASSERT_NE(shaped, nullptr);
  ASSERT_EQ(shaped->size(), 2u);
  EXPECT_TRUE((*shaped)[0].glyphs);
  EXPECT_STREQ((*shaped)[1].text, ": The Hindi Magazine");
  EXPECT_EQ((*shaped)[1].advance, ui_shaping::codepointAdvance(family, ": The Hindi Magazine", REGULAR));
  // The segments cover the string in order.
  EXPECT_EQ((*shaped)[0].begin, 0);
  EXPECT_EQ((*shaped)[0].end, (*shaped)[1].begin);
  EXPECT_EQ((*shaped)[1].end, title.size());
}

TEST_F(UiShapedTextTest, AScriptTheFaceDoesNotMapIsReorderedBesideTheShapedOne) {
  ReaderFace reader(kDevanagariFont);
  readFrom(reader);
  const std::string title = std::string(kKi) + " " + kBengaliKi;
  const ui_shaping::Shaped* const shaped = shape(title);
  ASSERT_NE(shaped, nullptr);
  ASSERT_EQ(shaped->size(), 3u);
  EXPECT_TRUE((*shaped)[0].glyphs);
  std::string form;
  ASSERT_TRUE(indicReorderForDisplay(kBengaliKi, form));
  EXPECT_TRUE((*shaped)[2].reordered);
  EXPECT_EQ(form, (*shaped)[2].text);
}

// --- What keeps today's form ---

TEST_F(UiShapedTextTest, WithoutAFaceThatDrawsKeysTheTextIsReorderedByteForByte) {
  // No reader font at all, then one whose provider draws no glyph keys (a .cpfont book font).
  for (const bool readerFont : {false, true}) {
    ReaderFace reader(kDevanagariFont);
    reader.keys = false;
    if (readerFont) readFrom(reader);
    for (const std::string& title : {join({kHindi, kPatrika}), std::string(kKi) + kKi, join({kShri, kGyan})}) {
      SCOPED_TRACE(title);
      EXPECT_EQ(shape(title), nullptr);
      std::string form;
      if (!indicReorderForDisplay(title.c_str(), form)) form = title;  // no vowel sign to move
      std::string holder;
      EXPECT_EQ(form, ui_shaping::reordered(title.c_str(), holder));
      EXPECT_EQ(width(title), unshapedWidth(title));
    }
    EXPECT_EQ(reader.facesMade, 0);
    EXPECT_FALSE(ui_shaping::endPass());
  }
}

TEST_F(UiShapedTextTest, AFaceThatDoesNotMapTheScriptLeavesItReordered) {
  ReaderFace reader(kDevanagariFont);
  readFrom(reader);
  const std::string bengali = std::string(kBengaliKi) + " " + kBengaliKi;
  EXPECT_EQ(shape(bengali), nullptr);
  EXPECT_EQ(width(bengali), unshapedWidth(bengali));
  // The face was made for the question, and is UI text's to free.
  EXPECT_TRUE(ui_shaping::endPass());
}

TEST_F(UiShapedTextTest, ArabicHebrewLatinAndCjkAreNotShaped) {
  ReaderFace reader(kDevanagariFont);
  readFrom(reader);
  const std::string strings[] = {kArabic,
                                 kHebrew,
                                 "Pride and Prejudice",
                                 kJapanese,
                                 std::string(kHindi) + " " + kHebrew,  // a right-to-left character keeps the bidi path
                                 std::string(kArabic) + " " + kHindi};
  for (const std::string& text : strings) {
    SCOPED_TRACE(text);
    EXPECT_FALSE(ui_shaping::wantsShaping(text.c_str()));
    EXPECT_EQ(shape(text), nullptr);
    int w = 0;
    int h = 0;
    family.getTextDimensions(text.c_str(), &w, &h, REGULAR);
    if (!indic::containsIndic(text.c_str())) {
      EXPECT_EQ(width(text), w);
    }
  }
  EXPECT_EQ(reader.facesMade, 0);
}

TEST_F(UiShapedTextTest, APackThatDrawsTheScriptFirstKeepsItUnshaped) {
  ReaderFace reader(kDevanagariFont);
  readFrom(reader);
  FallbackFaceProvider::Packs packs;
  packs.covers = [](void*, const uint32_t cp) { return cp >= 0x0900 && cp <= 0x097F; };
  shared.setPacks(packs);
  EXPECT_EQ(shape(join({kHindi, kPatrika})), nullptr);
  // In the reader the book's face goes before the packs for what it maps: it shapes.
  shared.setReaderMaps([](void*, uint32_t) { return true; }, nullptr);
  EXPECT_NE(shape(join({kHindi, kPatrika})), nullptr);
}

TEST_F(UiShapedTextTest, AShapingBlockTheHeapRefusesLeavesTheTextReordered) {
  ReaderFace reader(kDevanagariFont);
  readFrom(reader);
  const std::string title = join({kHindi, kPatrika});
  uint32_t ppem = 0;
  reader.shapingFace(&ppem, nullptr)->current();  // the face and its tables first: only the shaping block is refused
  {
    const AllocationRefusal refusal(2048);
    EXPECT_EQ(shape(title), nullptr);
    EXPECT_EQ(width(title), unshapedWidth(title));
  }
  EXPECT_NE(shape(title), nullptr);
}

// --- Measured as drawn ---

TEST_F(UiShapedTextTest, TheWidthIsThePenOfTheDrawOverItsSegments) {
  ReaderFace reader(kDevanagariFont);
  readFrom(reader);
  const std::string title = join({kShri, kKrishna, "(" + std::string(kPrarthana) + ")"});
  const ui_shaping::Shaped* const shaped = shape(title);
  ASSERT_NE(shaped, nullptr);
  // GfxRenderer::drawShapedUiText's pen: a text segment moves it by its codepoint advance, a run as forEachGlyph
  // steps it, record by record.
  int pen = 0;
  for (const ui_shaping::Segment& segment : *shaped) {
    if (segment.glyphs) {
      int last = pen;
      int lastAdvance = 0;
      forEachGlyph(segment.run, pen, false, [&](const GlyphRecord& record, const int penX) {
        last = penX;
        lastAdvance = fp4::toPixel(record.advance);
      });
      EXPECT_EQ(last + lastAdvance - pen, segment.advance);
    } else {
      EXPECT_EQ(segment.advance, ui_shaping::codepointAdvance(family, segment.text, REGULAR));
    }
    pen += segment.advance;
  }
  EXPECT_EQ(pen, shaped->width());
  ui_shaping::endPass();
  EXPECT_EQ(width(title), pen);  // what rawTextWidth measures
}

TEST_F(UiShapedTextTest, AShapedRunsGlyphsComeByKeyFromTheReaderFaceAsItsInk) {
  ReaderFace reader(kDevanagariFont);
  readFrom(reader);
  const ui_shaping::Shaped* const shaped = shape(kKrishna);
  ASSERT_NE(shaped, nullptr);
  ASSERT_TRUE((*shaped)[0].glyphs);
  GlyphRecord first;
  forEachGlyph((*shaped)[0].run, 0, false, [&](const GlyphRecord& record, int) {
    if (first.glyph == 0) first = record;
  });
  ASSERT_NE(first.glyph, 0);
  // Through the UI face's own lookup, as GfxRenderer draws it: the reader face's glyph, packed to 1-bit ink.
  const EpdGlyphRef glyph = font.findGlyph(glyph_key::of(first.glyph));
  ASSERT_TRUE(glyph);
  EXPECT_EQ(glyph.width, 4 + first.glyph % 9);
  const uint8_t* bitmap = nullptr;
  ASSERT_TRUE(ui.provider->ownsBitmap(glyph.sdRecord, bitmap));
  ASSERT_NE(bitmap, nullptr);
  const uint16_t inkBytes = glyphDataBytes(glyph.width, glyph.height, false);
  for (uint16_t i = 0; i + 1 < inkBytes; ++i) EXPECT_EQ(bitmap[i], 0xFF) << i;
  // Glyph 0, the .notdef, is no real character: never lent.
  EXPECT_FALSE(font.findGlyph(glyph_key::of(0)));
  int above = 0;
  int below = 0;
  EXPECT_TRUE(ui_shaping::inkExtents(family, REGULAR, *shaped, &above, &below));
  EXPECT_GT(above, 0);
}

// --- Fitted ---

TEST_F(UiShapedTextTest, ATruncatedTitleIsCutBetweenSyllablesAndFitsAsFarAsItCan) {
  ReaderFace devanagari(kDevanagariFont);
  ReaderFace myanmar(kMyanmarFont);
  struct Case {
    ReaderFace* reader;
    std::string title;
  };
  const Case cases[] = {
      {&devanagari, join({kShri, kKrishna, kPrarthana, kSanskritam, kGyan})},
      {&devanagari, std::string(kHindi) + kPatrika + kSanskritam},  // one long run
      {&devanagari, std::string(kHindi) + ": The Hindi Magazine " + kPatrika},
      {&devanagari, "A " + std::string(kPrarthana)},
      {&myanmar, join({kMyanmar, kKyaung, kMyanmar})},
  };
  const std::string ellipsis = textTruncation::ELLIPSIS_UTF8;
  for (const Case& c : cases) {
    readFrom(*c.reader);
    const int full = width(c.title);
    ASSERT_GT(full, 0);
    for (int maxWidth = 4; maxWidth <= full + 8; maxWidth += 3) {
      const std::string out = truncated(c.title, maxWidth);
      SCOPED_TRACE(c.title + " maxWidth=" + std::to_string(maxWidth) + " out=" + out);
      if (out == c.title) {
        EXPECT_LE(full, maxWidth);
        continue;
      }
      ASSERT_GE(out.size(), ellipsis.size());
      ASSERT_EQ(out.substr(out.size() - ellipsis.size()), ellipsis);
      const size_t cut = out.size() - ellipsis.size();
      if (cut == 0) continue;  // nothing fits before the ellipsis
      ASSERT_EQ(c.title.compare(0, cut, out, 0, cut), 0);
      const std::string_view all(c.title);
      EXPECT_TRUE(ui_shaping::cutAllowed(all, cut));
      EXPECT_LT(width(out), maxWidth);
      // As far as it can: the next cut allowed would not fit.
      size_t next = cut;
      do {
        TextShaping::decodeAt(all, next);
      } while (next < all.size() && !ui_shaping::cutAllowed(all, next));
      if (next < all.size()) {
        EXPECT_GE(width(c.title.substr(0, next) + ellipsis), maxWidth);
      }
    }
  }
}

TEST_F(UiShapedTextTest, ATitleLongerThanOneShapingIsFittedFromItsHead) {
  ReaderFace reader(kDevanagariFont);
  readFrom(reader);
  std::string title;
  while (title.size() < 300) title += std::string(title.empty() ? "" : " ") + kPrarthana;
  EXPECT_EQ(shape(title), nullptr);  // past 255 bytes
  const std::string out = truncated(title, 100000);
  ASSERT_LT(out.size(), title.size());
  EXPECT_LE(out.size(), 255u);
  EXPECT_EQ(out.substr(out.size() - 3), textTruncation::ELLIPSIS_UTF8);
  EXPECT_NE(shape(out), nullptr);  // drawn shaped
}

TEST(UiShapedText, NoCutFallsInsideASyllableOrBeforeAMark) {
  const auto allowedAt = [](const std::string& text, const size_t at) { return ui_shaping::cutAllowed(text, at); };
  const std::string ki = kKi;  // क ि: the vowel sign belongs to the consonant
  EXPECT_FALSE(allowedAt(ki + ki, 3));
  EXPECT_TRUE(allowedAt(ki + ki, 6));
  EXPECT_FALSE(allowedAt(kGyan, 6));         // ज ् | ञ: after a virama
  EXPECT_FALSE(allowedAt(kMyanmar, 9));      // မ ြ န | ် : before asat's consonant
  EXPECT_TRUE(allowedAt(kMyanmar, 12));      // မြန် | မာ
  EXPECT_FALSE(allowedAt("e\xCC\x81t", 1));  // before a combining acute
  EXPECT_TRUE(allowedAt("et", 1));
}

// --- What the heap keeps ---

TEST_F(UiShapedTextTest, TheLastStringIsKeptForItsDrawAndNothingOutlivesThePass) {
  ReaderFace reader(kDevanagariFont);
  readFrom(reader);
  const std::string title = join({kHindi, kPatrika});
  const size_t before = heapLiveBytes();
  const ui_shaping::Shaped* const measured = shape(title);
  ASSERT_NE(measured, nullptr);
  EXPECT_EQ(shape(title), measured);  // the draw after the measure shapes nothing again
  EXPECT_EQ(reader.facesMade, 1);
  EXPECT_GT(heapLiveBytes(), before);
  EXPECT_TRUE(ui_shaping::endPass());  // UI text made the face: the caller frees it
  reader.releaseShapingFace();
  EXPECT_EQ(heapLiveBytes(), before);
  EXPECT_FALSE(ui_shaping::endPass());
}

TEST_F(UiShapedTextTest, AFaceASectionBuildHoldsIsLeftToIt) {
  ReaderFace reader(kDevanagariFont);
  readFrom(reader);
  uint32_t ppem = 0;
  ASSERT_NE(reader.shapingFace(&ppem, nullptr), nullptr);  // the build's
  ASSERT_NE(shape(join({kHindi, kPatrika})), nullptr);
  EXPECT_FALSE(ui_shaping::endPass());
  EXPECT_NE(reader.face, nullptr);
}

// The heap and the time a shaped Library page takes on the host: eight rows of a title and an author, each fitted
// (truncatedText) and drawn (the shaped form the fitting kept). Printed for the record; the assertions are the
// lifetimes. The fixture's face copies its font (the device's maps the glyph store), so it is made before the count
// starts; the plan the page's first title builds in it is counted.
TEST_F(UiShapedTextTest, ALibraryPageOfShapedTitles) {
  ReaderFace reader(kDevanagariFont);
  readFrom(reader);
  const std::vector<std::string> titles = {
      join({kShri, kKrishna, kPrarthana}),
      join({kHindi, kPatrika}),
      join({kSanskritam, kGyan, kKrishna}),
      join({kPrarthana, kShri, kHindi, kPatrika, kGyan, kSanskritam}),
      join({kGyan, kHindi}),
      join({kKrishna, kSanskritam, kPrarthana, kShri}),
      join({kPatrika, kGyan}),
      join({kHindi, kShri, kKrishna}),
  };
  const std::string author = join({kShri, kKrishna});
  uint32_t ppem = 0;
  reader.shapingFace(&ppem, nullptr)->current();
  const size_t before = heapLiveBytes();
  resetHeapPeak();
  size_t held = 0;
  size_t largestKept = 0;
  const auto start = std::chrono::steady_clock::now();
  for (const std::string& title : titles) {
    for (const std::string* text : {&title, &author}) {
      const std::string fitted = truncated(*text, 200);
      const size_t withPlan = heapLiveBytes() - before;
      family.beginText(fitted.c_str(), REGULAR);
      ASSERT_NE(ui_shaping::shape(family, REGULAR, fitted.c_str()), nullptr);
      EXPECT_EQ(heapLiveBytes() - before, withPlan);  // the draw found the fitting's shaping
      held = std::max(held, withPlan);
    }
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
  const size_t peak = heapPeakBytes() - before;
  const size_t keptBeforeEnd = heapLiveBytes() - before;
  EXPECT_FALSE(ui_shaping::endPass());  // the face was there before the page: not UI text's
  const size_t plan = heapLiveBytes() - before;
  largestKept = held - plan;
  reader.releaseShapingFace();
  std::printf(
      "[ library page ] 16 strings fitted and drawn: %lld us on the host; heap peak +%zu B (the shaping "
      "block), held between strings <= %zu B (the string kept <= %zu B, the Devanagari plan %zu B); %zu B "
      "kept at the pass's end, the plan's alone after it\n",
      static_cast<long long>(elapsed), peak, held, largestKept, plan, keptBeforeEnd);
}

TEST_F(UiShapedTextTest, TheTimeOfATitle) {
  ReaderFace reader(kDevanagariFont);
  readFrom(reader);
  const std::string title = join({kShri, kKrishna, kPrarthana, kHindi});  // four words, 72 bytes
  std::string longTitle;
  while (longTitle.size() < 240) longTitle += std::string(longTitle.empty() ? "" : " ") + kSanskritam;
  constexpr int kRounds = 200;
  const auto timeOf = [&](const auto& work) {
    work();  // the face and its plan, made once a pass on the device
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kRounds; ++i) {
      ui_shaping::endPass();  // each round shapes again, as each render pass does
      work();
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count() /
           kRounds / 1000.0;
  };
  // A fitting title: truncatedText's shaping, which drawText then finds kept.
  const double fits = timeOf([&] { EXPECT_EQ(truncated(title, 1000), title); });
  // A long title cut to a row: the whole string shaped, the cut run up to each cut tried, the cut string shaped.
  const double cut = timeOf([&] { EXPECT_NE(truncated(longTitle, 150), longTitle); });
  // The same with the face made again each round: its plan built, as when UI text made it and the pass freed it.
  const double cold = timeOf([&] {
    reader.releaseShapingFace();
    EXPECT_EQ(truncated(title, 1000), title);
  });
  const double unshaped = timeOf([&] {
    std::string holder;
    int w = 0;
    int h = 0;
    family.getTextDimensions(ui_shaping::reordered(title.c_str(), holder), &w, &h, REGULAR);
  });
  std::printf(
      "[ title time ] fit+draw of a four-word title %.1f us, %.1f us with its face and plan made (unshaped "
      "measure %.1f us); a %zu-byte title cut to 150 px %.1f us\n",
      fits, cold, unshaped, longTitle.size(), cut);
}

}  // namespace
