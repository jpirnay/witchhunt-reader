#include <ExpectedGlyphRuns.h>
#include <ExpectedShaping.h>
#include <OtFilterWriter.h>
#include <OtShaper.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
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

using ottest::BoundBuffer;
using ottest::glyphsOf;
using ottest::readFileIn;
using ottest::readFixture;
using ottest::scriptOf;
using ottest::shapeAsStored;
using ottest::shapeRun;
using ottest::StoredGlyph;
using ottest::utf32;

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

// The Noto Sans Devanagari subsets of test/fixtures/fonts, over their own tables, against HarfBuzz's runs for them
// (lib/EpdFont/scripts/make_shaping_fixtures.py).
TEST(OtShaperReference, TheTrueTypeSubsetShapesAsHarfBuzz) {
  size_t runs = 0;
  for (const RunSet& set : kDevanagariRunSets) {
    const std::vector<uint8_t> font = readFileIn(TTF_FIXTURE_DIR, set.file);
    ASSERT_FALSE(font.empty()) << set.file;
    ot::FaceTables tables;
    ASSERT_TRUE(ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables)) << set.file;
    tables[ot::FaceTables::CPAC] = ot::Table();
    tables[ot::FaceTables::CPPL] = ot::Table();
    ot::Face face;
    ASSERT_TRUE(face.init(tables)) << set.file;

    for (size_t r = 0; r < set.count; r++) {
      const ExpectedRun& run = set.runs[r];
      ot::Plan plan;
      ASSERT_TRUE(plan.build(face, ot::Script::Devanagari, ot::languageTagsFor(run.language)))
          << set.file << " " << run.utf8;
      std::vector<StoredGlyph> expected;
      for (unsigned i = 0; i < run.count; i++) {
        const RunGlyph& g = run.glyphs[i];
        expected.push_back({g.gid, g.advance12_4, g.dx, g.dy});
      }
      EXPECT_EQ(shapeAsStored(face, plan, run.utf8, set.ppem26_6), expected)
          << set.file << " at " << set.ppem26_6 << ", run " << r << ": " << run.utf8;
      runs++;
    }
  }
  EXPECT_EQ(runs, 64u);
}

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

// The CPac table written from a face (deviation 4): byte for byte what the converter wrote, in any piece size, and
// what Face::init() reads back.
namespace {

// The whole table in one call.
std::vector<uint8_t> writeAll(ot::FilterTableWriter& writer) {
  std::vector<uint8_t> out(writer.bytes());
  EXPECT_EQ(writer.write(out.data(), static_cast<uint32_t>(out.size())), out.size());
  return out;
}

// A GSUB of two lookups: an Extension lookup whose two subtables extend types 1 and 4 (HarfBuzz drops it), then a
// SingleSubst lookup of one subtable.
std::vector<uint8_t> mixedExtensionGsub() {
  std::vector<uint8_t> bytes;
  const auto u16 = [&bytes](const std::initializer_list<uint16_t> words) {
    for (const uint16_t w : words) {
      bytes.push_back(static_cast<uint8_t>(w >> 8));
      bytes.push_back(static_cast<uint8_t>(w));
    }
  };
  u16({1, 0, 0, 0, 10});   // header: v1.0, no scripts or features, lookups at 10
  u16({2, 6, 48});         // 10: LookupList, lookups at 16 and 58
  u16({7, 0, 2, 10, 30});  // 16: Extension lookup, subtables at 26 and 46
  u16({1, 1, 0, 8});       // 26: ExtensionSubst extending type 1, at 34
  u16({1, 6, 1});          // 34: SingleSubstFormat1, coverage at 40
  u16({1, 1, 5});          // 40: Coverage, glyph 5
  u16({1, 4, 0, 8});       // 46: ExtensionSubst extending type 4, at 54
  u16({0, 0});             // 54: an empty extended subtable
  u16({1, 0, 1, 8});       // 58: SingleSubst lookup, one subtable at 66
  u16({1, 6, 1});          // 66: SingleSubstFormat1, coverage at 72
  u16({1, 1, 5});          // 72: Coverage, glyph 5
  return bytes;
}

}  // namespace

TEST(OtFilterWriter, WritesTheConvertersTableByteForByte) {
  for (const ShapingFixture& fx : kShapingFixtures) {
    const std::vector<uint8_t> font = readFixture(fx.layoutFile);
    ASSERT_FALSE(font.empty()) << fx.layoutFile;
    ot::FaceTables tables;
    ASSERT_TRUE(ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables));
    const ot::Table converter = tables[ot::FaceTables::CPAC];
    ASSERT_FALSE(converter.empty()) << fx.layoutFile;
    tables[ot::FaceTables::CPAC] = ot::Table();
    ot::Face face;
    ASSERT_TRUE(face.init(tables));

    ot::FilterTableWriter writer(face);
    ASSERT_EQ(writer.bytes(), converter.length()) << fx.layoutFile;
    const std::vector<uint8_t> written = writeAll(writer);
    EXPECT_EQ(std::memcmp(written.data(), converter.data(), written.size()), 0) << fx.layoutFile;
  }
}

TEST(OtFilterWriter, PiecesOfAnySizeGiveTheSameBytes) {
  const std::vector<uint8_t> font = readFixture(kShapingFixtures[0].layoutFile);
  ot::FaceTables tables;
  ASSERT_TRUE(ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables));
  tables[ot::FaceTables::CPAC] = ot::Table();
  ot::Face face;
  ASSERT_TRUE(face.init(tables));
  ot::FilterTableWriter whole(face);
  const std::vector<uint8_t> expected = writeAll(whole);

  for (const uint32_t piece : {1u, 7u, 24u, 4096u}) {
    ot::FilterTableWriter writer(face);
    std::vector<uint8_t> out;
    std::vector<uint8_t> buffer(piece);
    for (uint32_t n; (n = writer.write(buffer.data(), piece)) > 0;) {
      ASSERT_LE(n, piece);
      out.insert(out.end(), buffer.begin(), buffer.begin() + n);
    }
    EXPECT_EQ(out, expected) << "pieces of " << piece;
    EXPECT_EQ(writer.write(buffer.data(), piece), 0u) << "pieces of " << piece;
  }
}

TEST(OtFilterWriter, TheTableItWritesIsTheOneTheFaceReads) {
  size_t words = 0;
  for (const ShapingFixture& fx : kShapingFixtures) {
    ot::Script script = ot::Script::Devanagari;
    ASSERT_TRUE(scriptOf(fx.script, &script)) << fx.script;
    const std::vector<uint8_t> font = readFixture(fx.layoutFile);
    ot::FaceTables tables;
    ASSERT_TRUE(ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables));
    tables[ot::FaceTables::CPAC] = ot::Table();
    tables[ot::FaceTables::CPPL] = ot::Table();
    ot::Face bare;
    ASSERT_TRUE(bare.init(tables));
    ot::FilterTableWriter writer(bare);
    const std::vector<uint8_t> cpac = writeAll(writer);

    tables[ot::FaceTables::CPAC] = ot::Table(cpac.data(), static_cast<uint32_t>(cpac.size()));
    ot::Face face;
    ASSERT_TRUE(face.init(tables));
    EXPECT_NE(face.filters(ot::GSUB), nullptr) << fx.script;
    EXPECT_NE(face.filters(ot::GPOS), nullptr) << fx.script;
    for (size_t w = 0; w < fx.count; w++) {
      const ExpectedShaping& word = fx.words[w];
      ot::Plan plan;
      ASSERT_TRUE(plan.build(face, script, ot::languageTagsFor(word.language))) << word.utf8;
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

TEST(OtFilterWriter, AFaceWithoutLookupsHasNoTable) {
  const std::vector<uint8_t> font = readFixture("NotoSansTamil-Regular.layout");
  ot::FaceTables tables;
  ASSERT_TRUE(ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables));
  tables[ot::FaceTables::CPAC] = ot::Table();
  tables[ot::FaceTables::GSUB_TABLE] = ot::Table();
  tables[ot::FaceTables::GPOS_TABLE] = ot::Table();
  ot::Face face;
  ASSERT_TRUE(face.init(tables));
  ot::FilterTableWriter writer(face);
  EXPECT_EQ(writer.bytes(), 0u);
  uint8_t out[8];
  EXPECT_EQ(writer.write(out, sizeof(out)), 0u);
}

TEST(OtFilterWriter, AnExtensionLookupMixingTypesHasNoSubtables) {
  const std::vector<uint8_t> font = readFixture("NotoSansTamil-Regular.layout");
  const std::vector<uint8_t> gsub = mixedExtensionGsub();
  ot::Face bare;
  ASSERT_TRUE(initWithGsub(bare, font, gsub));
  ASSERT_EQ(bare.lookupCount(ot::GSUB), 2u);
  ASSERT_TRUE(bare.lookup(ot::GSUB, 0).empty());  // dropped
  ot::FilterTableWriter writer(bare);
  const std::vector<uint8_t> cpac = writeAll(writer);

  const uint32_t gpos = bare.lookupCount(ot::GPOS);
  // GSUB: header 16, two lookup digests, then the first-subtable indices.
  const uint8_t* starts = cpac.data() + 16 + 2 * ot::DIGEST_BYTES;
  const auto u32 = [](const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           p[3];
  };
  EXPECT_EQ(u32(starts), 0u);
  EXPECT_EQ(u32(starts + 4), 0u);
  // Header: one subtable in GSUB.
  EXPECT_EQ(u32(cpac.data() + 8), 1u);
  EXPECT_GT(gpos, 0u);

  ot::FaceTables tables;
  ASSERT_TRUE(ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables));
  tables[ot::FaceTables::GSUB_TABLE] = ot::Table(gsub.data(), static_cast<uint32_t>(gsub.size()));
  tables[ot::FaceTables::CPAC] = ot::Table(cpac.data(), static_cast<uint32_t>(cpac.size()));
  ot::Face face;
  ASSERT_TRUE(face.init(tables));
  EXPECT_NE(face.filters(ot::GSUB), nullptr);
}

// A GSUB of one lookup declaring `subtables` subtables (all at one offset, so the table stays small), padded to hold
// the offsets the declaration needs.
static std::vector<uint8_t> gsubWithSubtables(const uint16_t subtables) {
  std::vector<uint8_t> bytes = {0,
                                1,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                10,  // v1.0, no scripts or features, lookups at 10
                                0,
                                1,
                                0,
                                4,  // 10: LookupList, one lookup at 4
                                0,
                                1,
                                0,
                                0,  // 14: lookup type 1, no flags
                                static_cast<uint8_t>(subtables >> 8),
                                static_cast<uint8_t>(subtables)};
  bytes.resize(bytes.size() + 2u * subtables, 0);
  return bytes;
}

TEST(OtFilterWriter, MoreSubtablesThanTheCapGiveNoTable) {
  const std::vector<uint8_t> font = readFixture("NotoSansTamil-Regular.layout");
  ot::FaceTables tables;
  ASSERT_TRUE(ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables));
  tables[ot::FaceTables::CPAC] = ot::Table();
  tables[ot::FaceTables::GPOS_TABLE] = ot::Table();
  const auto bytesFor = [&](const uint16_t subtables) {
    const std::vector<uint8_t> gsub = gsubWithSubtables(subtables);
    tables[ot::FaceTables::GSUB_TABLE] = ot::Table(gsub.data(), static_cast<uint32_t>(gsub.size()));
    ot::Face face;
    EXPECT_TRUE(face.init(tables));
    EXPECT_EQ(face.lookupCount(ot::GSUB), 1u);
    return ot::FilterTableWriter(face).bytes();
  };
  constexpr uint32_t kCap = ot::FilterTableWriter::kMaxSubtables;
  EXPECT_EQ(bytesFor(kCap), 16u + 28u + 24u * kCap);  // at the cap: written
  EXPECT_EQ(bytesFor(kCap + 1), 0u);                  // one past: none
  EXPECT_EQ(bytesFor(UINT16_MAX), 0u);
}

// A GSUB of `lookups` lookups of no subtables.
static std::vector<uint8_t> gsubWithLookups(const uint16_t lookups) {
  std::vector<uint8_t> bytes;
  const auto u16 = [&bytes](const std::initializer_list<uint16_t> words) {
    for (const uint16_t w : words) {
      bytes.push_back(static_cast<uint8_t>(w >> 8));
      bytes.push_back(static_cast<uint8_t>(w));
    }
  };
  u16({1, 0, 0, 0, 10});  // header: v1.0, no scripts or features, lookups at 10
  u16({lookups});         // 10: LookupList
  for (uint32_t i = 0; i < lookups; i++) u16({static_cast<uint16_t>(2 + 2 * lookups + 6 * i)});
  for (uint32_t i = 0; i < lookups; i++) u16({1, 0, 0});  // SingleSubst, no flags, no subtables
  return bytes;
}

TEST(OtFilterWriter, MoreLookupsThanTheCapGiveNoTable) {
  const std::vector<uint8_t> font = readFixture("NotoSansTamil-Regular.layout");
  ot::FaceTables tables;
  ASSERT_TRUE(ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables));
  tables[ot::FaceTables::CPAC] = ot::Table();
  tables[ot::FaceTables::GPOS_TABLE] = ot::Table();
  const auto bytesFor = [&](const uint16_t lookups) {
    const std::vector<uint8_t> gsub = gsubWithLookups(lookups);
    tables[ot::FaceTables::GSUB_TABLE] = ot::Table(gsub.data(), static_cast<uint32_t>(gsub.size()));
    ot::Face face;
    EXPECT_TRUE(face.init(tables));
    EXPECT_EQ(face.lookupCount(ot::GSUB), lookups);
    return ot::FilterTableWriter(face).bytes();
  };
  constexpr uint32_t kCap = ot::FilterTableWriter::kMaxLookups;
  EXPECT_EQ(bytesFor(kCap), 16u + 28u * kCap);  // at the cap: written
  EXPECT_EQ(bytesFor(kCap + 1), 0u);            // one past: none
}

// A GSUB of one Extension lookup declaring `subtables` subtables, all at one ExtensionSubst that extends a SingleSubst
// of glyph 5.
static std::vector<uint8_t> extensionGsubOf(const uint16_t subtables) {
  std::vector<uint8_t> bytes;
  const auto u16 = [&bytes](const std::initializer_list<uint16_t> words) {
    for (const uint16_t w : words) {
      bytes.push_back(static_cast<uint8_t>(w >> 8));
      bytes.push_back(static_cast<uint8_t>(w));
    }
  };
  u16({1, 0, 0, 0, 10});   // header: v1.0, no scripts or features, lookups at 10
  u16({1, 4});             // 10: LookupList, one lookup at 14
  u16({7, 0, subtables});  // 14: an Extension lookup
  const uint16_t shared = static_cast<uint16_t>(6 + 2 * subtables);
  for (uint32_t i = 0; i < subtables; i++) u16({shared});
  u16({1, 1, 0, 8});  // ExtensionSubst extending type 1, at 8
  u16({1, 6, 1});     // SingleSubstFormat1, coverage at 6
  u16({1, 1, 5});     // Coverage, glyph 5
  return bytes;
}

TEST(OtFilterWriter, AnExtensionLookupAtTheSubtableCapIsWrittenWhole) {
  // One Extension lookup of 16,384 subtables, the cap: Face::lookup() checks every one of them, so the writer reads
  // the lookup once for all its subtable digests (once a subtable, it took 16,384 times that). Each digest is the one
  // coverage's, glyph 5.
  constexpr uint32_t kCap = ot::FilterTableWriter::kMaxSubtables;
  const std::vector<uint8_t> font = readFixture("NotoSansTamil-Regular.layout");
  const std::vector<uint8_t> gsub = extensionGsubOf(static_cast<uint16_t>(kCap));
  ot::Face face;
  ASSERT_TRUE(initWithGsub(face, font, gsub));
  ASSERT_EQ(ot::Face::subtableCount(face.lookup(ot::GSUB, 0)), kCap);
  ot::FilterTableWriter writer(face);
  const std::vector<uint8_t> cpac = writeAll(writer);
  ASSERT_EQ(cpac.size(), writer.bytes());
  const auto u32 = [](const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           p[3];
  };
  ASSERT_EQ(u32(cpac.data() + 8), kCap);  // GSUB's subtables
  // GSUB: header 16, the lookup's digest, its first-subtable index, then the subtable digests, each the lookup's.
  const uint8_t* const lookupDigest = cpac.data() + 16;
  const uint8_t* const digests = lookupDigest + ot::DIGEST_BYTES + 4;
  ASSERT_LE(static_cast<size_t>(digests - cpac.data()) + size_t{kCap} * ot::DIGEST_BYTES, cpac.size());
  EXPECT_FALSE(std::all_of(lookupDigest, lookupDigest + ot::DIGEST_BYTES, [](const uint8_t b) { return b == 0; }));
  uint32_t differing = 0;
  for (uint32_t i = 0; i < kCap; i++) {
    if (std::memcmp(digests + size_t{i} * ot::DIGEST_BYTES, lookupDigest, ot::DIGEST_BYTES) != 0) differing++;
  }
  EXPECT_EQ(differing, 0u);
}

TEST(OtShaperHelpers, DecodeUtf8StopsAtATruncatedSequence) {
  const char s[] = "\xE0\xA4\0A";
  uint32_t out[4] = {};
  EXPECT_EQ(ottest::decodeUtf8(s, out, 4), 1u);
  EXPECT_EQ(ottest::decodeUtf8("\xF0\x9F", nullptr, 0), 1u);
}
