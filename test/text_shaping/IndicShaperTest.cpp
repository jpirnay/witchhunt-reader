// Ours (writing modes step 2, PR 3): IndicShaper::shapeToken over FixtureShapingFace faces. The HarfBuzz references are
// test/complex_shaper's ExpectedShaping.h and test/ttf_font's ExpectedGlyphRuns.h; the piecewise and coverage cases
// follow crosspoint-reader PR #3787's ComplexShaperTest.
#include <BuildArena.h>
#include <GlyphRun.h>
#include <OtShaper.h>
#include <gtest/gtest.h>

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

// What ot::shape gives for before + text + after with the neighbours as context (RunContext{before, after}), on
// `face` in the default language at the fixtures' size; empty when it fails.
std::vector<StoredGlyph> shapedWithContext(FixtureShapingFace& face, const ot::Script script, const std::string& before,
                                           const std::string& text, const std::string& after) {
  const ot::Plan* plan = face.plan(script, ot::languageTagsFor(""));
  const ot::Face* shaping = face.current();
  if (plan == nullptr || shaping == nullptr) return {};
  const auto contextBefore = static_cast<unsigned>(ottest::utf32(before.c_str()).size());
  const auto contextAfter = static_cast<unsigned>(ottest::utf32(after.c_str()).size());
  const std::vector<uint32_t> cps = ottest::utf32((before + text + after).c_str());
  ottest::BoundBuffer bound(256);
  ot::Scale scale;
  scale.set(kFixturePpem26_6, (kFixturePpem26_6 + 32) >> 6, shaping->upem());
  if (!ot::shape(*shaping, scale, *plan, cps.data(), static_cast<unsigned>(cps.size()), bound.buffer,
                 ot::RunContext{contextBefore, contextAfter})) {
    return {};
  }
  return storedOf(bound.buffer);
}

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

TEST(IndicShaper, AnOffsetPastAByteIsDegradedNotClamped) {
  FixtureShapingFace face(layoutFont("Bengali"));
  const ExpectedShaping& word = kBengaliShaping[5];
  ASSERT_EQ(-13, word.glyphs[2].dx);  // -156 px at 12 times the size: past an int8_t
  RecordingSink sink;
  CallMemory memory(nullptr, std::strlen(word.utf8));
  EXPECT_EQ(Outcome::Degraded,
            IndicShaper::shapeToken(word.utf8, on(&face, word.language, 12 * kFixturePpem26_6), memory, sink));
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
