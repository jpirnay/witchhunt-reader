#include <OtFilterWriter.h>
#include <OtShaper.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "../ot_shaper_port/OtShaperTestHelpers.h"
#include "MyanmarExpected.h"
#include "OtShaperInternal.h"

// The Myanmar shaper (deviation 14, the Myanmar design's PR 1) on its fixture (test/fixtures/fonts,
// make_myanmar_fixtures.py): the lookups lib/OtShaper plans and the runs it shapes, checked against HarfBuzz
// 14.5.0's; the shaper HarfBuzz picks for each script tag; the initial reordering on the cases it exists for; and the
// refusals of a run that does not fit. The words include Somnius/CrossInk-Indic's Myanmar test words (Lef, MIT).

namespace {

using ottest::BoundBuffer;
using ottest::glyphsOf;
using ottest::StoredGlyph;
using Cps = std::vector<uint32_t>;

constexpr uint32_t KA = 0x1000, GA = 0x1002, NGA = 0x1004, NA = 0x1014, BHA = 0x1018, MA = 0x1019, LA = 0x101C,
                   PA = 0x1015, AA = 0x102C, E = 0x1031, VIRAMA = 0x1039, ASAT = 0x103A, MEDIAL_RA = 0x103C,
                   DOT_BELOW = 0x1037;

std::vector<uint8_t> readFont(const char* name) {
  std::vector<uint8_t> bytes;
  if (FILE* f = std::fopen((std::string(TTF_FIXTURE_DIR) + "/" + name).c_str(), "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
  }
  return bytes;
}

uint32_t be32(const std::vector<uint8_t>& b, const size_t at) {
  return static_cast<uint32_t>(b[at]) << 24 | static_cast<uint32_t>(b[at + 1]) << 16 |
         static_cast<uint32_t>(b[at + 2]) << 8 | b[at + 3];
}
uint16_t be16(const std::vector<uint8_t>& b, const size_t at) { return static_cast<uint16_t>(b[at] << 8 | b[at + 1]); }

// Renames a script tag of GSUB's and GPOS's ScriptList. The fixture's lists hold mym2 only, so they stay sorted.
void renameScript(std::vector<uint8_t>& font, const char* from, const char* to) {
  for (const char* table : {"GSUB", "GPOS"}) {
    size_t at = 0;
    for (unsigned i = 0; i < be16(font, 4); i++) {
      if (std::memcmp(&font[12 + 16 * i], table, 4) == 0) at = be32(font, 12 + 16 * i + 8);
    }
    ASSERT_NE(at, 0u) << table;
    const size_t scripts = at + be16(font, at + 4);
    ASSERT_EQ(be16(font, scripts), 1u) << table;
    if (std::memcmp(&font[scripts + 2], from, 4) == 0) std::memcpy(&font[scripts + 2], to, 4);
  }
}

// The fixture with its lookup filters computed, or read from the CPac FilterTableWriter writes for it.
struct FixtureFace {
  std::vector<uint8_t> font;
  std::vector<uint8_t> cpac;
  ot::Face face;

  bool open(std::vector<uint8_t> bytes, const bool withCpac = false) {
    font = std::move(bytes);
    ot::FaceTables tables;
    if (font.empty() || !ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables)) return false;
    if (!face.init(tables)) return false;
    if (!withCpac) return face.filters(ot::GSUB) == nullptr && face.filters(ot::GPOS) == nullptr;
    ot::FilterTableWriter writer(face);
    cpac.resize(writer.bytes());
    if (cpac.empty() || writer.write(cpac.data(), static_cast<uint32_t>(cpac.size())) != cpac.size()) return false;
    tables[ot::FaceTables::CPAC] = ot::Table(cpac.data(), static_cast<uint32_t>(cpac.size()));
    return face.init(tables) && face.filters(ot::GSUB) != nullptr && face.filters(ot::GPOS) != nullptr;
  }
  bool open(const bool withCpac = false) { return open(readFont(kFontNotoSansMyanmarShapingRegular), withCpac); }
};

std::vector<StoredGlyph> recorded(const MyanmarShapingExpectation& word) {
  std::vector<StoredGlyph> out;
  for (unsigned i = 0; i < word.count; i++) {
    const ExpectedGlyph& g = word.glyphs[i];
    out.push_back({g.gid, g.advance12_4, g.dx, g.dy});
  }
  return out;
}

const MyanmarShapingExpectation* findRecorded(const Cps& cps, const char* language) {
  for (const MyanmarShapingExpectation& word : kMyanmarShaping) {
    if (ottest::utf32(word.utf8) == cps && std::strcmp(word.language, language) == 0) return &word;
  }
  return nullptr;
}

std::string codepointsOf(const char* utf8) {
  std::ostringstream s;
  s << std::hex << std::uppercase;
  const char* separator = "U+";
  for (const uint32_t cp : ottest::utf32(utf8)) {
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

// Glyph IDs and positions in scaled units, flattened, of `cps` shaped with its first `before` and last `after` code
// points as context; empty when ot::shape fails.
std::vector<int32_t> shapeCodepoints(const ot::Face& face, const ot::Plan& plan, const Cps& cps,
                                     const unsigned before = 0, const unsigned after = 0) {
  BoundBuffer bound(256);
  ot::Scale scale;
  scale.set(kFixturePpem26_6, (kFixturePpem26_6 + 32) >> 6, face.upem());
  if (!ot::shape(face, scale, plan, cps.data(), static_cast<unsigned>(cps.size()), bound.buffer, {before, after})) {
    return {};
  }
  return glyphsOf(bound.buffer);
}

// `cps` through shape()'s steps up to the reordering pause, without the lookups: unicode properties, normalization,
// the categories, the glyphs, the syllables, then the dotted circles and the initial reordering. `maxGlyphs` is the
// run's reservation.
struct Reordered {
  ot::Buffer buffer;
  uint8_t storage[ot::Buffer::bytesFor(64)];
  std::vector<uint8_t> types;  // each glyph's syllable type before the reordering

  Reordered(const ot::Face& face, const ot::Plan& plan, const Cps& cps, const unsigned maxGlyphs = 64) {
    buffer.setStorage(storage, sizeof(storage));
    if (!buffer.prepare(maxGlyphs)) return;
    for (const uint32_t cp : cps) {
      ot::GlyphInfo g{};
      g.codepoint = cp;
      g.mask = plan.globalMask;
      ot::setUnicodeProps(g, buffer);
      buffer.info.push_back(g);
    }
    ot::normalize(face, plan, buffer);
    if (!buffer.successful) return;
    ot::myanmarSetupMasks(buffer);
    for (ot::GlyphInfo& g : buffer.info) {
      g.codepoint = g.glyphIndex;
      g.glyphProps = face.glyphProps(g.codepoint);
    }
    ot::myanmarSetupSyllables(buffer);
    for (unsigned i = 0; i < buffer.len(); i++) types.push_back(buffer.info[i].syllableType());
    ot::myanmarReorder(face, buffer);
  }
  Cps glyphs() const {
    Cps out;
    for (unsigned i = 0; i < buffer.len(); i++) out.push_back(buffer.info[i].codepoint);
    return out;
  }
};

// The font's nominal glyphs of `cps`, U+25CC the dotted circle.
Cps nominal(const ot::Face& face, const Cps& cps) {
  Cps out;
  for (const uint32_t cp : cps) {
    uint32_t glyph = 0;
    EXPECT_TRUE(face.nominalGlyph(cp, &glyph)) << std::hex << cp;
    out.push_back(glyph);
  }
  return out;
}

}  // namespace

TEST(OtPlanMyanmar, PlannedLookupsMatchHarfBuzz) {
  FixtureFace font;
  ASSERT_TRUE(font.open());
  size_t checked = 0;
  for (const MyanmarPlanExpectation& expected : kMyanmarPlans) {
    const std::string where = std::string("lang=") + (*expected.language ? expected.language : "none");
    ot::Plan plan;
    ASSERT_TRUE(plan.build(font.face, ot::Script::Myanmar, ot::languageTagsFor(expected.language))) << where;
    EXPECT_EQ(plan.chosenScript, ot::tag("mym2")) << where;
    EXPECT_EQ(plan.shaper, ot::ShaperKind::Myanmar) << where;
    EXPECT_TRUE(plan.zeroMarks) << where;
    EXPECT_FALSE(plan.backward) << where;
    ASSERT_EQ(plan.lookups[ot::GSUB].size(), expected.gsubCount) << where;
    for (size_t i = 0; i < expected.gsubCount; i++) {
      EXPECT_EQ(plan.lookups[ot::GSUB][i].index, expected.gsub[i]) << where << " GSUB " << i;
    }
    ASSERT_EQ(plan.lookups[ot::GPOS].size(), expected.gposCount) << where;
    for (size_t i = 0; i < expected.gposCount; i++) {
      EXPECT_EQ(plan.lookups[ot::GPOS][i].index, expected.gpos[i]) << where << " GPOS " << i;
    }
    ASSERT_EQ(plan.stages[ot::GSUB].size(), kMyanmarGsubStages) << where;
    EXPECT_EQ(plan.stages[ot::GSUB][1].pause, ot::Pause::MyanmarSetupSyllables) << where;
    EXPECT_EQ(plan.stages[ot::GSUB][2].pause, ot::Pause::MyanmarReorder) << where;
    EXPECT_EQ(plan.stages[ot::GPOS].size(), 1u) << where;
    checked++;
  }
  EXPECT_EQ(checked, 5u);  // none, my, mnw, ksw, shn
}

// hb_ot_shaper_categorize(): mymr (the tag from before the shaping spec), DFLT and latn take the default shaper; any
// other choice the Myanmar shaper, dflt and a font with no script for the run among them.
TEST(OtPlanMyanmar, ShaperFollowsTheScriptTag) {
  const struct {
    const char* tag;
    ot::ShaperKind shaper;
    uint32_t chosen;
  } cases[] = {
      {"mym2", ot::ShaperKind::Myanmar, ot::tag("mym2")}, {"mymr", ot::ShaperKind::Default, ot::tag("mymr")},
      {"DFLT", ot::ShaperKind::Default, ot::tag("DFLT")}, {"latn", ot::ShaperKind::Default, ot::tag("latn")},
      {"dflt", ot::ShaperKind::Myanmar, ot::tag("dflt")}, {"zzzz", ot::ShaperKind::Myanmar, 0},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.tag);
    std::vector<uint8_t> bytes = readFont(kFontNotoSansMyanmarShapingRegular);
    renameScript(bytes, "mym2", c.tag);
    FixtureFace font;
    ASSERT_TRUE(font.open(std::move(bytes)));
    ot::Plan plan;
    ASSERT_TRUE(plan.build(font.face, ot::Script::Myanmar, ot::languageTagsFor("")));
    EXPECT_EQ(plan.chosenScript, c.chosen);
    EXPECT_EQ(plan.shaper, c.shaper);
    EXPECT_EQ(plan.stages[ot::GSUB].size(), c.shaper == ot::ShaperKind::Myanmar ? kMyanmarGsubStages : 2u);
  }
}

// initial_reordering_consonant_syllable() on the font's nominal glyphs, no lookup run: the pre-base vowel and medial
// ra move before their consonant, kinzi after it, a stack stays, and a broken cluster gets a dotted circle first.
TEST(OtMyanmarReorder, MovesEachSignWhereHarfBuzzDoes) {
  FixtureFace font;
  ASSERT_TRUE(font.open());
  ot::Plan plan;
  ASSERT_TRUE(plan.build(font.face, ot::Script::Myanmar, ot::languageTagsFor("")));
  constexpr uint32_t DC = ot::DOTTED_CIRCLE;
  const struct {
    const char* what;
    Cps text;
    Cps visual;
  } cases[] = {
      {"pre-base vowel", {KA, E}, {E, KA}},
      {"two-part vowel", {KA, E, AA}, {E, KA, AA}},
      {"medial ra", {KA, MEDIAL_RA}, {MEDIAL_RA, KA}},
      {"medial ra and e", {KA, MEDIAL_RA, E}, {E, MEDIAL_RA, KA}},
      {"kinzi", {NGA, ASAT, VIRAMA, GA, AA}, {GA, NGA, ASAT, VIRAMA, AA}},
      {"kinzi and e", {NGA, ASAT, VIRAMA, KA, E}, {E, KA, NGA, ASAT, VIRAMA}},
      {"stack", {MA, VIRAMA, BHA, AA}, {MA, VIRAMA, BHA, AA}},
      {"stack and e", {KA, VIRAMA, KA, E}, {E, KA, VIRAMA, KA}},
      {"dot below and asat", {NA, DOT_BELOW, ASAT}, {NA, DOT_BELOW, ASAT}},
      {"dotted circle for e", {E}, {E, DC}},
      {"dotted circle for e first", {E, KA}, {E, DC, KA}},
      {"dotted circle for medial ra", {MEDIAL_RA}, {MEDIAL_RA, DC}},
      {"two stackers", {VIRAMA, VIRAMA}, {DC, VIRAMA, DC, VIRAMA}},
      {"two syllables", {MA, MEDIAL_RA, NA, ASAT, MA, AA}, {MEDIAL_RA, MA, NA, ASAT, MA, AA}},
      {"kinzi word", {MA, NGA, ASAT, VIRAMA, GA, LA, AA, PA}, {MA, GA, NGA, ASAT, VIRAMA, LA, AA, PA}},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.what);
    const Reordered r(font.face, plan, c.text);
    ASSERT_TRUE(r.buffer.successful);
    EXPECT_EQ(r.glyphs(), nominal(font.face, c.visual));
  }
  // The syllable types: a consonant syllable, a broken cluster, a joiner alone (not Myanmar: no circle).
  EXPECT_EQ(Reordered(font.face, plan, {KA, E}).types,
            (std::vector<uint8_t>{ot::msyl::CONSONANT, ot::msyl::CONSONANT}));
  const Reordered broken(font.face, plan, {E});
  EXPECT_EQ(broken.types, std::vector<uint8_t>{ot::msyl::BROKEN});
  EXPECT_TRUE(broken.buffer.hasBrokenSyllable);
  const Reordered joiner(font.face, plan, {0x200D});
  EXPECT_EQ(joiner.types, std::vector<uint8_t>{ot::msyl::NON_MYANMAR});
  EXPECT_FALSE(joiner.buffer.hasBrokenSyllable);
  EXPECT_EQ(joiner.buffer.len(), 1u);
}

// Every word make_myanmar_fixtures.py recorded: the glyph IDs, advances (12.4) and whole-pixel offsets HarfBuzz 14.5.0
// gives, converted as ExpectedShaping.h's words are (OtShaperTestHelpers.h).
TEST(OtMyanmarShaping, MatchesHarfBuzzGlyphForGlyph) {
  FixtureFace font;
  ASSERT_TRUE(font.open());
  size_t words = 0;
  size_t matching = 0;
  for (const MyanmarShapingExpectation& word : kMyanmarShaping) {
    words++;
    ot::Plan plan;
    ASSERT_TRUE(plan.build(font.face, ot::Script::Myanmar, ot::languageTagsFor(word.language)));
    const std::vector<StoredGlyph> shaped = ottest::shapeAsStored(font.face, plan, word.utf8);
    const std::vector<StoredGlyph> expected = recorded(word);
    if (shaped == expected) {
      matching++;
    } else {
      ADD_FAILURE() << "language '" << word.language << "' " << codepointsOf(word.utf8) << "\n  shaped   "
                    << glyphText(shaped) << "\n  recorded " << glyphText(expected);
    }
  }
  std::printf("[ MyanmarShaping ] %zu of %zu recorded words match HarfBuzz\n", matching, words);
  EXPECT_EQ(matching, words);
  EXPECT_EQ(words, 37u);
}

// The filters the install writes (CPac, deviation 4) shape every recorded word as the computed ones do; the plan's two
// memos (deviation 13) leave every glyph alone.
TEST(OtMyanmarShaping, TheSameThroughCpacAndWithoutTheMemos) {
  FixtureFace computed;
  FixtureFace written;
  ASSERT_TRUE(computed.open(false));
  ASSERT_TRUE(written.open(true));
  for (const MyanmarShapingExpectation& word : kMyanmarShaping) {
    SCOPED_TRACE(std::string("language '") + word.language + "' " + codepointsOf(word.utf8));
    const uint32_t* languages = ot::languageTagsFor(word.language);
    ot::Plan a, b, bypass;
    ASSERT_TRUE(a.build(computed.face, ot::Script::Myanmar, languages));
    ASSERT_TRUE(b.build(written.face, ot::Script::Myanmar, languages));
    ASSERT_TRUE(bypass.build(written.face, ot::Script::Myanmar, languages));
    bypass.testBypassCaches = true;
    const Cps cps = ottest::utf32(word.utf8);
    const std::vector<int32_t> expected = shapeCodepoints(computed.face, a, cps);
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(shapeCodepoints(written.face, b, cps), expected);
    EXPECT_EQ(shapeCodepoints(written.face, bypass, cps), expected);
  }
}

// A word cut between two syllables, each piece shaped with the other as context: together the pieces are the whole
// word's glyphs (the Myanmar design measured the same with HarfBuzz: one neighbour syllable each side is exact).
TEST(OtMyanmarShaping, PiecesShapedWithTheirNeighboursMatchTheWholeWord) {
  FixtureFace font;
  ASSERT_TRUE(font.open());
  ot::Plan plan;
  ASSERT_TRUE(plan.build(font.face, ot::Script::Myanmar, ot::languageTagsFor("")));
  // mingalapa: ma | kinzi ga | la aa | pa tall aa; myanma: medial ra ma | na asat | ma aa.
  const Cps mingalapa = {MA, NGA, ASAT, VIRAMA, GA, LA, AA, PA, 0x102B};
  const Cps myanma = {MA, MEDIAL_RA, NA, ASAT, MA, AA};
  const struct {
    const Cps& word;
    unsigned cut;
  } cases[] = {{mingalapa, 1}, {mingalapa, 5}, {mingalapa, 7}, {myanma, 2}, {myanma, 4}};
  for (const auto& c : cases) {
    SCOPED_TRACE("cut " + std::to_string(c.cut));
    const auto rest = static_cast<unsigned>(c.word.size()) - c.cut;
    const std::vector<int32_t> whole = shapeCodepoints(font.face, plan, c.word);
    const std::vector<int32_t> first = shapeCodepoints(font.face, plan, c.word, 0, rest);
    const std::vector<int32_t> second = shapeCodepoints(font.face, plan, c.word, c.cut, 0);
    ASSERT_FALSE(whole.empty());
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    std::vector<int32_t> pieces = first;
    pieces.insert(pieces.end(), second.begin(), second.end());
    EXPECT_EQ(pieces, whole);
  }
}

// The dotted circles are inserted within the run's reservation: a run of six stackers (six broken clusters, twelve
// glyphs with their circles) under a reservation of ten is refused at the reordering pause, the buffer left as it was.
TEST(OtMyanmarShaping, DottedCirclesPastTheReservationAreRefused) {
  FixtureFace font;
  ASSERT_TRUE(font.open());
  ot::Plan plan;
  ASSERT_TRUE(plan.build(font.face, ot::Script::Myanmar, ot::languageTagsFor("")));
  const Cps stackers(6, VIRAMA);
  const Reordered fits(font.face, plan, stackers, 12);
  EXPECT_TRUE(fits.buffer.successful);
  EXPECT_EQ(fits.buffer.len(), 12u);
  const Reordered refused(font.face, plan, stackers, 10);
  EXPECT_FALSE(refused.buffer.successful);
  EXPECT_EQ(refused.buffer.len(), 6u);
  EXPECT_EQ(refused.glyphs(), nominal(font.face, stackers));
}

// A buffer for N slots, and a word of N code points that grows to 2N + 1 glyphs, the most any Noto Myanmar style grows
// (measured: dot below and medial ra, a dotted circle each and ccmp's two-glyph medial ra): the run is refused (its
// reservation, 2N + 16 slots, does not fit), nothing past the storage is written, and the buffer shapes the next
// word as HarfBuzz does; in 2N + 16 slots it fits.
TEST(OtMyanmarShaping, ARunPastTheStorageIsRefused) {
  FixtureFace font;
  ASSERT_TRUE(font.open());
  ot::Plan plan;
  ASSERT_TRUE(plan.build(font.face, ot::Script::Myanmar, ot::languageTagsFor("")));
  constexpr unsigned N = 32;
  Cps word;
  for (unsigned i = 0; i < N / 2; i++) word.insert(word.end(), {DOT_BELOW, MEDIAL_RA});
  {
    BoundBuffer roomy(256);
    ASSERT_TRUE(ottest::shapeRun(font.face, plan, word.data(), N, roomy.buffer));
    ASSERT_EQ(roomy.buffer.len(), 2 * N + 1);
  }
  const MyanmarShapingExpectation* next = findRecorded({KA, MA, VIRAMA, BHA, AA}, "");
  ASSERT_NE(next, nullptr);
  const Cps nextWord = ottest::utf32(next->utf8);

  constexpr uint8_t CANARY = 0xA5;
  constexpr size_t CANARY_BYTES = 512;
  for (const unsigned slots : {N, 2 * N + 16}) {
    SCOPED_TRACE(std::to_string(slots) + " slots");
    const size_t bytes = ot::Buffer::bytesFor(slots);
    std::vector<uint8_t> block(bytes + CANARY_BYTES, CANARY);
    ot::Buffer buffer;
    buffer.setStorage(block.data(), bytes);
    const bool fits = slots >= 2 * N + 16;
    EXPECT_EQ(ottest::shapeRun(font.face, plan, word.data(), N, buffer), fits);
    if (fits) {
      EXPECT_EQ(buffer.len(), 2 * N + 1);
    }
    for (size_t i = bytes; i < block.size(); i++) ASSERT_EQ(block[i], CANARY) << "byte " << i - bytes << " past";
    ASSERT_TRUE(ottest::shapeRun(font.face, plan, nextWord.data(), static_cast<unsigned>(nextWord.size()), buffer));
    EXPECT_EQ(ottest::storedGlyphsOf(buffer), recorded(*next));
    for (size_t i = bytes; i < block.size(); i++) ASSERT_EQ(block[i], CANARY) << "byte " << i - bytes << " past";
  }
}
