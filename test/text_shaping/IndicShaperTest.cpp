// Ours (writing modes step 2, PR 3): IndicShaper::shapeToken over FixtureShapingFace faces. The HarfBuzz references are
// test/complex_shaper's ExpectedShaping.h and test/ttf_font's ExpectedGlyphRuns.h; the piecewise and coverage cases
// follow crosspoint-reader PR #3787's ComplexShaperTest.
#include <BuildArena.h>
#include <GlyphRun.h>
#include <OtShaper.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "ExpectedGlyphRuns.h"
#include "ExpectedShaping.h"
#include "FixtureShapingFace.h"
#include "IndicScripts.h"
#include "IndicShaper.h"
#include "OtShaperTestHelpers.h"
#include "TextShaping.h"

namespace IndicShaper {
// gtest prints these through ADL.
void PrintTo(const Outcome outcome, std::ostream* os) {
  *os << (outcome == Outcome::Plain ? "Plain" : outcome == Outcome::Formed ? "Formed" : "Degraded");
}
void PrintTo(const PieceKind kind, std::ostream* os) {
  *os << (kind == PieceKind::Text ? "Text" : kind == PieceKind::Glyphs ? "Glyphs" : "Reordered");
}
}  // namespace IndicShaper

namespace {

using IndicShaper::CallMemory;
using IndicShaper::Outcome;
using IndicShaper::PieceKind;
using IndicShaper::Request;
using ottest::StoredGlyph;

constexpr const char* kKa = "\xE0\xA4\x95";                     // क
constexpr const char* kMa = "\xE0\xA4\xAE";                     // म
constexpr const char* kLa = "\xE0\xA4\xB2";                     // ल
constexpr const char* kRa = "\xE0\xA4\xB0";                     // र
constexpr const char* kKi = "\xE0\xA4\x95\xE0\xA4\xBF";         // कि
constexpr const char* kIKa = "\xE0\xA4\xBF\xE0\xA4\x95";        // ि क: कि in visual order
constexpr const char* kBengaliKi = "\xE0\xA6\x95\xE0\xA6\xBF";  // কি
constexpr const char* kBengaliKa = "\xE0\xA6\x95";              // ক
constexpr const char* kBengaliKe = "\xE0\xA6\x95\xE0\xA7\x87";  // কে
constexpr const char* kTamilKa = "\xE0\xAE\x95";                // க
constexpr const char* kTamilKe = "\xE0\xAE\x95\xE0\xAF\x87";    // கே
constexpr const char* kDanda = "\xE0\xA5\xA4";                  // ।
// তাঁদের: upstream's tail for its long word, whose e-kar must not take a word-start form where a piece begins.
constexpr const char* kTader = "\xE0\xA6\xA4\xE0\xA6\xBE\xE0\xA6\x81\xE0\xA6\xA6\xE0\xA7\x87\xE0\xA6\xB0";

struct Piece {
  PieceKind kind;
  uint16_t begin;
  uint16_t end;
  std::vector<StoredGlyph> glyphs;
  std::string bytes;
};

// The pieces shapeToken writes. `room`: how many calls it answers before it has none (-1: no limit).
class RecordingSink final : public IndicShaper::PieceSink {
 public:
  std::vector<Piece> pieces;
  int calls = 0;
  int room = -1;

  bool beginPiece(const PieceKind kind, const uint16_t begin, const uint16_t end) override {
    if (!take()) return false;
    pieces.push_back({kind, begin, end, {}, {}});
    return true;
  }
  bool addGlyph(const GlyphRecord& record) override {
    if (!take() || pieces.empty()) return false;
    pieces.back().glyphs.push_back({record.glyph, record.advance, record.dx, record.dy});
    return true;
  }
  bool addBytes(const char* bytes, const size_t length) override {
    if (!take() || pieces.empty()) return false;
    pieces.back().bytes.append(bytes, length);
    return true;
  }

 private:
  bool take() {
    ++calls;
    return room < 0 || calls <= room;
  }
};

// Counts what it is given and keeps nothing, so it allocates nothing.
class CountingSink final : public IndicShaper::PieceSink {
 public:
  int pieces = 0;
  int glyphs = 0;
  bool beginPiece(PieceKind, uint16_t, uint16_t) override {
    ++pieces;
    return true;
  }
  bool addGlyph(const GlyphRecord&) override {
    ++glyphs;
    return true;
  }
  bool addBytes(const char*, size_t) override { return true; }
};

struct Shaped {
  Outcome outcome;
  std::vector<Piece> pieces;
};

// `text` shaped in a call of its own, in the build's heap block.
Shaped shape(std::string_view text, const Request& request,
             const unsigned runCodepoints = IndicShaper::kMaxRunCodepoints) {
  RecordingSink sink;
  CallMemory memory(nullptr, text.size(), runCodepoints);
  const Outcome outcome = IndicShaper::shapeToken(text, request, memory, sink);
  return {outcome, std::move(sink.pieces)};
}

Request on(ShapingFace* face, const char* language = "", const uint32_t ppem26_6 = kFixturePpem26_6) {
  Request request;
  request.face = face;
  request.ppem26_6 = ppem26_6;
  request.languageTags = ot::languageTagsFor(language);
  return request;
}

const ShapingFixture& fixtureOf(const char* script) {
  for (const ShapingFixture& fx : kShapingFixtures) {
    if (std::strcmp(fx.script, script) == 0) return fx;
  }
  ADD_FAILURE() << script;
  return kShapingFixtures[0];
}

std::vector<uint8_t> layoutFont(const char* script) { return ottest::readFixture(fixtureOf(script).layoutFile); }

std::vector<StoredGlyph> storedOf(const ExpectedShaping& word) {
  std::vector<StoredGlyph> out;
  for (unsigned i = 0; i < word.count; ++i) {
    out.push_back({word.glyphs[i].gid, word.glyphs[i].advance12_4, word.glyphs[i].dx, word.glyphs[i].dy});
  }
  return out;
}

std::vector<StoredGlyph> storedOf(const ExpectedRun& run) {
  std::vector<StoredGlyph> out;
  for (unsigned i = 0; i < run.count; ++i) {
    out.push_back({run.glyphs[i].gid, run.glyphs[i].advance12_4, run.glyphs[i].dx, run.glyphs[i].dy});
  }
  return out;
}

std::vector<StoredGlyph> storedOf(const ot::Buffer& buffer) {
  std::vector<StoredGlyph> out;
  for (unsigned i = 0; i < buffer.len(); ++i) {
    out.push_back({buffer.info[i].codepoint, (buffer.pos[i].xAdvance + 2) >> 2,
                   ottest::roundPixels(buffer.pos[i].xOffset), -ottest::roundPixels(buffer.pos[i].yOffset)});
  }
  return out;
}

// What ot::shape gives for before + text + after with the neighbours as context (RunContext{before, after, partial}),
// on `face` in the default language at `ppem26_6`; empty when it fails.
std::vector<StoredGlyph> shapedWithContext(FixtureShapingFace& face, const ot::Script script, const std::string& before,
                                           const std::string& text, const std::string& after,
                                           const bool partial = false, const uint32_t ppem26_6 = kFixturePpem26_6) {
  const ot::Plan* plan = face.plan(script, ot::languageTagsFor(""));
  const ot::Face* shaping = face.current();
  if (plan == nullptr || shaping == nullptr) return {};
  const auto contextBefore = static_cast<unsigned>(ottest::utf32(before.c_str()).size());
  const auto contextAfter = static_cast<unsigned>(ottest::utf32(after.c_str()).size());
  const std::vector<uint32_t> cps = ottest::utf32((before + text + after).c_str());
  ottest::BoundBuffer bound(256);
  ot::Scale scale;
  scale.set(static_cast<int32_t>(ppem26_6), (ppem26_6 + 32) >> 6, shaping->upem());
  if (!ot::shape(*shaping, scale, *plan, cps.data(), static_cast<unsigned>(cps.size()), bound.buffer,
                 ot::RunContext{contextBefore, contextAfter, partial})) {
    return {};
  }
  return storedOf(bound.buffer);
}

// The code points of `text`'s longest syllable (indic::syllableBreakAllowed's breaks): the least a call takes it in.
unsigned longestSyllable(const std::string& text) {
  const std::vector<uint32_t> cps = ottest::utf32(text.c_str());
  unsigned longest = 0;
  unsigned current = 0;
  for (size_t i = 0; i < cps.size(); ++i) {
    current = i > 0 && indic::syllableBreakAllowed(cps[i - 1], cps[i]) ? 1 : current + 1;
    longest = std::max(longest, current);
  }
  return longest;
}

// A GSUB of one lookup, ccmp in the dev2 script's default language system: a MultipleSubst of `glyph` to no glyph,
// which deletes it. Big-endian fields, each table at the offset its parent names.
std::vector<uint8_t> deletingGsub(const uint16_t glyph) {
  const uint16_t fields[] = {
      1, 0,      10,     30, 44,  // the header (version 1.0) and its script, feature and lookup lists
      1, 0x6465, 0x7632, 8,       // the script list (at 10): one script, "dev2"
      4, 0,                       // the script (at 18): its default language system, no other
      0, 0xFFFF, 1,      0,       // the language system (at 22): no required feature, feature 0
      1, 0x6363, 0x6D70, 8,       // the feature list (at 30): one feature, "ccmp"
      0, 1,      0,               // the feature (at 38): lookup 0
      1, 4,                       // the lookup list (at 44): one lookup
      2, 0,      1,      8,       // the lookup (at 48): a MultipleSubst of one subtable
      1, 10,     1,      8,       // the subtable (at 56): its coverage at 66, its one sequence at 64
      0,                          // the sequence (at 64): no glyph
      1, 1,      glyph,           // the coverage (at 66): the glyph
  };
  std::vector<uint8_t> out;
  for (const uint16_t field : fields) {
    out.push_back(static_cast<uint8_t>(field >> 8));
    out.push_back(static_cast<uint8_t>(field & 0xFF));
  }
  return out;
}

// A face over a font with its GSUB replaced by `gsub`, and no GPOS, CPac or CPpl: it computes its filters.
class GsubFace final : public ShapingFace {
 public:
  GsubFace(std::vector<uint8_t> font, std::vector<uint8_t> gsub) : font_(std::move(font)), gsub_(std::move(gsub)) {}

 protected:
  const uint8_t* map() override { return font_.empty() ? nullptr : font_.data(); }
  bool tables(const uint8_t* base, ot::FaceTables* out) override {
    if (!ot::tablesFromSfnt(base, static_cast<uint32_t>(font_.size()), out)) return false;
    (*out)[ot::FaceTables::GSUB_TABLE] = ot::Table(gsub_.data(), static_cast<uint32_t>(gsub_.size()));
    (*out)[ot::FaceTables::GPOS_TABLE] = ot::Table();
    (*out)[ot::FaceTables::CPAC] = ot::Table();
    (*out)[ot::FaceTables::CPPL] = ot::Table();
    return true;
  }

 private:
  std::vector<uint8_t> font_;
  std::vector<uint8_t> gsub_;
};

// The build's heap block released before and after a test that counts it.
struct BuildMemoryScope {
  BuildMemoryScope() { TextShaping::releaseBuildMemory(); }
  ~BuildMemoryScope() { TextShaping::releaseBuildMemory(); }
};

}  // namespace

TEST(IndicShaper, MatchesHarfBuzzInEveryScript) {
  size_t words = 0;
  for (const ShapingFixture& fx : kShapingFixtures) {
    FixtureShapingFace face(ottest::readFixture(fx.layoutFile));
    for (size_t w = 0; w < fx.count; ++w) {
      const ExpectedShaping& word = fx.words[w];
      const Shaped shaped = shape(word.utf8, on(&face, word.language));
      ASSERT_EQ(Outcome::Formed, shaped.outcome) << fx.script << " " << w << ": " << word.utf8;
      ASSERT_EQ(1u, shaped.pieces.size()) << fx.script << " " << w << ": " << word.utf8;
      EXPECT_EQ(PieceKind::Glyphs, shaped.pieces[0].kind) << fx.script << " " << w;
      EXPECT_EQ(0, shaped.pieces[0].begin) << fx.script << " " << w;
      EXPECT_EQ(std::strlen(word.utf8), shaped.pieces[0].end) << fx.script << " " << w;
      EXPECT_EQ(storedOf(word), shaped.pieces[0].glyphs) << fx.script << " " << w << ": " << word.utf8;
      ++words;
    }
  }
  EXPECT_GT(words, 50u);
}

TEST(IndicShaper, MatchesHarfBuzzOnTheTrueTypeSubset) {
  size_t runs = 0;
  for (const RunSet& set : kDevanagariRunSets) {
    FixtureShapingFace face(readFont(TTF_FIXTURE_DIR, set.file));
    for (size_t r = 0; r < set.count; ++r) {
      const ExpectedRun& run = set.runs[r];
      const Shaped shaped = shape(run.utf8, on(&face, run.language, set.ppem26_6));
      ASSERT_EQ(Outcome::Formed, shaped.outcome) << set.file << " at " << set.ppem26_6 << ", run " << r;
      ASSERT_EQ(1u, shaped.pieces.size()) << set.file << " at " << set.ppem26_6 << ", run " << r;
      EXPECT_EQ(PieceKind::Glyphs, shaped.pieces[0].kind);
      EXPECT_EQ(storedOf(run), shaped.pieces[0].glyphs)
          << set.file << " at " << set.ppem26_6 << ", run " << r << ": " << run.utf8;
      ++runs;
    }
  }
  EXPECT_EQ(64u, runs);
}

TEST(IndicShaper, LeavesTextWithoutIndicPlain) {
  FixtureShapingFace face(layoutFont("Devanagari"));
  for (const Request& request : {on(&face), on(nullptr)}) {
    for (const char* text : {"hello", kDanda}) {
      RecordingSink sink;
      CallMemory memory(nullptr, std::strlen(text));
      EXPECT_EQ(Outcome::Plain, IndicShaper::shapeToken(text, request, memory, sink)) << text;
      EXPECT_EQ(0, sink.calls) << text;
    }
  }
}

TEST(IndicShaper, CutsPiecesAtRunEdges) {
  FixtureShapingFace face(layoutFont("Bengali"));
  const std::string text = std::string("(") + kBengaliKi + "),";
  const Shaped shaped = shape(text, on(&face));
  ASSERT_EQ(Outcome::Formed, shaped.outcome);
  ASSERT_EQ(3u, shaped.pieces.size());
  EXPECT_EQ(PieceKind::Text, shaped.pieces[0].kind);
  EXPECT_EQ(0, shaped.pieces[0].begin);
  EXPECT_EQ(1, shaped.pieces[0].end);
  EXPECT_EQ(PieceKind::Glyphs, shaped.pieces[1].kind);
  EXPECT_EQ(1, shaped.pieces[1].begin);
  EXPECT_EQ(7, shaped.pieces[1].end);
  EXPECT_EQ(storedOf(kBengaliShaping[1]), shaped.pieces[1].glyphs);  // কি
  EXPECT_EQ(PieceKind::Text, shaped.pieces[2].kind);
  EXPECT_EQ(7, shaped.pieces[2].begin);
  EXPECT_EQ(9, shaped.pieces[2].end);
  EXPECT_TRUE(shaped.pieces[0].glyphs.empty() && shaped.pieces[0].bytes.empty());
  EXPECT_TRUE(shaped.pieces[2].glyphs.empty() && shaped.pieces[2].bytes.empty());
}

TEST(IndicShaper, ReordersAScriptTheFaceDoesNotMap) {
  FixtureShapingFace face(layoutFont("Bengali"));
  const std::string text = std::string(kKi) + kBengaliKi;
  const Shaped shaped = shape(text, on(&face));
  ASSERT_EQ(Outcome::Formed, shaped.outcome);
  ASSERT_EQ(2u, shaped.pieces.size());
  EXPECT_EQ(PieceKind::Reordered, shaped.pieces[0].kind);
  EXPECT_EQ(0, shaped.pieces[0].begin);
  EXPECT_EQ(6, shaped.pieces[0].end);
  EXPECT_EQ(kIKa, shaped.pieces[0].bytes);
  EXPECT_EQ(PieceKind::Glyphs, shaped.pieces[1].kind);
  EXPECT_EQ(6, shaped.pieces[1].begin);
  EXPECT_EQ(12, shaped.pieces[1].end);
  EXPECT_EQ(storedOf(kBengaliShaping[1]), shaped.pieces[1].glyphs);
}

TEST(IndicShaper, AFaceWithoutLayoutStillReorders) {
  FixtureShapingFace face(layoutFont("Devanagari"), FixtureFaceOptions{.keepLayout = false});
  const Shaped shaped = shape(kKi, on(&face));
  ASSERT_EQ(Outcome::Formed, shaped.outcome);
  ASSERT_EQ(1u, shaped.pieces.size());
  EXPECT_EQ(PieceKind::Glyphs, shaped.pieces[0].kind);
  const ot::Face* shaping = face.current();
  ASSERT_NE(nullptr, shaping);
  uint32_t iMatra = 0;
  uint32_t ka = 0;
  ASSERT_TRUE(shaping->nominalGlyph(0x093F, &iMatra));
  ASSERT_TRUE(shaping->nominalGlyph(0x0915, &ka));
  ASSERT_EQ(2u, shaped.pieces[0].glyphs.size());
  EXPECT_EQ(iMatra, shaped.pieces[0].glyphs[0].gid);  // the i-matra drawn first, as the shaper reordered it
  EXPECT_EQ(ka, shaped.pieces[0].glyphs[1].gid);
}

TEST(IndicShaper, WithoutAFaceTheWholeTokenIsReordered) {
  const std::string text = std::string(kKi) + ",";
  const Shaped shaped = shape(text, on(nullptr));
  ASSERT_EQ(Outcome::Formed, shaped.outcome);
  ASSERT_EQ(1u, shaped.pieces.size());
  EXPECT_EQ(PieceKind::Reordered, shaped.pieces[0].kind);
  EXPECT_EQ(0, shaped.pieces[0].begin);
  EXPECT_EQ(7, shaped.pieces[0].end);
  EXPECT_EQ(std::string(kIKa) + ",", shaped.pieces[0].bytes);
}

TEST(IndicShaper, ShapesInTheBooksLanguage) {
  FixtureShapingFace face(layoutFont("Devanagari"));
  Request byDefault = on(&face);
  byDefault.languageTags = nullptr;  // Request's own default: the script's default language system
  const Shaped none = shape(kLa, byDefault);
  const Shaped marathi = shape(kLa, on(&face, "mr"));
  ASSERT_EQ(Outcome::Formed, none.outcome);
  ASSERT_EQ(Outcome::Formed, marathi.outcome);
  ASSERT_EQ(1u, none.pieces.size());
  ASSERT_EQ(1u, marathi.pieces.size());
  EXPECT_NE(none.pieces[0].glyphs, marathi.pieces[0].glyphs);
}

TEST(IndicShaper, ARunLongerThanTheCallShapesInSyllables) {
  FixtureShapingFace face(layoutFont("Bengali"));
  const std::string word = std::string(kBengaliShaping[5].utf8) + kBengaliShaping[6].utf8 + kBengaliShaping[7].utf8 +
                           kBengaliShaping[8].utf8 + kTader;
  ASSERT_GT(ottest::utf32(word.c_str()).size(), 3u * 8);  // several pieces of 8 code points
  const Shaped whole = shape(word, on(&face), 64);
  const Shaped pieces = shape(word, on(&face), 8);
  ASSERT_EQ(Outcome::Formed, whole.outcome);
  ASSERT_EQ(Outcome::Formed, pieces.outcome);
  ASSERT_EQ(1u, whole.pieces.size());
  ASSERT_EQ(1u, pieces.pieces.size());
  EXPECT_EQ(PieceKind::Glyphs, pieces.pieces[0].kind);
  EXPECT_FALSE(whole.pieces[0].glyphs.empty());
  EXPECT_EQ(whole.pieces[0].glyphs, pieces.pieces[0].glyphs) << "pieces shaped with their neighbours match the whole";

  // Every call that holds the longest syllable (র্ত্তৃ, 6 code points) gives the whole's records; without the context
  // levels, calls of 6, 14 and 37 would not. A syllable longer than the call cannot be shaped.
  const unsigned length = static_cast<unsigned>(ottest::utf32(word.c_str()).size());
  for (unsigned n = 6; n < length; ++n) {
    const Shaped at = shape(word, on(&face), n);
    ASSERT_EQ(Outcome::Formed, at.outcome) << n;
    ASSERT_EQ(1u, at.pieces.size()) << n;
    EXPECT_EQ(whole.pieces[0].glyphs, at.pieces[0].glyphs) << "a call of " << n << " code points";
  }
  EXPECT_EQ(Outcome::Degraded, shape(word, on(&face), 5).outcome);
}

TEST(IndicShaper, ALongRunKeepsItsGluedNeighbourInItsFirstPiece) {
  FixtureShapingFace face(layoutFont("Bengali"));
  // কে then শকুন্তলাপ্রকাশ: the e-kar after a glued ক takes no word-start form, in whichever piece it falls.
  const std::string word = std::string(kBengaliKe) + kBengaliShaping[5].utf8 + kBengaliShaping[6].utf8;
  Request request = on(&face);
  request.before = kBengaliKa;
  const Shaped whole = shape(word, request, 64);
  const Shaped alone = shape(word, on(&face), 64);
  ASSERT_EQ(Outcome::Formed, whole.outcome);
  ASSERT_EQ(Outcome::Formed, alone.outcome);
  ASSERT_EQ(1u, whole.pieces.size());
  ASSERT_EQ(1u, alone.pieces.size());
  EXPECT_NE(alone.pieces[0].glyphs, whole.pieces[0].glyphs);
  // Every call that holds the longest syllable (ন্ত, 3 code points) gives the whole's records.
  const unsigned length = static_cast<unsigned>(ottest::utf32(word.c_str()).size());
  for (unsigned n = 3; n < length; ++n) {
    const Shaped at = shape(word, request, n);
    ASSERT_EQ(Outcome::Formed, at.outcome) << n;
    ASSERT_EQ(1u, at.pieces.size()) << n;
    EXPECT_EQ(whole.pieces[0].glyphs, at.pieces[0].glyphs) << "a call of " << n << " code points";
  }
}

TEST(IndicShaper, ALongRunKeepsItsGluedNeighbourInItsLastPiece) {
  FixtureShapingFace face(layoutFont("Tamil"));
  // பொன்னியின்செல்வன் then கக, with கே glued after: the last KA is kerned against the neighbour's pre-base e, in
  // whichever piece it falls, and none of the neighbour's glyphs is output.
  const std::string word = std::string(kTamilShaping[6].utf8) + kTamilShaping[7].utf8 + kTamilKa + kTamilKa;
  Request request = on(&face);
  request.after = kTamilKe;
  const Shaped whole = shape(word, request, 64);
  const Shaped alone = shape(word, on(&face), 64);
  ASSERT_EQ(Outcome::Formed, whole.outcome);
  ASSERT_EQ(Outcome::Formed, alone.outcome);
  ASSERT_EQ(1u, whole.pieces.size());
  ASSERT_EQ(1u, alone.pieces.size());
  EXPECT_NE(alone.pieces[0].glyphs, whole.pieces[0].glyphs);
  EXPECT_EQ(shapedWithContext(face, ot::Script::Tamil, "", word, kTamilKe), whole.pieces[0].glyphs);
  // From 6 code points the last piece, க, holds the syllable before it (ன்க) and the neighbour's (கே) as context. A
  // smaller call falls back to one code point of context, as upstream's loop does, and the kerning goes with it.
  const unsigned length = static_cast<unsigned>(ottest::utf32(word.c_str()).size());
  for (unsigned n = 6; n < length; ++n) {
    const Shaped at = shape(word, request, n);
    ASSERT_EQ(Outcome::Formed, at.outcome) << n;
    ASSERT_EQ(1u, at.pieces.size()) << n;
    EXPECT_EQ(whole.pieces[0].glyphs, at.pieces[0].glyphs) << "a call of " << n << " code points";
  }
}

TEST(IndicShaper, TheNeighboursContextIsItsNearestSyllable) {
  {
    // Before: the neighbour's last syllable, ক, whatever comes before it in the neighbour.
    FixtureShapingFace face(layoutFont("Bengali"));
    const std::string kaaKa = std::string("\xE0\xA6\x95\xE0\xA6\xBE") + kBengaliKa;  // কাক: two syllables of Bengali
    for (const std::string& before : {std::string("(") + kBengaliKa, kaaKa}) {
      Request request = on(&face);
      request.before = before;
      const Shaped glued = shape(kBengaliKe, request);
      ASSERT_EQ(Outcome::Formed, glued.outcome) << before;
      ASSERT_EQ(1u, glued.pieces.size()) << before;
      EXPECT_EQ(shapedWithContext(face, ot::Script::Bengali, kBengaliKa, kBengaliKe, ""), glued.pieces[0].glyphs)
          << before;
    }
  }
  {
    // After: the neighbour's first syllable, கே, whatever follows it in the neighbour.
    FixtureShapingFace face(layoutFont("Tamil"));
    for (const std::string& after : {std::string(kTamilKe) + ")", std::string(kTamilKe) + kTamilKa}) {
      Request request = on(&face);
      request.after = after;
      const Shaped glued = shape(kTamilKa, request);
      ASSERT_EQ(Outcome::Formed, glued.outcome) << after;
      ASSERT_EQ(1u, glued.pieces.size()) << after;
      EXPECT_EQ(shapedWithContext(face, ot::Script::Tamil, "", kTamilKa, kTamilKe), glued.pieces[0].glyphs) << after;
    }
  }
}

TEST(IndicShaper, ANeighbourSyllableLongerThanTheContextIsCutNearTheToken) {
  FixtureShapingFace face(layoutFont("Devanagari"));
  // क्क्क्क्क: one syllable of 9 code points. Before the token the 8 nearest it are context (ending in KA, so RA stays
  // a letter; the 8 farthest would end in a virama and take RA into a conjunct), after it the 8 first; either is cut,
  // so partial.
  const std::string kaKaKa =
      "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\x95"
      "\xE0\xA5\x8D\xE0\xA4\x95";
  ASSERT_EQ(9u, longestSyllable(kaKaKa));
  ASSERT_EQ(9u, ottest::utf32(kaKaKa.c_str()).size());
  const std::string nearestBefore = kaKaKa.substr(3);                  // ्क्क्क्क
  const std::string firstAfter = kaKaKa.substr(0, kaKaKa.size() - 3);  // क्क्क्क्
  {
    Request request = on(&face);
    request.before = kaKaKa;
    const Shaped glued = shape(kRa, request);
    ASSERT_EQ(Outcome::Formed, glued.outcome);
    ASSERT_EQ(1u, glued.pieces.size());
    EXPECT_EQ(shapedWithContext(face, ot::Script::Devanagari, nearestBefore, kRa, "", true), glued.pieces[0].glyphs);
    EXPECT_NE(shapedWithContext(face, ot::Script::Devanagari, firstAfter, kRa, "", true), glued.pieces[0].glyphs);
  }
  {
    Request request = on(&face);
    request.after = kaKaKa;
    const Shaped glued = shape(kRa, request);
    ASSERT_EQ(Outcome::Formed, glued.outcome);
    ASSERT_EQ(1u, glued.pieces.size());
    EXPECT_EQ(shapedWithContext(face, ot::Script::Devanagari, "", kRa, firstAfter, true), glued.pieces[0].glyphs);
  }
}

TEST(IndicShaper, AGluedNeighbourIsContextNotOutput) {
  FixtureShapingFace face(layoutFont("Devanagari"));
  Request request = on(&face);
  request.before = kKa;
  const Shaped shaped = shape(kMa, request);
  ASSERT_EQ(Outcome::Formed, shaped.outcome);
  ASSERT_EQ(1u, shaped.pieces.size());
  EXPECT_EQ(0, shaped.pieces[0].begin);
  EXPECT_EQ(3, shaped.pieces[0].end);
  // कम with RunContext{1, 0}
  EXPECT_EQ(shapedWithContext(face, ot::Script::Devanagari, kKa, kMa, ""), shaped.pieces[0].glyphs);
}

TEST(IndicShaper, AGluedNeighbourShapesTheTokensEdge) {
  {
    // Upstream's word-start case: the Bengali e-kar takes its word-start form unless a letter comes before it.
    FixtureShapingFace face(layoutFont("Bengali"));
    Request request = on(&face);
    request.before = kBengaliKa;
    const Shaped glued = shape(kBengaliKe, request);
    const Shaped alone = shape(kBengaliKe, on(&face));
    ASSERT_EQ(Outcome::Formed, glued.outcome);
    ASSERT_EQ(Outcome::Formed, alone.outcome);
    ASSERT_EQ(1u, glued.pieces.size());
    ASSERT_EQ(1u, alone.pieces.size());
    EXPECT_NE(alone.pieces[0].glyphs, glued.pieces[0].glyphs);
    EXPECT_EQ(shapedWithContext(face, ot::Script::Bengali, kBengaliKa, kBengaliKe, ""), glued.pieces[0].glyphs);
  }
  {
    // Tamil KA before a syllable with a pre-base e: the neighbour after it is context too.
    FixtureShapingFace face(layoutFont("Tamil"));
    Request request = on(&face);
    request.after = kTamilKe;
    const Shaped glued = shape(kTamilKa, request);
    const Shaped alone = shape(kTamilKa, on(&face));
    ASSERT_EQ(Outcome::Formed, glued.outcome);
    ASSERT_EQ(Outcome::Formed, alone.outcome);
    ASSERT_EQ(1u, glued.pieces.size());
    ASSERT_EQ(1u, alone.pieces.size());
    EXPECT_NE(alone.pieces[0].glyphs, glued.pieces[0].glyphs);
    EXPECT_EQ(shapedWithContext(face, ot::Script::Tamil, "", kTamilKa, kTamilKe), glued.pieces[0].glyphs);
  }
}

TEST(IndicShaper, ANeighbourOfAnotherScriptIsNoContext) {
  FixtureShapingFace face(layoutFont("Bengali"));
  const Shaped alone = shape(kBengaliKe, on(&face));
  ASSERT_EQ(Outcome::Formed, alone.outcome);
  for (const char* neighbour : {kKa, "a"}) {
    Request request = on(&face);
    request.before = neighbour;
    const Shaped glued = shape(kBengaliKe, request);
    ASSERT_EQ(Outcome::Formed, glued.outcome) << neighbour;
    ASSERT_EQ(1u, glued.pieces.size()) << neighbour;
    EXPECT_EQ(alone.pieces[0].glyphs, glued.pieces[0].glyphs) << neighbour;
  }
}

TEST(IndicShaper, ARunTheFontEmptiesIsDegraded) {
  // A glyph form of no glyphs is no form (a line would draw the run's code points at no width): a run whose every
  // glyph the font's GSUB deletes is laid out as its code points, as a run that cannot be shaped is. Shaped whole (क)
  // and in pieces (40 of them, past kMaxRunCodepoints).
  const std::vector<uint8_t> font = layoutFont("Devanagari");
  FixtureShapingFace plain(font);
  uint32_t ka = 0;
  ASSERT_NE(nullptr, plain.current());
  ASSERT_TRUE(plain.current()->nominalGlyph(0x0915, &ka));
  std::string many;
  for (int i = 0; i < 40; ++i) many += kKa;
  ASSERT_GT(ottest::utf32(many.c_str()).size(), IndicShaper::kMaxRunCodepoints);
  GsubFace deleting(font, deletingGsub(static_cast<uint16_t>(ka)));
  for (const std::string& text : {std::string(kKa), many}) {
    EXPECT_EQ(Outcome::Degraded, shape(text, on(&deleting)).outcome) << text.size() << " B";
  }
  // The control: the lookup deletes another glyph, and the same runs are formed.
  GsubFace other(font, deletingGsub(static_cast<uint16_t>(ka + 1)));
  for (const std::string& text : {std::string(kKa), many}) {
    const Shaped shaped = shape(text, on(&other));
    ASSERT_EQ(Outcome::Formed, shaped.outcome) << text.size() << " B";
    ASSERT_EQ(1u, shaped.pieces.size());
    EXPECT_FALSE(shaped.pieces[0].glyphs.empty());
  }
}

TEST(IndicShaper, AnOffsetPastAByteIsDegradedNotClamped) {
  FixtureShapingFace face(layoutFont("Bengali"));
  const ExpectedShaping& word = kBengaliShaping[5];
  ASSERT_EQ(-13, word.glyphs[2].dx);  // -156 px at 12 times the size: past an int8_t
  {
    RecordingSink sink;
    CallMemory memory(nullptr, std::strlen(word.utf8));
    EXPECT_EQ(Outcome::Degraded,
              IndicShaper::shapeToken(word.utf8, on(&face, word.language, 12 * kFixturePpem26_6), memory, sink));
  }
  // The cause: at 12 times the size every advance still fits its field, and an offset does not.
  const std::vector<StoredGlyph> large =
      shapedWithContext(face, ot::Script::Bengali, "", word.utf8, "", false, 12 * kFixturePpem26_6);
  ASSERT_EQ(word.count, large.size());
  bool offsetPast = false;
  for (const StoredGlyph& glyph : large) {
    EXPECT_GE(glyph.advance12_4, glyph_run::kMinAdvance);
    EXPECT_LE(glyph.advance12_4, glyph_run::kMaxAdvance);
    offsetPast = offsetPast || glyph.dx < INT8_MIN || glyph.dx > INT8_MAX || glyph.dy < INT8_MIN || glyph.dy > INT8_MAX;
  }
  EXPECT_TRUE(offsetPast);
  // The control: at 4 times the size every offset fits, and the word is formed as ot::shape gives it.
  const Shaped fits = shape(word.utf8, on(&face, word.language, 4 * kFixturePpem26_6));
  ASSERT_EQ(Outcome::Formed, fits.outcome);
  ASSERT_EQ(1u, fits.pieces.size());
  EXPECT_EQ(shapedWithContext(face, ot::Script::Bengali, "", word.utf8, "", false, 4 * kFixturePpem26_6),
            fits.pieces[0].glyphs);
}

TEST(IndicShaper, ASinkWithoutRoomIsDegraded) {
  FixtureShapingFace face(layoutFont("Devanagari"));
  RecordingSink sink;
  sink.room = 1;  // the piece begins; its first glyph finds no room
  CallMemory memory(nullptr, std::strlen(kKi));
  EXPECT_EQ(Outcome::Degraded, IndicShaper::shapeToken(kKi, on(&face), memory, sink));
}

TEST(IndicShaper, ACallWithoutMemoryIsDegraded) {
  BuildMemoryScope scope;
  FixtureShapingFace face(layoutFont("Devanagari"));
  BuildArena arena(4096);
  ASSERT_TRUE(arena.valid());
  ASSERT_NE(nullptr, arena.alloc(arena.capacity() - 64, 1));
  const size_t used = arena.used();
  RecordingSink sink;
  {
    AllocationRefusal refusal(4096);
    CallMemory memory(&arena, std::strlen(kKi));
    EXPECT_FALSE(memory.valid());
    EXPECT_EQ(Outcome::Degraded, IndicShaper::shapeToken(kKi, on(&face), memory, sink));
  }
  EXPECT_EQ(used, arena.used());
  EXPECT_FALSE(TextShaping::buildMemoryHeld());
  EXPECT_EQ(0, sink.calls);
}

TEST(IndicShaper, AFullArenaFallsBackToTheHeapBlockQuietly) {
  BuildMemoryScope scope;
  FixtureShapingFace face(layoutFont("Devanagari"));
  BuildArena arena(4096);
  ASSERT_TRUE(arena.valid());
  ASSERT_NE(nullptr, arena.alloc(arena.capacity() - 64, 1));
  const size_t used = arena.used();
  {
    RecordingSink sink;
    CallMemory memory(&arena, std::strlen(kKi));
    ASSERT_TRUE(memory.valid());
    EXPECT_TRUE(TextShaping::buildMemoryHeld());  // the build's heap block serves the call
    EXPECT_EQ(used, arena.used());
    EXPECT_EQ(Outcome::Formed, IndicShaper::shapeToken(kKi, on(&face), memory, sink));
  }
  EXPECT_EQ(0u, arena.failedAllocSize());  // the fallback leaves no refusal in the arena's own diagnostics
}

TEST(IndicShaper, ASecondCallWhileTheFirstLivesIsRefused) {
  BuildMemoryScope scope;
  FixtureShapingFace face(layoutFont("Devanagari"));
  {
    CallMemory first(nullptr, std::strlen(kKi));
    ASSERT_TRUE(first.valid());
    {
      CallMemory second(nullptr, std::strlen(kKi));  // the build's heap block is the first's
      EXPECT_FALSE(second.valid());
      RecordingSink sink;
      EXPECT_EQ(Outcome::Degraded, IndicShaper::shapeToken(kKi, on(&face), second, sink));
      EXPECT_EQ(0, sink.calls);
    }
    RecordingSink sink;
    EXPECT_EQ(Outcome::Formed, IndicShaper::shapeToken(kKi, on(&face), first, sink));
    ASSERT_EQ(1u, sink.pieces.size());
    EXPECT_EQ(2u, sink.pieces[0].glyphs.size());
  }
  CallMemory next(nullptr, std::strlen(kKi));  // given back when the first ended
  EXPECT_TRUE(next.valid());
}

TEST(IndicShaper, TheCallsBlockIsTheArenasForTheCallAlone) {
  BuildMemoryScope scope;
  FixtureShapingFace face(layoutFont("Devanagari"));
  BuildArena arena(16 * 1024);
  ASSERT_TRUE(arena.valid());
  ASSERT_NE(nullptr, arena.alloc(100, 1));  // the build's own, below the call's block
  const size_t start = arena.used();
  {
    RecordingSink sink;
    CallMemory memory(&arena, std::strlen(kKi));
    ASSERT_TRUE(memory.valid());
    EXPECT_GT(arena.used(), start);
    EXPECT_EQ(Outcome::Formed, IndicShaper::shapeToken(kKi, on(&face), memory, sink));
  }
  EXPECT_EQ(start, arena.used());
  EXPECT_FALSE(TextShaping::buildMemoryHeld());
}

TEST(IndicShaper, WithoutAnArenaTheBuildsHeapBlockServesEveryCall) {
  BuildMemoryScope scope;
  FixtureShapingFace face(layoutFont("Devanagari"));
  const Request request = on(&face);
  ASSERT_FALSE(TextShaping::buildMemoryHeld());
  {
    CountingSink sink;
    const size_t allocations = heapAllocations();
    CallMemory memory(nullptr, std::strlen(kKi));
    ASSERT_TRUE(memory.valid());
    EXPECT_TRUE(TextShaping::buildMemoryHeld());
    EXPECT_GT(heapAllocations(), allocations);  // the first call makes the block
    EXPECT_EQ(Outcome::Formed, IndicShaper::shapeToken(kKi, request, memory, sink));
  }
  EXPECT_TRUE(TextShaping::buildMemoryHeld());
  {
    CountingSink sink;
    const size_t allocations = heapAllocations();
    CallMemory memory(nullptr, std::strlen(kKi));
    ASSERT_TRUE(memory.valid());
    EXPECT_EQ(Outcome::Formed, IndicShaper::shapeToken(kKi, request, memory, sink));
    EXPECT_EQ(allocations, heapAllocations());  // a second of the same size makes nothing (its plan is the face's)
    EXPECT_EQ(1, sink.pieces);
    EXPECT_EQ(2, sink.glyphs);
  }
  const size_t held = heapLiveBytes();
  TextShaping::releaseBuildMemory();
  EXPECT_FALSE(TextShaping::buildMemoryHeld());
  EXPECT_LT(heapLiveBytes(), held);
}
