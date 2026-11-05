#include <ExpectedShaping.h>
#include <OtShaper.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "OtShaperTestHelpers.h"

// The engine (lib/OtShaper) against the HarfBuzz output stored in
// test/complex_shaper/ExpectedShaping.h, converted the way ComplexShaper's
// appendGlyphs stored it; and the shaping buffer in its owner's storage
// (deviation 1).

static_assert(ot::MAX_NESTING_LEVEL == 8);
static_assert(ot::MAX_CONTEXT_LENGTH == 64);
static_assert(ot::MAX_PLANNED_LOOKUPS == 4096);
static_assert(ot::Buffer::SLOT_BYTES == 62);

namespace {

using ottest::glyphsOf;
using ottest::readFixture;
using ottest::scriptOf;
using ottest::shapeRun;
using ottest::utf32;

struct StoredGlyph {
  uint32_t gid;
  int advance12_4;
  int dx;
  int dy;
  bool operator==(const StoredGlyph&) const = default;
};

std::ostream& operator<<(std::ostream& os, const StoredGlyph& g) {
  return os << "{" << g.gid << "," << g.advance12_4 << "," << g.dx << "," << g.dy << "}";
}

int roundPixels(const int v) { return v >= 0 ? (v + 32) / 64 : -((-v + 32) / 64); }

// A Buffer over storage of its own, `slots` slots.
struct BoundBuffer {
  explicit BoundBuffer(const unsigned slots) : storage(ot::Buffer::bytesFor(slots)) {
    buffer.setStorage(storage.data(), storage.size());
  }
  std::vector<uint8_t> storage;
  ot::Buffer buffer;
};

// Shapes `utf8` and converts each glyph to what ExpectedShaping.h stores.
// Empty when ot::shape fails.
std::vector<StoredGlyph> shapeAsStored(const ot::Face& face, const ot::Plan& plan, const char* utf8) {
  BoundBuffer bound(256);
  ot::Buffer& buffer = bound.buffer;
  const std::vector<uint32_t> cps = utf32(utf8);
  std::vector<StoredGlyph> out;
  if (!shapeRun(face, plan, cps.data(), static_cast<unsigned>(cps.size()), buffer)) return out;
  for (unsigned i = 0; i < buffer.len(); i++) {
    out.push_back({buffer.info[i].codepoint, (buffer.pos[i].xAdvance + 2) >> 2, roundPixels(buffer.pos[i].xOffset),
                   -roundPixels(buffer.pos[i].yOffset)});
  }
  return out;
}

// Every fixture word, built plan against the stored output. `withFilters`
// keeps CPac (the stored-filters route); CPpl is always dropped.
void checkAllWords(const bool withFilters) {
  size_t words = 0;
  for (const ShapingFixture& fx : kShapingFixtures) {
    ot::Script script = ot::Script::Devanagari;
    ASSERT_TRUE(scriptOf(fx.script, &script)) << fx.script;
    const std::vector<uint8_t> font = readFixture(fx.layoutFile);
    ASSERT_FALSE(font.empty()) << fx.layoutFile;
    ot::FaceTables tables;
    ASSERT_TRUE(ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables));
    if (!withFilters) tables[ot::FaceTables::CPAC] = ot::Table();
    tables[ot::FaceTables::CPPL] = ot::Table();
    ot::Face face;
    ASSERT_TRUE(face.init(tables));

    for (size_t w = 0; w < fx.count; w++) {
      const ExpectedShaping& word = fx.words[w];
      ot::Plan plan;
      ASSERT_TRUE(plan.build(face, script, ot::languageTagsFor(word.language))) << fx.script << " " << word.utf8;
      std::vector<StoredGlyph> expected;
      for (unsigned i = 0; i < word.count; i++) {
        const ExpectedGlyph& g = word.glyphs[i];
        expected.push_back({g.gid, g.advance12_4, g.dx, g.dy});
      }
      EXPECT_EQ(shapeAsStored(face, plan, word.utf8), expected) << fx.script << " word " << w << ": " << word.utf8;
      words++;
    }
  }
  EXPECT_EQ(words, 56u);
}

// A fixture font with its stored filters and a built plan (CPpl dropped).
struct FixtureFont {
  std::vector<uint8_t> bytes;
  ot::Face face;
  ot::Plan plan;
};

bool loadFixture(const char* file, const ot::Script script, FixtureFont* out) {
  out->bytes = readFixture(file);
  ot::FaceTables tables;
  if (!ot::tablesFromSfnt(out->bytes.data(), static_cast<uint32_t>(out->bytes.size()), &tables)) return false;
  tables[ot::FaceTables::CPPL] = ot::Table();
  return out->face.init(tables) && out->plan.build(out->face, script, ot::languageTagsFor(""));
}

bool shapeWith(const FixtureFont& font, const std::vector<uint32_t>& cps, ot::Buffer& buffer) {
  return shapeRun(font.face, font.plan, cps.data(), static_cast<unsigned>(cps.size()), buffer);
}

// What a fresh buffer of 256 slots gives; empty when ot::shape fails.
std::vector<int32_t> freshGlyphs(const FixtureFont& font, const std::vector<uint32_t>& cps) {
  BoundBuffer fresh(256);
  if (!shapeWith(font, cps, fresh.buffer)) return {};
  return glyphsOf(fresh.buffer);
}

// A fixture font with its GSUB replaced by `gsub`, as upstream's
// OtShaperTest builds it (CPac dropped).
bool initWithGsub(ot::Face& face, const std::vector<uint8_t>& font, const std::vector<uint8_t>& gsub) {
  ot::FaceTables tables;
  if (!ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables)) return false;
  tables[ot::FaceTables::CPAC] = ot::Table();
  tables[ot::FaceTables::GSUB_TABLE] = ot::Table(gsub.data(), static_cast<uint32_t>(gsub.size()));
  return face.init(tables);
}

// A GSUB of one MultipleSubst lookup: glyph 5 becomes ten of glyph 6.
std::vector<uint8_t> multipleSubstGsub() {
  std::vector<uint8_t> bytes;
  const auto u16 = [&bytes](const std::initializer_list<uint16_t> words) {
    for (const uint16_t w : words) {
      bytes.push_back(static_cast<uint8_t>(w >> 8));
      bytes.push_back(static_cast<uint8_t>(w));
    }
  };
  u16({1, 0, 0, 0, 10});                    // header: v1.0, no scripts or features, lookups at 10
  u16({1, 4});                              // 10: LookupList, one lookup at 14
  u16({2, 0, 1, 8});                        // 14: Lookup, type 2, one subtable at 22
  u16({1, 30, 1, 8});                       // 22: MultipleSubstFormat1, coverage at 52, one Sequence at 30
  u16({10, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6});  // 30: Sequence, ten glyphs
  u16({1, 1, 5});                           // 52: Coverage, glyph 5
  return bytes;
}

}  // namespace

TEST(OtShaperReference, BuiltPlansMatchHarfBuzz) { checkAllWords(false); }

TEST(OtShaperReference, StoredFiltersMatchHarfBuzz) { checkAllWords(true); }

// Deviation 1: the buffer lives in storage its owner binds. A run's growth
// is capped at 2n + 16 glyphs (upstream's limit), so S slots take a run of
// (S - 16) / 2 code points.
TEST(OtShaperBuffer, MaxRunLengthIsTwoNPlusSixteen) {
  const struct {
    unsigned slots;
    unsigned run;
  } cases[] = {{16, 0}, {17, 0}, {18, 1}, {80, 32}, {112, 48}};
  for (const auto& c : cases) {
    BoundBuffer bound(c.slots);
    EXPECT_EQ(ot::maxRunLength(bound.buffer), c.run) << c.slots << " slots";
  }
}

// bytesFor() leaves room to align any start: the four arrays lie end to end
// inside the block, each with every slot.
TEST(OtShaperBuffer, SetStorageLaysTheArraysInsideTheBlock) {
  constexpr unsigned SLOTS = 80;
  const size_t bytes = ot::Buffer::bytesFor(SLOTS);
  std::vector<uint8_t> raw(bytes + alignof(ot::GlyphInfo));
  for (size_t offset = 0; offset < alignof(ot::GlyphInfo); offset++) {
    ot::Buffer buffer;
    buffer.setStorage(raw.data() + offset, bytes);
    const auto* const info = reinterpret_cast<const uint8_t*>(buffer.info.data());
    EXPECT_EQ(reinterpret_cast<uintptr_t>(info) % alignof(ot::GlyphInfo), 0u) << offset;
    EXPECT_GE(info, raw.data() + offset);
    EXPECT_EQ(buffer.scratch.data(), buffer.info.data() + SLOTS) << offset;
    EXPECT_EQ(static_cast<const void*>(buffer.pos.data()), static_cast<const void*>(buffer.scratch.data() + SLOTS));
    EXPECT_EQ(static_cast<const void*>(buffer.indices.data()), static_cast<const void*>(buffer.pos.data() + SLOTS));
    EXPECT_LE(reinterpret_cast<const uint8_t*>(buffer.indices.data() + SLOTS), raw.data() + offset + bytes);
    EXPECT_EQ(buffer.info.capacity(), SLOTS);
    EXPECT_EQ(buffer.scratch.capacity(), SLOTS);
    EXPECT_EQ(buffer.pos.capacity(), SLOTS);
    EXPECT_EQ(buffer.indices.capacity(), SLOTS);
  }
}

// prepare() refuses more than 65,536 glyphs (`indices` is uint16_t), so a
// larger block binds only that many slots and maxRunLength() names no run
// that prepare() would then refuse.
TEST(OtShaperBuffer, ABlockPastTheIndexLimitBindsAtMostItsSlots) {
  constexpr unsigned LIMIT = UINT16_MAX + 1u;
  BoundBuffer bound(LIMIT + 100);
  EXPECT_EQ(bound.buffer.info.capacity(), LIMIT);
  EXPECT_EQ(bound.buffer.indices.capacity(), LIMIT);
  EXPECT_EQ(ot::maxRunLength(bound.buffer), (LIMIT - 16) / 2);
  EXPECT_TRUE(bound.buffer.prepare(LIMIT));
}

// A growth past the storage fails the run through the Buffer's own
// `successful`, whichever array refuses it.
TEST(OtShaperBuffer, AGrowthPastTheStorageClearsSuccessful) {
  BoundBuffer bound(4);
  ot::Buffer& buffer = bound.buffer;
  ASSERT_TRUE(buffer.prepare(4));
  for (int i = 0; i < 5; i++) buffer.info.push_back(ot::GlyphInfo{});
  EXPECT_FALSE(buffer.successful);
  EXPECT_EQ(buffer.len(), 4u);

  ASSERT_TRUE(buffer.prepare(4));
  ASSERT_TRUE(buffer.successful);
  for (int i = 0; i < 5; i++) buffer.scratch.push_back(ot::GlyphInfo{});
  EXPECT_FALSE(buffer.successful);
  EXPECT_EQ(buffer.scratch.size(), 4u);

  ASSERT_TRUE(buffer.prepare(4));
  EXPECT_FALSE(buffer.pos.assign(5, ot::GlyphPosition{}));
  EXPECT_FALSE(buffer.successful);

  ASSERT_TRUE(buffer.prepare(4));
  EXPECT_FALSE(buffer.indices.resize(5));
  EXPECT_FALSE(buffer.successful);
}

TEST(OtShaperBuffer, NoStorageShapesNothing) {
  FixtureFont devanagari;
  ASSERT_TRUE(loadFixture("NotoSansDevanagari-Regular.layout", ot::Script::Devanagari, &devanagari));
  ot::Buffer buffer;
  EXPECT_FALSE(shapeWith(devanagari, utf32("हिन्दी"), buffer));
  EXPECT_TRUE(buffer.successful);
  EXPECT_EQ(ot::maxRunLength(buffer), 0u);
}

// A run longer than the storage takes is refused before shaping, with
// `successful` still true, so the caller can shape it in pieces.
TEST(OtShaperBuffer, ARunPastTheStorageFails) {
  FixtureFont devanagari;
  ASSERT_TRUE(loadFixture("NotoSansDevanagari-Regular.layout", ot::Script::Devanagari, &devanagari));
  std::vector<uint32_t> run;
  for (int i = 0; i < 5; i++) {
    const std::vector<uint32_t> word = utf32("हिन्दी");
    run.insert(run.end(), word.begin(), word.end());
  }
  const std::vector<uint32_t> tail = utf32("किक");  // क ि क
  run.insert(run.end(), tail.begin(), tail.end());
  ASSERT_EQ(run.size(), 33u);

  BoundBuffer bound(80);
  ASSERT_EQ(ot::maxRunLength(bound.buffer), 32u);
  EXPECT_FALSE(shapeWith(devanagari, run, bound.buffer));
  EXPECT_TRUE(bound.buffer.successful);

  run.pop_back();
  const std::vector<int32_t> expected = freshGlyphs(devanagari, run);
  ASSERT_FALSE(expected.empty());
  EXPECT_TRUE(shapeWith(devanagari, run, bound.buffer));
  EXPECT_EQ(glyphsOf(bound.buffer), expected);
}

// A font whose lookups grow a run past its limit fails that run; the same
// buffer then shapes the next one as a fresh buffer would.
TEST(OtShaperBuffer, AFailedRunLeavesTheBufferUsable) {
  FixtureFont tamil;
  ASSERT_TRUE(loadFixture("NotoSansTamil-Regular.layout", ot::Script::Tamil, &tamil));
  const std::vector<uint8_t> gsub = multipleSubstGsub();
  ASSERT_EQ(gsub.size(), 58u);
  ot::Face growing;
  ASSERT_TRUE(initWithGsub(growing, tamil.bytes, gsub));

  BoundBuffer bound(64);
  ot::Buffer& buffer = bound.buffer;
  ASSERT_TRUE(buffer.prepare(22));
  for (int i = 0; i < 3; i++) {
    ot::GlyphInfo g{};
    g.codepoint = 5;
    g.mask = 1;
    buffer.info.push_back(g);
  }
  ot::Scale scale;
  scale.set(kFixturePpem26_6, (kFixturePpem26_6 + 32) >> 6, growing.upem());
  ot::LookupSettings settings;
  settings.mask = 1;
  ot::applyLookup(growing, scale, buffer, ot::GSUB, 0, settings);
  // 3 glyphs grow to 12, then 21; the third expansion would make 30.
  EXPECT_FALSE(buffer.successful);
  EXPECT_EQ(buffer.len(), 21u);

  const std::vector<uint32_t> word = utf32("தமிழ்");
  const std::vector<int32_t> expected = freshGlyphs(tamil, word);
  ASSERT_FALSE(expected.empty());
  EXPECT_TRUE(shapeWith(tamil, word, buffer));
  EXPECT_EQ(glyphsOf(buffer), expected);
}

// Passes that rebuild the glyph string swap `info` and `scratch`; the next
// run binds the arrays afresh.
TEST(OtShaperBuffer, TheNextRunUndoesASwap) {
  FixtureFont bengali;
  ASSERT_TRUE(loadFixture("NotoSansBengali-Regular.layout", ot::Script::Bengali, &bengali));
  const std::vector<uint32_t> word = utf32("হৃদয়");
  const std::vector<int32_t> expected = freshGlyphs(bengali, word);
  ASSERT_FALSE(expected.empty());

  BoundBuffer bound(256);
  ot::Buffer& buffer = bound.buffer;
  const ot::GlyphInfo* const infoStart = buffer.info.data();
  // A broken syllable: normalisation swaps the arrays twice, the dotted
  // circle once more, so the run ends with them swapped.
  EXPECT_TRUE(shapeWith(bengali, utf32("েদের"), buffer));
  EXPECT_TRUE(buffer.hasBrokenSyllable);
  EXPECT_NE(buffer.info.data(), infoStart);
  // The next prepare() lays the arrays over the block in order again.
  ASSERT_TRUE(buffer.prepare(16));
  EXPECT_EQ(buffer.info.data(), infoStart);
  EXPECT_EQ(buffer.scratch.data(), infoStart + 256);

  EXPECT_TRUE(shapeWith(bengali, word, buffer));
  EXPECT_EQ(glyphsOf(buffer), expected);
}

// A face re-pointed at a copy of its bytes (deviation 12) shapes as a fresh
// one does, the old bytes poisoned.
namespace {

void checkMovedFace(const bool withFilters) {
  size_t words = 0;
  for (const ShapingFixture& fx : kShapingFixtures) {
    ot::Script script = ot::Script::Devanagari;
    ASSERT_TRUE(scriptOf(fx.script, &script)) << fx.script;
    std::vector<uint8_t> a = readFixture(fx.layoutFile);
    ASSERT_FALSE(a.empty()) << fx.layoutFile;
    const std::vector<uint8_t> b = a;
    ot::FaceTables tables;
    ASSERT_TRUE(ot::tablesFromSfnt(a.data(), static_cast<uint32_t>(a.size()), &tables));
    if (!withFilters) tables[ot::FaceTables::CPAC] = ot::Table();
    tables[ot::FaceTables::CPPL] = ot::Table();
    ot::Face face;
    ASSERT_TRUE(face.init(tables));
    std::vector<ot::Plan> plans(fx.count);
    for (size_t w = 0; w < fx.count; w++) {
      ASSERT_TRUE(plans[w].build(face, script, ot::languageTagsFor(fx.words[w].language))) << fx.words[w].utf8;
    }
    // Moved: B is the live copy, A is gone.
    std::vector<uint8_t> moved = b;
    face.rebase(a.data(), moved.data());
    std::fill(a.begin(), a.end(), static_cast<uint8_t>(0xA5));
    for (size_t w = 0; w < fx.count; w++) {
      const ExpectedShaping& word = fx.words[w];
      std::vector<StoredGlyph> expected;
      for (unsigned i = 0; i < word.count; i++) {
        const ExpectedGlyph& g = word.glyphs[i];
        expected.push_back({g.gid, g.advance12_4, g.dx, g.dy});
      }
      EXPECT_EQ(shapeAsStored(face, plans[w], word.utf8), expected) << fx.script << " word " << w << ": " << word.utf8;
      words++;
    }
  }
  EXPECT_EQ(words, 56u);
}

}  // namespace

TEST(OtFaceRebase, AMovedFaceShapesAsAFreshOne) {
  checkMovedFace(true);
  checkMovedFace(false);
}

TEST(OtFaceRebase, TheFilterRecordsMoveButTheirAddressStays) {
  std::vector<uint8_t> a = readFixture(kShapingFixtures[0].layoutFile);
  ASSERT_FALSE(a.empty());
  std::vector<uint8_t> b = a;
  ot::FaceTables tables;
  ASSERT_TRUE(ot::tablesFromSfnt(a.data(), static_cast<uint32_t>(a.size()), &tables));
  tables[ot::FaceTables::CPPL] = ot::Table();
  ot::Face face;
  ASSERT_TRUE(face.init(tables));
  const ot::FilterRecords* before = face.filters(ot::GSUB);
  ASSERT_NE(before, nullptr);
  const uint8_t* digests = before->lookupDigests;
  ASSERT_GE(digests, a.data());
  ASSERT_LT(digests, a.data() + a.size());
  face.rebase(a.data(), b.data());
  EXPECT_EQ(face.filters(ot::GSUB), before);
  EXPECT_EQ(before->lookupDigests, b.data() + (digests - a.data()));
}

TEST(OtShaperHelpers, DecodeUtf8StopsAtATruncatedSequence) {
  const char s[] = "\xE0\xA4\0A";
  uint32_t out[4] = {};
  EXPECT_EQ(ottest::decodeUtf8(s, out, 4), 1u);
  EXPECT_EQ(ottest::decodeUtf8("\xF0\x9F", nullptr, 0), 1u);
}
