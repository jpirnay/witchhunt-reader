// Ours (writing modes step 2, PR 3, and PR 4b's Arabic runs): IndicShaper::shapeToken over FixtureShapingFace faces.
// The HarfBuzz references are test/complex_shaper's ExpectedShaping.h, test/ttf_font's ExpectedGlyphRuns.h and
// test/ot_shaper's ArabicExpected.h; the piecewise and coverage cases follow crosspoint-reader PR #3787's
// ComplexShaperTest.
#include <BuildArena.h>
#include <GlyphRun.h>
#include <OtShaper.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "ArabicExpected.h"
#include "ArabicScripts.h"
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

using IndicShaper::CallKind;
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

// Arabic (PR 4b), named in the comments: the sources hold no Arabic character.
constexpr const char* kBeh = "\xD8\xA8";                            // U+0628 beh
constexpr const char* kTeh = "\xD8\xAA";                            // U+062A teh
constexpr const char* kAlef = "\xD8\xA7";                           // U+0627 alef
constexpr const char* kLam = "\xD9\x84";                            // U+0644 lam
constexpr const char* kKaf = "\xD9\x83";                            // U+0643 kaf
constexpr const char* kDad = "\xD8\xB6";                            // U+0636 dad
constexpr const char* kFatha = "\xD9\x8E";                          // U+064E fatha, a mark
constexpr const char* kKitab = "\xD9\x83\xD8\xAA\xD8\xA7\xD8\xA8";  // kaf teh alef beh: kitab, a book
constexpr const char* kArabicComma = "\xD8\x8C";                    // U+060C (CS): outside a run
constexpr const char* kArabicQuestionMark = "\xD8\x9F";             // U+061F (AL): inside a run
constexpr const char* kArabicIndicOneNine = "\xD9\xA1\xD9\xA9";     // U+0661 U+0669 (AN): outside a run
constexpr const char* kZwj = "\xE2\x80\x8D";                        // U+200D

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

// `text` shaped in a call of its own, in the build's heap block, laid out as ParsedText lays it out: a call without a
// face only reorders.
Shaped shape(std::string_view text, const Request& request,
             const unsigned runCodepoints = IndicShaper::kMaxRunCodepoints) {
  RecordingSink sink;
  CallMemory memory(nullptr, text.size(), request.face != nullptr ? CallKind::Shape : CallKind::Reorder, runCodepoints);
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
// on `face` in `language` (the default: none) at `ppem26_6`; empty when it fails.
std::vector<StoredGlyph> shapedWithContext(FixtureShapingFace& face, const ot::Script script, const std::string& before,
                                           const std::string& text, const std::string& after,
                                           const bool partial = false, const uint32_t ppem26_6 = kFixturePpem26_6,
                                           const char* language = "") {
  const ot::Plan* plan = face.plan(script, ot::languageTagsFor(language));
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

// The build's heap block released before and after a test that counts it.
struct BuildMemoryScope {
  BuildMemoryScope() { TextShaping::releaseBuildMemory(); }
  ~BuildMemoryScope() { TextShaping::releaseBuildMemory(); }
};

std::vector<uint8_t> arabicFont(const char* name) { return readFont(TTF_FIXTURE_DIR, name); }

std::string utf8Of(const std::vector<uint32_t>& cps) {
  std::string out;
  for (const uint32_t cp : cps) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | cp >> 6);
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xE0 | cp >> 12);
      out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

std::string codepointsOf(const std::string& text) {
  std::ostringstream s;
  s << std::hex << std::uppercase;
  const char* separator = "U+";
  for (const uint32_t cp : ottest::utf32(text.c_str())) {
    s << separator << cp;
    separator = " ";
  }
  return s.str();
}

std::string glyphText(const std::vector<StoredGlyph>& glyphs) {
  std::ostringstream s;
  for (const StoredGlyph& g : glyphs) s << g;
  return s.str();
}

std::vector<StoredGlyph> storedOf(const ArabicShapingExpectation& word) {
  std::vector<StoredGlyph> out;
  for (unsigned i = 0; i < word.count; ++i) {
    out.push_back({word.glyphs[i].gid, word.glyphs[i].advance12_4, word.glyphs[i].dx, word.glyphs[i].dy});
  }
  return out;
}

const ArabicShapingExpectation* findRecorded(const char* font, const std::string& utf8, const char* language) {
  for (const ArabicShapingExpectation& word : kArabicShaping) {
    if (std::strcmp(word.font, font) == 0 && utf8 == word.utf8 && std::strcmp(word.language, language) == 0) {
      return &word;
    }
  }
  ADD_FAILURE() << font << " '" << language << "' " << codepointsOf(utf8) << " is not recorded";
  return nullptr;
}

// The pieces' glyphs side by side in visual order, the last piece first: a right-to-left token's.
std::vector<StoredGlyph> rightToLeft(const std::vector<std::vector<StoredGlyph>>& tokens) {
  std::vector<StoredGlyph> out;
  for (auto token = tokens.rbegin(); token != tokens.rend(); ++token)
    out.insert(out.end(), token->begin(), token->end());
  return out;
}

// The code points of `text`'s longest `n` consecutive Arabic clusters (arabic::isClusterBreak's breaks). A piece of one
// cluster shapes with two clusters on either side as context in a call of at least five clusters' code points.
unsigned longestClusters(const std::string& text, const size_t n) {
  const std::vector<uint32_t> cps = ottest::utf32(text.c_str());
  std::vector<unsigned> clusters;
  for (size_t i = 0; i < cps.size(); ++i) {
    if (i == 0 || arabic::isClusterBreak(cps[i - 1], cps[i])) clusters.push_back(0);
    clusters.back()++;
  }
  unsigned longest = 0;
  for (size_t i = 0; i < clusters.size(); ++i) {
    unsigned sum = 0;
    for (size_t k = i; k < clusters.size() && k < i + n; ++k) sum += clusters[k];
    longest = std::max(longest, sum);
  }
  return longest;
}

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

TEST(IndicShaper, ACallWithoutAFaceTakesABlockForItsFormAlone) {
  // A bitmap or built-in font only reorders: its call's block is IndicReorder's form, 3 * len + 1 B, without the
  // Buffer, its storage, the runs and the code points a shaping call lays out.
  BuildMemoryScope scope;
  FixtureShapingFace face(layoutFont("Devanagari"));
  const size_t len = std::strlen(kKi);
  BuildArena arena(16 * 1024);
  ASSERT_TRUE(arena.valid());
  const size_t start = arena.used();
  size_t shaping = 0;
  {
    CallMemory memory(&arena, len);
    ASSERT_TRUE(memory.valid());
    shaping = arena.used() - start;
  }
  EXPECT_GT(shaping, ot::Buffer::bytesFor(2 * IndicShaper::kMaxRunCodepoints + 16));
  {
    CallMemory memory(&arena, len, CallKind::Reorder);
    ASSERT_TRUE(memory.valid());
    EXPECT_EQ(3 * len + 1, arena.used() - start);
    EXPECT_EQ(3 * len + 1, memory.reorderRoom());
    RecordingSink reordered;
    ASSERT_EQ(Outcome::Formed, IndicShaper::shapeToken(kKi, on(nullptr), memory, reordered));
    ASSERT_EQ(1u, reordered.pieces.size());
    EXPECT_EQ(PieceKind::Reordered, reordered.pieces[0].kind);
    EXPECT_EQ(std::string("\xE0\xA4\xBF\xE0\xA4\x95"), reordered.pieces[0].bytes);  // ि क
    // A face's call in a reordering block shapes nothing.
    RecordingSink shaped;
    EXPECT_EQ(Outcome::Degraded, IndicShaper::shapeToken(kKi, on(&face), memory, shaped));
    EXPECT_EQ(0, shaped.calls);
  }
  EXPECT_EQ(start, arena.used());

  // Without an arena, the build's heap block a reordering call makes is one 512 B step, not a shaping call's.
  {
    const size_t held = heapLiveBytes();
    CallMemory memory(nullptr, len, CallKind::Reorder);
    ASSERT_TRUE(memory.valid());
    EXPECT_LT(heapLiveBytes() - held, size_t{1024});
  }
  TextShaping::releaseBuildMemory();
  {
    const size_t held = heapLiveBytes();
    CallMemory memory(nullptr, len);
    ASSERT_TRUE(memory.valid());
    EXPECT_GE(heapLiveBytes() - held, shaping);
  }
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

// Arabic runs (writing modes step 2, PR 4b): shaped under ot::Script::Arabic over the Arabic fixtures (test/fixtures/
// fonts), in the book's language, with a glued neighbour's nearest cluster as context.

namespace {

// A code point of a Text piece as HarfBuzz gives it inside the run: the face's glyph for it, or a joiner hidden at no
// advance (hide_default_ignorables). Its advance is not compared: a Text piece is drawn as code points, not shaped.
bool drawnAsItself(const ot::Face& face, const uint32_t cp, const StoredGlyph& glyph) {
  if (cp == arabic::ZWJ || cp == arabic::ZWNJ) return glyph.advance12_4 == 0 && glyph.dx == 0 && glyph.dy == 0;
  uint32_t nominal = 0;
  return face.nominalGlyph(cp, &nominal) && nominal == glyph.gid;
}

}  // namespace

// Every word make_arabic_fixtures.py recorded from HarfBuzz 14.5.0 (test/ot_shaper/ArabicExpected.h), through
// shapeToken. HarfBuzz shapes each as one right-to-left run, so its glyphs are the pieces', the last piece first: a
// Glyphs piece is its records, glyph for glyph; a Text piece (a space, a digit, a joiner before a letter: outside
// every run) is one glyph a code point, the last first, as drawnAsItself says. A word that is one run is one Glyphs
// piece.
TEST(IndicShaper, MatchesHarfBuzzOnTheRecordedArabicWords) {
  std::map<std::string, std::unique_ptr<FixtureShapingFace>> faces;
  size_t words = 0;
  size_t matching = 0;
  size_t oneRun = 0;
  size_t onePiece = 0;
  for (const ArabicShapingExpectation& word : kArabicShaping) {
    ++words;
    std::unique_ptr<FixtureShapingFace>& face = faces[word.font];
    if (!face) face = std::make_unique<FixtureShapingFace>(arabicFont(word.font));
    const std::string text = word.utf8;
    const auto size = static_cast<uint16_t>(text.size());
    const Shaped shaped = shape(text, on(face.get(), word.language));
    ASSERT_NE(Outcome::Degraded, shaped.outcome) << word.font << " '" << word.language << "' " << codepointsOf(text);
    const ot::Face* shaping = face->current();
    ASSERT_NE(nullptr, shaping);
    std::vector<Piece> pieces = shaped.pieces;
    if (shaped.outcome == Outcome::Plain) pieces.push_back({PieceKind::Text, 0, size, {}, {}});  // nothing written
    const std::vector<StoredGlyph> recorded = storedOf(word);
    std::vector<StoredGlyph> drawn;  // in visual order; a Text code point takes the recorded glyph it is drawn as
    for (auto piece = pieces.rbegin(); piece != pieces.rend(); ++piece) {
      if (piece->kind == PieceKind::Glyphs) {
        drawn.insert(drawn.end(), piece->glyphs.begin(), piece->glyphs.end());
        continue;
      }
      const std::vector<uint32_t> cps = ottest::utf32(text.substr(piece->begin, piece->end - piece->begin).c_str());
      for (auto cp = cps.rbegin(); cp != cps.rend(); ++cp) {
        const size_t at = drawn.size();
        const bool itself =
            piece->kind == PieceKind::Text && at < recorded.size() && drawnAsItself(*shaping, *cp, recorded[at]);
        drawn.push_back(itself ? recorded[at] : StoredGlyph{UINT32_MAX, 0, 0, 0});  // a Reordered piece never is
      }
    }
    std::vector<IndicShaper::Run> runs(IndicShaper::maxRuns(text.size()));
    if (IndicShaper::scanRuns(text, runs.data()) == 1 && runs[0].arabic && runs[0].begin == 0 && runs[0].end == size) {
      ++oneRun;
      if (shaped.pieces.size() == 1 && shaped.pieces[0].kind == PieceKind::Glyphs && shaped.pieces[0].begin == 0 &&
          shaped.pieces[0].end == size) {
        ++onePiece;
      }
    }
    if (drawn == recorded) {
      ++matching;
    } else if (words - matching <= 10) {
      ADD_FAILURE() << word.font << " language '" << word.language << "' " << codepointsOf(text) << " in "
                    << pieces.size() << " pieces\n  shaped   " << glyphText(drawn) << "\n  recorded "
                    << glyphText(recorded);
    }
  }
  std::printf("[ ArabicShaping ] %zu of %zu recorded words shaped as HarfBuzz shapes them, %zu of them one run\n",
              matching, words, oneRun);
  EXPECT_EQ(words, matching);
  EXPECT_EQ(1808u, words);
  EXPECT_EQ(oneRun, onePiece);
  EXPECT_EQ(1774u, oneRun);
}

TEST(IndicShaper, CutsArabicPiecesAtRunEdges) {
  FixtureShapingFace face(arabicFont(kFontNotoSansArabicShapingBold));
  const std::vector<StoredGlyph> kitab = shapedWithContext(face, ot::Script::Arabic, "", kKitab, "");
  ASSERT_FALSE(kitab.empty());
  {
    // Brackets and the Arabic comma are outside the run: (kitab), is Text, Glyphs, Text.
    const std::string text = std::string("(") + kKitab + ")" + kArabicComma;
    const Shaped shaped = shape(text, on(&face));
    ASSERT_EQ(Outcome::Formed, shaped.outcome);
    ASSERT_EQ(3u, shaped.pieces.size());
    EXPECT_EQ(PieceKind::Text, shaped.pieces[0].kind);
    EXPECT_EQ(0, shaped.pieces[0].begin);
    EXPECT_EQ(1, shaped.pieces[0].end);
    EXPECT_EQ(PieceKind::Glyphs, shaped.pieces[1].kind);
    EXPECT_EQ(1, shaped.pieces[1].begin);
    EXPECT_EQ(9, shaped.pieces[1].end);
    EXPECT_EQ(kitab, shaped.pieces[1].glyphs);
    EXPECT_EQ(PieceKind::Text, shaped.pieces[2].kind);
    EXPECT_EQ(9, shaped.pieces[2].begin);
    EXPECT_EQ(12, shaped.pieces[2].end);
    EXPECT_TRUE(shaped.pieces[0].glyphs.empty() && shaped.pieces[0].bytes.empty());
    EXPECT_TRUE(shaped.pieces[2].glyphs.empty() && shaped.pieces[2].bytes.empty());
  }
  {
    // Arabic-Indic digits end it: bidi orders them (deviation 6).
    const std::string text = std::string(kKitab) + kArabicIndicOneNine;
    const Shaped shaped = shape(text, on(&face));
    ASSERT_EQ(Outcome::Formed, shaped.outcome);
    ASSERT_EQ(2u, shaped.pieces.size());
    EXPECT_EQ(PieceKind::Glyphs, shaped.pieces[0].kind);
    EXPECT_EQ(0, shaped.pieces[0].begin);
    EXPECT_EQ(8, shaped.pieces[0].end);
    EXPECT_EQ(kitab, shaped.pieces[0].glyphs);
    EXPECT_EQ(PieceKind::Text, shaped.pieces[1].kind);
    EXPECT_EQ(8, shaped.pieces[1].begin);
    EXPECT_EQ(12, shaped.pieces[1].end);
  }
  {
    // AL punctuation is in it: kitab and the Arabic question mark are one Glyphs piece, shaped together.
    const std::string text = std::string(kKitab) + kArabicQuestionMark;
    const Shaped shaped = shape(text, on(&face));
    ASSERT_EQ(Outcome::Formed, shaped.outcome);
    ASSERT_EQ(1u, shaped.pieces.size());
    EXPECT_EQ(PieceKind::Glyphs, shaped.pieces[0].kind);
    EXPECT_EQ(0, shaped.pieces[0].begin);
    EXPECT_EQ(10, shaped.pieces[0].end);
    EXPECT_EQ(shapedWithContext(face, ot::Script::Arabic, "", text, ""), shaped.pieces[0].glyphs);
  }
}

TEST(IndicShaper, AFontWithoutArabicLeavesItText) {
  FixtureShapingFace face(layoutFont("Devanagari"));
  ASSERT_NE(nullptr, face.current());
  uint32_t glyph = 0;
  ASSERT_FALSE(face.current()->nominalGlyph(arabic::kProbe, &glyph));
  {
    // Unjoined, as step 1 draws it: nothing written.
    RecordingSink sink;
    CallMemory memory(nullptr, std::strlen(kKitab));
    EXPECT_EQ(Outcome::Plain, IndicShaper::shapeToken(kKitab, on(&face), memory, sink));
    EXPECT_EQ(0, sink.calls);
  }
  // Beside an Indic run the face shapes, the Arabic run is a Text piece: never Reordered.
  const std::vector<StoredGlyph> ki = shapedWithContext(face, ot::Script::Devanagari, "", kKi, "");
  {
    const Shaped shaped = shape(std::string(kKi) + kKitab, on(&face));
    ASSERT_EQ(Outcome::Formed, shaped.outcome);
    ASSERT_EQ(2u, shaped.pieces.size());
    EXPECT_EQ(PieceKind::Glyphs, shaped.pieces[0].kind);
    EXPECT_EQ(ki, shaped.pieces[0].glyphs);
    EXPECT_EQ(PieceKind::Text, shaped.pieces[1].kind);
    EXPECT_EQ(6, shaped.pieces[1].begin);
    EXPECT_EQ(14, shaped.pieces[1].end);
    EXPECT_TRUE(shaped.pieces[1].bytes.empty());
  }
  {
    const Shaped shaped = shape(std::string(kKitab) + kKi, on(&face));
    ASSERT_EQ(Outcome::Formed, shaped.outcome);
    ASSERT_EQ(2u, shaped.pieces.size());
    EXPECT_EQ(PieceKind::Text, shaped.pieces[0].kind);
    EXPECT_EQ(0, shaped.pieces[0].begin);
    EXPECT_EQ(8, shaped.pieces[0].end);
    EXPECT_EQ(PieceKind::Glyphs, shaped.pieces[1].kind);
    EXPECT_EQ(ki, shaped.pieces[1].glyphs);
  }
}

TEST(IndicShaper, NoFaceLeavesArabicPlain) {
  // A bitmap or built-in font draws an Arabic word unjoined, as step 1 does (decision 5): nothing written.
  for (const std::string& text :
       {std::string(kKitab), std::string(kKitab) + kArabicIndicOneNine, std::string("(") + kKitab + ")"}) {
    RecordingSink sink;
    CallMemory memory(nullptr, text.size(), CallKind::Reorder);
    EXPECT_EQ(Outcome::Plain, IndicShaper::shapeToken(text, on(nullptr), memory, sink)) << codepointsOf(text);
    EXPECT_EQ(0, sink.calls) << codepointsOf(text);
  }
  // Beside Indic text, IndicReorder's form covers the Indic text alone: the Arabic run is Text, never reordered.
  {
    const Shaped shaped = shape(std::string(kKi) + kKitab, on(nullptr));
    ASSERT_EQ(Outcome::Formed, shaped.outcome);
    ASSERT_EQ(2u, shaped.pieces.size());
    EXPECT_EQ(PieceKind::Reordered, shaped.pieces[0].kind);
    EXPECT_EQ(0, shaped.pieces[0].begin);
    EXPECT_EQ(6, shaped.pieces[0].end);
    EXPECT_EQ(kIKa, shaped.pieces[0].bytes);
    EXPECT_EQ(PieceKind::Text, shaped.pieces[1].kind);
    EXPECT_EQ(6, shaped.pieces[1].begin);
    EXPECT_EQ(14, shaped.pieces[1].end);
  }
  {
    const Shaped shaped = shape(std::string(kKitab) + kKi + ",", on(nullptr));
    ASSERT_EQ(Outcome::Formed, shaped.outcome);
    ASSERT_EQ(2u, shaped.pieces.size());
    EXPECT_EQ(PieceKind::Text, shaped.pieces[0].kind);
    EXPECT_EQ(0, shaped.pieces[0].begin);
    EXPECT_EQ(8, shaped.pieces[0].end);
    EXPECT_EQ(PieceKind::Reordered, shaped.pieces[1].kind);
    EXPECT_EQ(8, shaped.pieces[1].begin);
    EXPECT_EQ(15, shaped.pieces[1].end);
    EXPECT_EQ(std::string(kIKa) + ",", shaped.pieces[1].bytes);
  }
}

TEST(IndicShaper, ShapesArabicInTheBooksLanguage) {
  // The synthetic font's locl gives lam another glyph in Urdu; Persian takes the default language system, as
  // HarfBuzz recorded.
  FixtureShapingFace face(arabicFont(kFontArabicRtlTestRegular));
  const std::string lamBeh = std::string(kLam) + kBeh;
  const Shaped persian = shape(lamBeh, on(&face, "fa"));
  const Shaped urdu = shape(lamBeh, on(&face, "ur"));
  ASSERT_EQ(Outcome::Formed, persian.outcome);
  ASSERT_EQ(Outcome::Formed, urdu.outcome);
  ASSERT_EQ(1u, persian.pieces.size());
  ASSERT_EQ(1u, urdu.pieces.size());
  EXPECT_NE(persian.pieces[0].glyphs, urdu.pieces[0].glyphs);
  const ArabicShapingExpectation* byDefault = findRecorded(kFontArabicRtlTestRegular, lamBeh, "");
  const ArabicShapingExpectation* inUrdu = findRecorded(kFontArabicRtlTestRegular, lamBeh, "ur");
  ASSERT_TRUE(byDefault != nullptr && inUrdu != nullptr);
  EXPECT_EQ(storedOf(*byDefault), persian.pieces[0].glyphs);
  EXPECT_EQ(storedOf(*inUrdu), urdu.pieces[0].glyphs);
}

TEST(IndicShaper, ARunLongerThanTheCallShapesInClusters) {
  // Two runs of 33 code points, past kMaxRunCodepoints, with lam-alefs, harakat and a shadda for the splits to fall
  // beside: as-salam, the Persian and Urdu letters, kitab, as-salam and kitab, unvocalised (a code point a cluster);
  // and as-salam, al-'arabiyyatu vocalised, then the Persian and Urdu letters.
  const std::vector<uint32_t> salam = {0x627, 0x644, 0x633, 0x644, 0x627, 0x645};
  const std::vector<uint32_t> letters = {0x67E, 0x686, 0x698, 0x6AF, 0x6A9, 0x6CC, 0x6C1,
                                         0x6D2, 0x6BA, 0x679, 0x688, 0x691, 0x6BE};
  const std::vector<uint32_t> kitab = {0x643, 0x62A, 0x627, 0x628};
  const std::vector<uint32_t> vocalised = {0x627, 0x644, 0x652, 0x639, 0x64E, 0x631, 0x64E,
                                           0x628, 0x650, 0x64A, 0x651, 0x64E, 0x629, 0x64F};
  const auto joined = [](const std::vector<std::vector<uint32_t>>& parts) {
    std::vector<uint32_t> out;
    for (const std::vector<uint32_t>& part : parts) out.insert(out.end(), part.begin(), part.end());
    return utf8Of(out);
  };
  const std::string plain = joined({salam, letters, kitab, salam, kitab});
  const std::string voweled = joined({salam, vocalised, letters});
  ASSERT_LE(longestClusters(plain, 5), 8u);  // a call of 8 code points holds five of its clusters
  for (const char* font : {kFontNotoNaskhArabicShapingRegular, kFontNotoSansArabicShapingBold}) {
    for (const std::string& word : {plain, voweled}) {
      // Alone, and glued between a beh and a lam kept apart: their clusters are context across the run's edges.
      for (const auto& [before, after] : {std::pair<std::string, std::string>{"", ""}, {kBeh, kLam}}) {
        SCOPED_TRACE(std::string(font) + " " + codepointsOf(before) + " | " + codepointsOf(word) + " | " +
                     codepointsOf(after));
        const auto length = static_cast<unsigned>(ottest::utf32(word.c_str()).size());
        ASSERT_EQ(33u, length);
        ASSERT_GT(length, IndicShaper::kMaxRunCodepoints);
        FixtureShapingFace face(arabicFont(font));
        Request request = on(&face);
        request.before = before;
        request.after = after;
        const Shaped whole = shape(word, request, 64);
        ASSERT_EQ(Outcome::Formed, whole.outcome);
        ASSERT_EQ(1u, whole.pieces.size());
        EXPECT_EQ(shapedWithContext(face, ot::Script::Arabic, before, word, after), whole.pieces[0].glyphs);
        // A piece is shaped with two clusters on either side as context: a letter's glyph can follow its neighbour's
        // joining form, which follows the neighbour's own neighbour (Noto's alef after a medial or an initial lam,
        // Noto Sans's kern of reh before an initial or an isolated beh). Every call that holds five clusters, the
        // default call among them, gives the whole run's records, the pieces written last first.
        for (unsigned n = longestClusters(word, 5); n < length; ++n) {
          const Shaped at = shape(word, request, n);
          ASSERT_EQ(Outcome::Formed, at.outcome) << n;
          ASSERT_EQ(1u, at.pieces.size()) << n;
          EXPECT_EQ(PieceKind::Glyphs, at.pieces[0].kind) << n;
          EXPECT_EQ(whole.pieces[0].glyphs, at.pieces[0].glyphs)
              << "a call of " << n << " code points\n  whole  " << glyphText(whole.pieces[0].glyphs) << "\n  pieces "
              << glyphText(at.pieces[0].glyphs);
        }
      }
    }
  }
}

TEST(IndicShaper, AGluedNeighbourIsContext) {
  // kaf <b>teh</b> alef beh: three tokens kept apart. Each is shaped with its neighbours' nearest clusters as context,
  // so the bold teh keeps its medial form and its neighbours their joined ones: side by side, right to left, they are
  // the whole word's glyphs.
  const std::string alefBeh = std::string(kAlef) + kBeh;
  for (const char* font : {kFontNotoNaskhArabicShapingRegular, kFontNotoSansArabicShapingBold}) {
    SCOPED_TRACE(font);
    FixtureShapingFace face(arabicFont(font));
    Request first = on(&face);
    first.after = std::string(kTeh) + alefBeh;
    Request middle = on(&face);
    middle.before = kKaf;
    middle.after = alefBeh;
    Request last = on(&face);
    last.before = std::string(kKaf) + kTeh;
    const Shaped kaf = shape(kKaf, first);
    const Shaped teh = shape(kTeh, middle);
    const Shaped rest = shape(alefBeh, last);
    const Shaped alone = shape(kTeh, on(&face));
    for (const Shaped* token : {&kaf, &teh, &rest, &alone}) {
      ASSERT_EQ(Outcome::Formed, token->outcome);
      ASSERT_EQ(1u, token->pieces.size());
      ASSERT_EQ(PieceKind::Glyphs, token->pieces[0].kind);
    }
    EXPECT_NE(alone.pieces[0].glyphs, teh.pieces[0].glyphs);
    EXPECT_EQ(shapedWithContext(face, ot::Script::Arabic, kKaf, kTeh, alefBeh), teh.pieces[0].glyphs);
    EXPECT_EQ(shapedWithContext(face, ot::Script::Arabic, "", kKitab, ""),
              rightToLeft({kaf.pieces[0].glyphs, teh.pieces[0].glyphs, rest.pieces[0].glyphs}));
  }
}

TEST(IndicShaper, TwoClustersOfContextFormTheWholeWordsGlyphs) {
  // A glued neighbour kept apart is context by its two nearest letters, each with its marks and joiners: a letter's
  // glyph can follow its neighbour's joining form, which follows that neighbour's own neighbour. Two words, each cut
  // into three tokens kept apart; side by side, right to left, the tokens are the whole word's glyphs.
  const std::string seenLam = utf8Of({0x633, 0x644});
  const std::string meem = utf8Of({0x645});
  const std::string head = utf8Of({0x627, 0x644, 0x652, 0x639, 0x64E, 0x631, 0x64E});  // al-'ara
  const std::string behKasra = utf8Of({0x628, 0x650});
  const std::string yehShaddaFatha = utf8Of({0x64A, 0x651, 0x64E});
  const std::string tail = yehShaddaFatha + utf8Of({0x629, 0x64F});  // -iyyatu
  for (const char* font : {kFontNotoNaskhArabicShapingRegular, kFontNotoSansArabicShapingBold}) {
    SCOPED_TRACE(font);
    FixtureShapingFace face(arabicFont(font));
    const auto glued = [&](const std::string& text, const std::string& before, const std::string& after) {
      Request request = on(&face);
      request.before = before;
      request.after = after;
      const Shaped shaped = shape(text, request);
      EXPECT_EQ(Outcome::Formed, shaped.outcome) << codepointsOf(text);
      EXPECT_EQ(1u, shaped.pieces.size()) << codepointsOf(text);
      return shaped.pieces.empty() ? std::vector<StoredGlyph>{} : shaped.pieces[0].glyphs;
    };
    {
      // seen lam <b>alef</b> meem: Noto's alef takes another glyph after a medial lam than after an initial one, and
      // the lam is medial for the seen before it.
      const std::vector<StoredGlyph> first = glued(seenLam, "", std::string(kAlef) + meem);
      const std::vector<StoredGlyph> alef = glued(kAlef, seenLam, meem);
      const std::vector<StoredGlyph> last = glued(meem, seenLam + kAlef, "");
      EXPECT_EQ(shapedWithContext(face, ot::Script::Arabic, seenLam, kAlef, meem), alef);
      EXPECT_NE(shapedWithContext(face, ot::Script::Arabic, kLam, kAlef, meem), alef);  // one cluster: an initial lam
      EXPECT_EQ(shapedWithContext(face, ot::Script::Arabic, "", seenLam + kAlef + meem, ""),
                rightToLeft({first, alef, last}));
    }
    {
      // al-'ara <b>beh kasra</b> -iyyatu: Noto Sans Bold kerns the reh against an initial beh, which is initial for
      // the yeh after it.
      const std::vector<StoredGlyph> first = glued(head, "", behKasra + tail);
      const std::vector<StoredGlyph> beh = glued(behKasra, head, tail);
      const std::vector<StoredGlyph> last = glued(tail, head + behKasra, "");
      EXPECT_EQ(shapedWithContext(face, ot::Script::Arabic, "", head, behKasra + yehShaddaFatha), first);
      if (std::strcmp(font, kFontNotoSansArabicShapingBold) == 0) {
        EXPECT_NE(shapedWithContext(face, ot::Script::Arabic, "", head, behKasra), first);  // one cluster: no kern
      }
      EXPECT_EQ(shapedWithContext(face, ot::Script::Arabic, "", head + behKasra + tail, ""),
                rightToLeft({first, beh, last}));
    }
  }
}

TEST(IndicShaper, TheArabicNeighboursContextIsItsTwoNearestClusters) {
  FixtureShapingFace face(arabicFont(kFontNotoSansArabicShapingBold));
  // Tokens whose glyphs follow the farther cluster: alef after lam, whose form the letter before the lam decides; and
  // ain-fatha reh-fatha before beh-kasra, whose reh is kerned as the beh is initial (a yeh after it) or isolated.
  const std::string seenLam = utf8Of({0x633, 0x644});
  const std::string ainReh = utf8Of({0x639, 0x64E, 0x631, 0x64E});
  const std::string behKasra = utf8Of({0x628, 0x650});
  const std::string behKasraYeh = behKasra + utf8Of({0x64A, 0x651, 0x64E});
  const auto alefAfter = [&](const std::string& context) {
    return shapedWithContext(face, ot::Script::Arabic, context, kAlef, "");
  };
  const auto ainRehBefore = [&](const std::string& context) {
    return shapedWithContext(face, ot::Script::Arabic, "", ainReh, context);
  };
  ASSERT_NE(alefAfter(seenLam), alefAfter(kLam));
  ASSERT_NE(ainRehBefore(behKasraYeh), ainRehBefore(behKasra));
  // Before: the neighbour's last two letters, each with the marks and joiners after it, whatever comes before them;
  // the farther one only when it is Arabic.
  const std::pair<std::string, std::string> befores[] = {{seenLam, seenLam},
                                                         {std::string(kBeh) + seenLam, seenLam},
                                                         {std::string("(") + kLam, kLam},
                                                         {std::string("a") + kLam, kLam}};
  for (const auto& [before, context] : befores) {
    Request request = on(&face);
    request.before = before;
    const Shaped glued = shape(kAlef, request);
    ASSERT_EQ(Outcome::Formed, glued.outcome) << codepointsOf(before);
    ASSERT_EQ(1u, glued.pieces.size());
    EXPECT_EQ(alefAfter(context), glued.pieces[0].glyphs) << codepointsOf(before);
  }
  // After: its first two, likewise.
  const std::pair<std::string, std::string> afters[] = {
      {behKasraYeh, behKasraYeh}, {behKasraYeh + utf8Of({0x629, 0x64F}), behKasraYeh}, {behKasra + ")", behKasra}};
  for (const auto& [after, context] : afters) {
    Request request = on(&face);
    request.after = after;
    const Shaped glued = shape(ainReh, request);
    ASSERT_EQ(Outcome::Formed, glued.outcome) << codepointsOf(after);
    ASSERT_EQ(1u, glued.pieces.size());
    EXPECT_EQ(ainRehBefore(context), glued.pieces[0].glyphs) << codepointsOf(after);
  }
  // Two clusters of eight code points fill kArabicContextCodepoints, the vocalised seen and lam here. One code point
  // more cuts the farther cluster's letter off, and its marks alone are no context: the nearer cluster is.
  const std::vector<uint32_t> marks = {0x64E, 0x64F, 0x650, 0x651, 0x652, 0x653, 0x654, 0x655};
  std::vector<uint32_t> seenMarks = {0x633};
  seenMarks.insert(seenMarks.end(), marks.begin(), marks.end() - 1);
  std::vector<uint32_t> lamMarks = {0x644};
  lamMarks.insert(lamMarks.end(), marks.begin(), marks.end() - 1);
  ASSERT_EQ(IndicShaper::kArabicContextCodepoints, seenMarks.size() + lamMarks.size());
  std::vector<uint32_t> seenMoreMarks = seenMarks;
  seenMoreMarks.push_back(0x655);
  const std::string lam = utf8Of(lamMarks);
  ASSERT_NE(alefAfter(utf8Of(seenMarks) + lam), alefAfter(lam));
  for (const auto& [farther, context] :
       {std::pair<std::string, std::string>{utf8Of(seenMarks), utf8Of(seenMarks) + lam},
        {utf8Of(seenMoreMarks), lam}}) {
    Request request = on(&face);
    request.before = farther + lam;
    const Shaped glued = shape(kAlef, request);
    ASSERT_EQ(Outcome::Formed, glued.outcome) << codepointsOf(farther + lam);
    ASSERT_EQ(1u, glued.pieces.size());
    EXPECT_EQ(alefAfter(context), glued.pieces[0].glyphs) << codepointsOf(farther + lam);
  }
  // A neighbour whose nearest code point is no Arabic letter, mark or joiner (a bracket, a Latin or an Indic letter)
  // is no context.
  const Shaped alone = shape(kTeh, on(&face));
  ASSERT_EQ(Outcome::Formed, alone.outcome);
  for (const std::string& neighbour : {std::string("("), std::string("a"), std::string(kKa), std::string(kKaf) + ")"}) {
    Request request = on(&face);
    request.before = neighbour;
    const Shaped glued = shape(kTeh, request);
    ASSERT_EQ(Outcome::Formed, glued.outcome) << codepointsOf(neighbour);
    ASSERT_EQ(1u, glued.pieces.size()) << codepointsOf(neighbour);
    EXPECT_EQ(alone.pieces[0].glyphs, glued.pieces[0].glyphs) << codepointsOf(neighbour);
  }
}

TEST(IndicShaper, AJoinerBeforeAnArabicRunIsItsContext) {
  // ZWJ beh: a run starts at a letter, so the joiner is Text, yet it joins the beh on its right side (the final form).
  // It is the run's context, never output.
  FixtureShapingFace face(arabicFont(kFontNotoNaskhArabicShapingRegular));
  const std::string text = std::string(kZwj) + kBeh;
  const Shaped alone = shape(kBeh, on(&face));
  const Shaped shaped = shape(text, on(&face));
  ASSERT_EQ(Outcome::Formed, alone.outcome);
  ASSERT_EQ(Outcome::Formed, shaped.outcome);
  ASSERT_EQ(2u, shaped.pieces.size());
  EXPECT_EQ(PieceKind::Text, shaped.pieces[0].kind);
  EXPECT_EQ(0, shaped.pieces[0].begin);
  EXPECT_EQ(3, shaped.pieces[0].end);
  EXPECT_EQ(PieceKind::Glyphs, shaped.pieces[1].kind);
  EXPECT_EQ(3, shaped.pieces[1].begin);
  EXPECT_EQ(5, shaped.pieces[1].end);
  EXPECT_EQ(shapedWithContext(face, ot::Script::Arabic, kZwj, kBeh, ""), shaped.pieces[1].glyphs);
  EXPECT_NE(alone.pieces[0].glyphs, shaped.pieces[1].glyphs);
  // Glued after a token kept apart, that token's nearest cluster comes before the joiner.
  Request request = on(&face);
  request.before = kLam;
  const Shaped glued = shape(text, request);
  ASSERT_EQ(Outcome::Formed, glued.outcome);
  ASSERT_EQ(2u, glued.pieces.size());
  EXPECT_EQ(shapedWithContext(face, ot::Script::Arabic, std::string(kLam) + kZwj, kBeh, ""), glued.pieces[1].glyphs);
}

TEST(IndicShaper, ALamAlefAcrossTheGlueIsDrawnOnce) {
  // lam, then <span>alef</span>: each shaped with the other as context. In the synthetic font rlig makes the two one
  // ligature, which carries the lam's mask: the first token keeps it, in the first token's style, and the second
  // keeps no glyph. That is an empty Glyphs piece, of no width, and the token is Formed: not Degraded, which the
  // paragraph would latch and lay out again every session. The Noto subsets draw lam-alef as two glyphs, one a token.
  // Side by side the tokens are the pair's recorded glyphs.
  const std::string lamAlef = std::string(kLam) + kAlef;
  for (const char* font :
       {kFontArabicRtlTestRegular, kFontNotoNaskhArabicShapingRegular, kFontNotoSansArabicShapingBold}) {
    SCOPED_TRACE(font);
    FixtureShapingFace face(arabicFont(font));
    Request first = on(&face);
    first.after = kAlef;
    Request second = on(&face);
    second.before = kLam;
    const Shaped lam = shape(kLam, first);
    const Shaped alef = shape(kAlef, second);
    for (const Shaped* token : {&lam, &alef}) {
      ASSERT_EQ(Outcome::Formed, token->outcome);
      ASSERT_EQ(1u, token->pieces.size());
      EXPECT_EQ(PieceKind::Glyphs, token->pieces[0].kind);
      EXPECT_EQ(0, token->pieces[0].begin);
      EXPECT_EQ(2, token->pieces[0].end);
    }
    const ArabicShapingExpectation* recorded = findRecorded(font, lamAlef, "");
    ASSERT_NE(nullptr, recorded);
    EXPECT_EQ(storedOf(*recorded), rightToLeft({lam.pieces[0].glyphs, alef.pieces[0].glyphs}));
    if (std::strcmp(font, kFontArabicRtlTestRegular) == 0) {
      EXPECT_EQ(1u, lam.pieces[0].glyphs.size());
      EXPECT_TRUE(alef.pieces[0].glyphs.empty());
    }
  }
}

TEST(IndicShaper, AnArabicRunTheFontEmptiesIsDegradedWithoutContext) {
  // A ccmp of the arab script that deletes beh. Alone, a beh keeps no glyph and has no context to have taken it: no
  // form, Degraded, as an Indic run the font empties. Glued after another beh kept apart, it is shaped with context,
  // so the empty run is an empty Glyphs piece.
  const std::vector<uint8_t> font = arabicFont(kFontArabicRtlTestRegular);
  FixtureShapingFace plain(font);
  uint32_t beh = 0;
  ASSERT_NE(nullptr, plain.current());
  ASSERT_TRUE(plain.current()->nominalGlyph(arabic::kProbe, &beh));
  GsubFace deleting(font, deletingGsub(static_cast<uint16_t>(beh), "arab"));
  EXPECT_EQ(Outcome::Degraded, shape(kBeh, on(&deleting)).outcome);
  Request glued = on(&deleting);
  glued.before = kBeh;
  const Shaped consumed = shape(kBeh, glued);
  ASSERT_EQ(Outcome::Formed, consumed.outcome);
  ASSERT_EQ(1u, consumed.pieces.size());
  EXPECT_EQ(PieceKind::Glyphs, consumed.pieces[0].kind);
  EXPECT_TRUE(consumed.pieces[0].glyphs.empty());
  // The control: the lookup deletes another glyph, and the beh is formed.
  GsubFace other(font, deletingGsub(static_cast<uint16_t>(beh + 1), "arab"));
  const Shaped shaped = shape(kBeh, on(&other));
  ASSERT_EQ(Outcome::Formed, shaped.outcome);
  ASSERT_EQ(1u, shaped.pieces.size());
  EXPECT_FALSE(shaped.pieces[0].glyphs.empty());
}

TEST(IndicShaper, AnOffsetPastAByteIsDegraded) {
  // Noto Sans Arabic Bold's ccmp splits the dot off dad and places it 35 px from its base at the fixture's size: at
  // four times the size, past an int8_t.
  FixtureShapingFace face(arabicFont(kFontNotoSansArabicShapingBold));
  const ArabicShapingExpectation* word = findRecorded(kFontNotoSansArabicShapingBold, kDad, "");
  ASSERT_NE(nullptr, word);
  {
    RecordingSink sink;
    CallMemory memory(nullptr, std::strlen(kDad));
    EXPECT_EQ(Outcome::Degraded, IndicShaper::shapeToken(kDad, on(&face, "", 4 * kFixturePpem26_6), memory, sink));
  }
  // The cause: at four times the size every advance still fits its field, and an offset does not.
  const std::vector<StoredGlyph> large =
      shapedWithContext(face, ot::Script::Arabic, "", kDad, "", false, 4 * kFixturePpem26_6);
  ASSERT_EQ(word->count, large.size());
  bool offsetPast = false;
  for (const StoredGlyph& glyph : large) {
    EXPECT_GE(glyph.advance12_4, glyph_run::kMinAdvance);
    EXPECT_LE(glyph.advance12_4, glyph_run::kMaxAdvance);
    offsetPast = offsetPast || glyph.dx < INT8_MIN || glyph.dx > INT8_MAX || glyph.dy < INT8_MIN || glyph.dy > INT8_MAX;
  }
  EXPECT_TRUE(offsetPast);
  // The control: at twice the size every offset fits, and the word is formed as ot::shape gives it.
  const Shaped fits = shape(kDad, on(&face, "", 2 * kFixturePpem26_6));
  ASSERT_EQ(Outcome::Formed, fits.outcome);
  ASSERT_EQ(1u, fits.pieces.size());
  EXPECT_EQ(shapedWithContext(face, ot::Script::Arabic, "", kDad, "", false, 2 * kFixturePpem26_6),
            fits.pieces[0].glyphs);
}

TEST(IndicShaper, ATokenWithBothScriptsShapesEachRun) {
  // kitab then KI in one token: each run is shaped, or not, under its own script, in one call.
  const std::string text = std::string(kKitab) + kKi;
  {
    // Over Noto Naskh Arabic, which maps beh but not KA: the Arabic run is Glyphs, the Devanagari one Reordered.
    FixtureShapingFace face(arabicFont(kFontNotoNaskhArabicShapingRegular));
    uint32_t glyph = 0;
    ASSERT_NE(nullptr, face.current());
    ASSERT_FALSE(face.current()->nominalGlyph(0x0915, &glyph));
    const Shaped shaped = shape(text, on(&face));
    ASSERT_EQ(Outcome::Formed, shaped.outcome);
    ASSERT_EQ(2u, shaped.pieces.size());
    EXPECT_EQ(PieceKind::Glyphs, shaped.pieces[0].kind);
    EXPECT_EQ(0, shaped.pieces[0].begin);
    EXPECT_EQ(8, shaped.pieces[0].end);
    EXPECT_EQ(shapedWithContext(face, ot::Script::Arabic, "", kKitab, ""), shaped.pieces[0].glyphs);
    EXPECT_EQ(PieceKind::Reordered, shaped.pieces[1].kind);
    EXPECT_EQ(8, shaped.pieces[1].begin);
    EXPECT_EQ(14, shaped.pieces[1].end);
    EXPECT_EQ(kIKa, shaped.pieces[1].bytes);
  }
  {
    // Over Noto Sans Devanagari, which maps KA but not beh: the Arabic run is Text, the Devanagari one Glyphs.
    FixtureShapingFace face(layoutFont("Devanagari"));
    const Shaped shaped = shape(text, on(&face));
    ASSERT_EQ(Outcome::Formed, shaped.outcome);
    ASSERT_EQ(2u, shaped.pieces.size());
    EXPECT_EQ(PieceKind::Text, shaped.pieces[0].kind);
    EXPECT_EQ(PieceKind::Glyphs, shaped.pieces[1].kind);
    EXPECT_EQ(shapedWithContext(face, ot::Script::Devanagari, "", kKi, ""), shaped.pieces[1].glyphs);
  }
}
