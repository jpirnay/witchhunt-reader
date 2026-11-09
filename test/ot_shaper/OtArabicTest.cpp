#include <OtFilterWriter.h>
#include <OtShaper.h>
#include <OtUnicodeData.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../ot_shaper_port/OtShaperTestHelpers.h"
#include "ArabicExpected.h"
#include "OtShaperInternal.h"

// The Arabic shaper (step 2 PR 4a) on the Arabic fixtures (test/fixtures/fonts, make_arabic_fixtures.py): the
// lookups, stages and form masks lib/OtShaper plans, checked against the lookups HarfBuzz 14.5.0 plans for them, and
// the right-to-left runs it shapes, checked glyph for glyph against HarfBuzz's.

namespace {

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

// Where `table` starts in an sfnt held in memory; 0 when absent.
size_t tableAt(const std::vector<uint8_t>& font, const char* table) {
  for (unsigned i = 0; i < be16(font, 4); i++) {
    const size_t record = 12 + 16 * i;
    if (std::memcmp(&font[record], table, 4) == 0) return be32(font, record + 8);
  }
  return 0;
}

// Renames a script tag of GSUB's and GPOS's ScriptList (the records stay sorted for the renames used here).
void renameScript(std::vector<uint8_t>& font, const char* from, const char* to) {
  for (const char* table : {"GSUB", "GPOS"}) {
    const size_t at = tableAt(font, table);
    ASSERT_NE(at, 0u);
    const size_t scripts = at + be16(font, at + 4);
    for (unsigned i = 0; i < be16(font, scripts); i++) {
      const size_t tag = scripts + 2 + 6 * i;
      if (std::memcmp(&font[tag], from, 4) == 0) std::memcpy(&font[tag], to, 4);
    }
  }
}

// Renames a feature tag of GSUB's FeatureList.
void renameFeature(std::vector<uint8_t>& font, const char* from, const char* to) {
  const size_t at = tableAt(font, "GSUB");
  ASSERT_NE(at, 0u);
  const size_t features = at + be16(font, at + 6);
  for (unsigned i = 0; i < be16(font, features); i++) {
    const size_t tag = features + 2 + 6 * i;
    if (std::memcmp(&font[tag], from, 4) == 0) std::memcpy(&font[tag], to, 4);
  }
}

struct OpenFont {
  std::vector<uint8_t> bytes;
  ot::Face face;

  bool open(std::vector<uint8_t> data) {
    bytes = std::move(data);
    ot::FaceTables tables;
    return !bytes.empty() && ot::tablesFromSfnt(bytes.data(), static_cast<uint32_t>(bytes.size()), &tables) &&
           face.init(tables);
  }
  bool open(const char* name) { return open(readFont(name)); }
};

constexpr const char* SYNTHETIC = "ArabicRtlTest-Regular.ttf";

// A code point string through shape()'s first steps: unicode properties, normalization, the joining machine.
// The first `before` and last `after` code points are context.
struct Normalized {
  ot::Buffer buffer;
  uint8_t storage[ot::Buffer::bytesFor(64)];
  bool ok = false;

  Normalized(const ot::Face& face, const ot::Plan& plan, const std::vector<uint32_t>& cps, const bool joining,
             const unsigned before = 0, const unsigned after = 0) {
    buffer.setStorage(storage, sizeof(storage));
    if (!buffer.prepare(64)) return;
    for (size_t i = 0; i < cps.size(); i++) {
      ot::GlyphInfo g{};
      g.codepoint = cps[i];
      g.mask = plan.globalMask;
      if (i < before || i + after >= cps.size()) g.mask |= ot::CONTEXT_MASK;
      ot::setUnicodeProps(g, buffer);
      buffer.info.push_back(g);
    }
    ot::normalize(face, plan, buffer);
    if (!buffer.successful) return;
    if (joining) ot::arabicSetupMasks(plan, buffer);
    ok = true;
  }
  std::vector<uint32_t> codepoints() const {
    std::vector<uint32_t> out;
    for (unsigned i = 0; i < buffer.len(); i++) out.push_back(buffer.info[i].codepoint);
    return out;
  }
  std::vector<unsigned> actions() const {
    std::vector<unsigned> out;
    for (unsigned i = 0; i < buffer.len(); i++) out.push_back(buffer.info[i].position);
    return out;
  }
};

using Cps = std::vector<uint32_t>;
using Acts = std::vector<unsigned>;
constexpr unsigned ISOL = 0, FINA = 1, MEDI = 4, INIT = 6, NONE = 7;

}  // namespace

TEST(OtPlanArabic, PlannedLookupsMatchHarfBuzz) {
  size_t checked = 0;
  for (const ArabicPlanExpectation& expected : kArabicPlans) {
    OpenFont font;
    ASSERT_TRUE(font.open(expected.font)) << expected.font;
    ot::Plan plan;
    const std::string where = std::string(expected.font) + " lang=" + (*expected.language ? expected.language : "none");
    ASSERT_TRUE(plan.build(font.face, ot::Script::Arabic, ot::languageTagsFor(expected.language))) << where;
    EXPECT_EQ(plan.shaper, ot::ShaperKind::Arabic) << where;
    ASSERT_EQ(plan.lookups[ot::GSUB].size(), expected.gsubCount) << where;
    for (size_t i = 0; i < expected.gsubCount; i++) {
      EXPECT_EQ(plan.lookups[ot::GSUB][i].index, expected.gsub[i]) << where << " GSUB " << i;
    }
    ASSERT_EQ(plan.lookups[ot::GPOS].size(), expected.gposCount) << where;
    for (size_t i = 0; i < expected.gposCount; i++) {
      EXPECT_EQ(plan.lookups[ot::GPOS][i].index, expected.gpos[i]) << where << " GPOS " << i;
    }
    EXPECT_EQ(plan.stages[ot::GSUB].size(), expected.gsubStages) << where;
    checked++;
  }
  EXPECT_EQ(checked, sizeof(kArabicPlans) / sizeof(kArabicPlans[0]));
  EXPECT_EQ(checked, 15u);  // three fonts, five languages
}

TEST(OtPlanArabic, StagesFollowRclt) {
  OpenFont font;
  ASSERT_TRUE(font.open(SYNTHETIC));
  ot::Plan dflt;  // the dflt language system has no rclt: calt gets its pause
  ASSERT_TRUE(dflt.build(font.face, ot::Script::Arabic, ot::languageTagsFor("")));
  EXPECT_EQ(dflt.stages[ot::GSUB].size(), 14u);
  EXPECT_EQ(dflt.stages[ot::GPOS].size(), 1u);
  ot::Plan urdu;  // URD has rclt: no pause after calt, so rclt runs with liga and the common features
  ASSERT_TRUE(urdu.build(font.face, ot::Script::Arabic, ot::languageTagsFor("ur")));
  EXPECT_EQ(urdu.stages[ot::GSUB].size(), 13u);
  EXPECT_EQ(urdu.stages[ot::GPOS].size(), 1u);
  // The Noto fonts have no rclt either.
  OpenFont naskh;
  ASSERT_TRUE(naskh.open("NotoNaskhArabic-Shaping-Regular.ttf"));
  ot::Plan noto;
  ASSERT_TRUE(noto.build(naskh.face, ot::Script::Arabic, ot::languageTagsFor("ar")));
  EXPECT_EQ(noto.stages[ot::GSUB].size(), 14u);
}

namespace {

void expectFormMasks(const ot::Plan& plan, const char* where) {
  // isol, fina, fin2, fin3, medi, med2, init, none: HarfBuzz's order.
  const uint32_t isol = plan.arabicMasks[0], fina = plan.arabicMasks[1], medi = plan.arabicMasks[4],
                 init = plan.arabicMasks[6];
  for (const uint32_t mask : {isol, fina, medi, init}) {
    EXPECT_NE(mask, 0u) << where;
    EXPECT_EQ(mask & (mask - 1), 0u) << where << ": one bit";
  }
  EXPECT_NE(isol, fina) << where;
  EXPECT_NE(isol, medi) << where;
  EXPECT_NE(isol, init) << where;
  EXPECT_NE(fina, medi) << where;
  EXPECT_NE(fina, init) << where;
  EXPECT_NE(medi, init) << where;
  EXPECT_EQ(plan.arabicMasks[2], 0u) << where << " fin2";
  EXPECT_EQ(plan.arabicMasks[3], 0u) << where << " fin3";
  EXPECT_EQ(plan.arabicMasks[5], 0u) << where << " med2";
  EXPECT_EQ(plan.arabicMasks[7], 0u) << where << " none";
}

}  // namespace

TEST(OtPlanArabic, FormMasksAreHarfBuzzs) {
  OpenFont font;
  ASSERT_TRUE(font.open(SYNTHETIC));
  ot::Plan plan;
  ASSERT_TRUE(plan.build(font.face, ot::Script::Arabic, ot::languageTagsFor("")));
  expectFormMasks(plan, "synthetic");
  EXPECT_TRUE(plan.backward);
  EXPECT_EQ(plan.rtlmMask, 0u);  // the synthetic font has no rtlm

  // Noto Naskh Arabic has no isol: the mask is there all the same (the forms have a fallback).
  OpenFont naskh;
  ASSERT_TRUE(naskh.open("NotoNaskhArabic-Shaping-Regular.ttf"));
  ot::Plan noto;
  ASSERT_TRUE(noto.build(naskh.face, ot::Script::Arabic, ot::languageTagsFor("ar")));
  expectFormMasks(noto, "Naskh");

  // A font with none of the four features, the lookups left where they are: renamed away in the FeatureList.
  std::vector<uint8_t> bytes = readFont(SYNTHETIC);
  renameFeature(bytes, "isol", "xsol");
  renameFeature(bytes, "fina", "xina");
  renameFeature(bytes, "medi", "xedi");
  renameFeature(bytes, "init", "xnit");
  OpenFont bare;
  ASSERT_TRUE(bare.open(std::move(bytes)));
  ot::Plan fallback;
  ASSERT_TRUE(fallback.build(bare.face, ot::Script::Arabic, ot::languageTagsFor("")));
  expectFormMasks(fallback, "no forms");

  // rtlm is a mask bit of its own when the font has it (the subsets lack it: nothing maps U+221A): calt renamed.
  std::vector<uint8_t> withRtlm = readFont(SYNTHETIC);
  renameFeature(withRtlm, "calt", "rtlm");
  OpenFont rtlm;
  ASSERT_TRUE(rtlm.open(std::move(withRtlm)));
  ot::Plan masked;
  ASSERT_TRUE(masked.build(rtlm.face, ot::Script::Arabic, ot::languageTagsFor("")));
  EXPECT_NE(masked.rtlmMask, 0u);
  EXPECT_EQ(masked.rtlmMask & (masked.rtlmMask - 1), 0u);
  for (const uint32_t form :
       {masked.arabicMasks[0], masked.arabicMasks[1], masked.arabicMasks[4], masked.arabicMasks[6]}) {
    EXPECT_NE(form, masked.rtlmMask);
  }
}

TEST(OtPlanArabic, ChosenWhateverTheScriptTag) {
  OpenFont arab;
  ASSERT_TRUE(arab.open(SYNTHETIC));
  ot::Plan plan;
  ASSERT_TRUE(plan.build(arab.face, ot::Script::Arabic, ot::languageTagsFor("")));
  EXPECT_EQ(plan.chosenScript, ot::tag("arab"));
  EXPECT_EQ(plan.shaper, ot::ShaperKind::Arabic);

  // The same font with arab renamed: GSUB and GPOS have only DFLT for the plan to find.
  std::vector<uint8_t> bytes = readFont(SYNTHETIC);
  renameScript(bytes, "arab", "zarb");
  OpenFont dfltOnly;
  ASSERT_TRUE(dfltOnly.open(std::move(bytes)));
  ot::Plan dflt;
  ASSERT_TRUE(dflt.build(dfltOnly.face, ot::Script::Arabic, ot::languageTagsFor("")));
  EXPECT_EQ(dflt.chosenScript, ot::tag("DFLT"));
  EXPECT_EQ(dflt.shaper, ot::ShaperKind::Arabic);
  EXPECT_TRUE(dflt.backward);
}

namespace {

struct NaskhPlan {
  OpenFont font;
  ot::Plan plan;
  NaskhPlan() {
    EXPECT_TRUE(font.open("NotoNaskhArabic-Shaping-Regular.ttf"));
    EXPECT_TRUE(plan.build(font.face, ot::Script::Arabic, ot::languageTagsFor("ar")));
  }
};

Acts joinActions(const NaskhPlan& n, const Cps& cps, const unsigned before = 0, const unsigned after = 0) {
  const Normalized r(n.font.face, n.plan, cps, true, before, after);
  EXPECT_TRUE(r.ok);
  return r.actions();
}

}  // namespace

TEST(OtArabic, JoiningActionsFollowTheMachine) {
  const NaskhPlan n;
  EXPECT_EQ(joinActions(n, {0x628, 0x64A, 0x62A}), (Acts{INIT, MEDI, FINA}));
  EXPECT_EQ(joinActions(n, {0x628, 0x627}), (Acts{INIT, FINA}));
  EXPECT_EQ(joinActions(n, {0x627, 0x628}), (Acts{ISOL, ISOL}));
  EXPECT_EQ(joinActions(n, {0x628}), (Acts{ISOL}));
  EXPECT_EQ(joinActions(n, {0x628, 0x64E, 0x62A}), (Acts{INIT, NONE, FINA}));
  EXPECT_EQ(joinActions(n, {0x628, 0x200C, 0x62A}), (Acts{ISOL, NONE, ISOL}));
  // One context code point before: it takes part in the machine, and the run's first glyph is joined to it.
  EXPECT_EQ(joinActions(n, {0x628, 0x64A, 0x62A}, 1, 0), (Acts{INIT, MEDI, FINA}));
  Normalized ctx(n.font.face, n.plan, {0x628, 0x64A, 0x62A}, true, 1, 0);
  ASSERT_TRUE(ctx.ok);
  EXPECT_EQ(ctx.buffer.info[1].position, MEDI);
  EXPECT_EQ(ctx.buffer.info[2].position, FINA);
  // The mask carries the plan's bit for the action.
  EXPECT_TRUE(ctx.buffer.info[1].mask & n.plan.arabicMasks[MEDI]);
  EXPECT_FALSE(ctx.buffer.info[1].mask & n.plan.arabicMasks[FINA]);
  EXPECT_FALSE(ctx.buffer.info[0].mask & n.plan.arabicMasks[ISOL]);
}

// The brief expects "init, none" for 0628 200D. HarfBuzz joins a ZWJ (joining type C, folded into D): the machine
// gives the beh its init form and the joiner its fina action (hb-ot-shaper-arabic.cc, state 2 on jt_D).
TEST(OtArabic, ZwjJoinsLikeHarfBuzz) {
  const NaskhPlan n;
  EXPECT_EQ(joinActions(n, {0x628, 0x200D}), (Acts{INIT, FINA}));
}

TEST(OtArabic, ModifierMarksMoveToTheFront) {
  const NaskhPlan n;
  const auto run = [&](const Cps& in) {
    auto r = std::make_unique<Normalized>(n.font.face, n.plan, in, false);
    EXPECT_TRUE(r->ok);
    return r;
  };
  const auto a = run({0x628, 0x650, 0x655});
  EXPECT_EQ(a->codepoints(), (Cps{0x628, 0x655, 0x650}));
  EXPECT_EQ(a->buffer.info[1].combiningClass(), 25u);
  const auto b = run({0x628, 0x64E, 0x654});
  EXPECT_EQ(b->codepoints(), (Cps{0x628, 0x654, 0x64E}));
  EXPECT_EQ(b->buffer.info[1].combiningClass(), 26u);
  EXPECT_EQ(run({0x628, 0x64E, 0x651})->codepoints(), (Cps{0x628, 0x651, 0x64E}));
  EXPECT_EQ(run({0x628, 0x651, 0x64E})->codepoints(), (Cps{0x628, 0x651, 0x64E}));
}

TEST(OtNormalize, ArabicComposesAsUnicodeDoes) {
  const NaskhPlan n;
  const Normalized composed(n.font.face, n.plan, {0x627, 0x653}, false);
  ASSERT_TRUE(composed.ok);
  ASSERT_EQ(composed.buffer.len(), 1u);
  uint32_t glyph = 0;
  ASSERT_TRUE(n.font.face.nominalGlyph(0x622, &glyph));
  EXPECT_EQ(composed.buffer.info[0].glyphIndex, glyph);

  OpenFont synthetic;
  ASSERT_TRUE(synthetic.open(SYNTHETIC));
  ot::Plan plan;
  ASSERT_TRUE(plan.build(synthetic.face, ot::Script::Arabic, ot::languageTagsFor("")));
  const Normalized split(synthetic.face, plan, {0x622}, false);
  ASSERT_TRUE(split.ok);
  EXPECT_EQ(split.codepoints(), (Cps{0x627, 0x653}));
}

// Right to left, end to end: shape() on the recorded words.

namespace {

using ottest::BoundBuffer;
using ottest::glyphsOf;
using ottest::StoredGlyph;

// A fixture font with its lookup filters computed (the fixtures have no CPac), or read from the CPac table
// FilterTableWriter writes for it, as ShippedFamiliesTest's ShapedFace::open builds one.
struct FixtureFace {
  std::vector<uint8_t> font;
  std::vector<uint8_t> cpac;
  ot::Face face;

  bool open(const char* name, const bool withCpac) {
    font = readFont(name);
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
};

// The fixture faces and a plan per font and language, made on first use; nullptr (and a failure) when one cannot be.
class Fixtures {
 public:
  explicit Fixtures(const bool withCpac) : withCpac_(withCpac) {}

  const FixtureFace* face(const char* font) {
    std::unique_ptr<FixtureFace>& slot = faces_[font];
    if (!slot) {
      auto opened = std::make_unique<FixtureFace>();
      if (!opened->open(font, withCpac_)) {
        ADD_FAILURE() << font << (withCpac_ ? " does not open with CPac" : " does not open");
        return nullptr;
      }
      slot = std::move(opened);
    }
    return slot.get();
  }

  const ot::Plan* plan(const char* font, const char* language) {
    std::unique_ptr<ot::Plan>& slot = plans_[std::string(font) + " " + language];
    if (!slot) {
      const FixtureFace* f = face(font);
      if (!f) return nullptr;
      auto built = std::make_unique<ot::Plan>();
      if (!built->build(f->face, ot::Script::Arabic, ot::languageTagsFor(language))) {
        ADD_FAILURE() << font << ": no plan for language '" << language << "'";
        return nullptr;
      }
      slot = std::move(built);
    }
    return slot.get();
  }

 private:
  bool withCpac_;
  std::map<std::string, std::unique_ptr<FixtureFace>> faces_;  // before plans_: a plan reads its face's filters
  std::map<std::string, std::unique_ptr<ot::Plan>> plans_;
};

std::vector<StoredGlyph> recorded(const ArabicShapingExpectation& word) {
  std::vector<StoredGlyph> out;
  for (unsigned i = 0; i < word.count; i++) {
    const ExpectedGlyph& g = word.glyphs[i];
    out.push_back({g.gid, g.advance12_4, g.dx, g.dy});
  }
  return out;
}

const ArabicShapingExpectation* findRecorded(const char* font, const char* utf8, const char* language) {
  for (const ArabicShapingExpectation& word : kArabicShaping) {
    if (std::strcmp(word.font, font) == 0 && std::strcmp(word.utf8, utf8) == 0 &&
        std::strcmp(word.language, language) == 0) {
      return &word;
    }
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

}  // namespace

// Every word make_arabic_fixtures.py recorded: the glyph IDs, advances (12.4) and whole-pixel offsets HarfBuzz 14.5.0
// gives right to left, converted as ExpectedShaping.h's words are (OtShaperTestHelpers.h), in visual order.
TEST(OtArabicShaping, MatchesHarfBuzzGlyphForGlyph) {
  Fixtures fixtures(false);
  size_t words = 0;
  size_t matching = 0;
  for (const ArabicShapingExpectation& word : kArabicShaping) {
    words++;
    const FixtureFace* font = fixtures.face(word.font);
    const ot::Plan* plan = fixtures.plan(word.font, word.language);
    ASSERT_NE(font, nullptr);
    ASSERT_NE(plan, nullptr);
    const std::vector<StoredGlyph> shaped = ottest::shapeAsStored(font->face, *plan, word.utf8);
    const std::vector<StoredGlyph> expected = recorded(word);
    if (shaped == expected) {
      matching++;
    } else if (words - matching <= 10) {
      ADD_FAILURE() << word.font << " language '" << word.language << "' " << codepointsOf(word.utf8) << "\n  shaped   "
                    << glyphText(shaped) << "\n  recorded " << glyphText(expected);
    }
  }
  std::printf("[ ArabicShaping ] %zu of %zu recorded words match HarfBuzz\n", matching, words);
  EXPECT_EQ(matching, words);
  EXPECT_EQ(words, 1808u);
}

// The filters the install writes (CPac, deviation 4) shape every recorded word as the computed ones do.
TEST(OtArabicShaping, TheSameThroughCpac) {
  Fixtures computed(false);
  Fixtures written(true);
  BoundBuffer a(256);
  BoundBuffer b(256);
  std::map<std::string, size_t> wordsPerFont;
  size_t different = 0;
  for (const ArabicShapingExpectation& word : kArabicShaping) {
    const FixtureFace* computedFace = computed.face(word.font);
    const FixtureFace* writtenFace = written.face(word.font);
    const ot::Plan* computedPlan = computed.plan(word.font, word.language);
    const ot::Plan* writtenPlan = written.plan(word.font, word.language);
    ASSERT_TRUE(computedFace && writtenFace && computedPlan && writtenPlan);
    const Cps cps = ottest::utf32(word.utf8);
    const auto n = static_cast<unsigned>(cps.size());
    ASSERT_TRUE(ottest::shapeRun(computedFace->face, *computedPlan, cps.data(), n, a.buffer)) << word.utf8;
    ASSERT_TRUE(ottest::shapeRun(writtenFace->face, *writtenPlan, cps.data(), n, b.buffer)) << word.utf8;
    if (glyphsOf(a.buffer) != glyphsOf(b.buffer) && ++different <= 3) {
      ADD_FAILURE() << word.font << " language '" << word.language << "' " << codepointsOf(word.utf8);
    }
    wordsPerFont[word.font]++;
  }
  EXPECT_EQ(different, 0u);
  ASSERT_EQ(wordsPerFont.size(), 3u);
  for (const auto& [font, words] : wordsPerFont) EXPECT_GT(words, 50u) << font;
}

// A word cut after a letter that does not join to the left, each piece shaped with the other as context: together
// the pieces are the whole word's glyphs, in visual order (the word's second piece on the left). The same holds for
// a cut inside a joined sequence: the joining machine and cursive attachment run through the context.
TEST(OtArabicShaping, PiecesShapedWithTheirNeighboursMatchTheWholeWord) {
  // al-'arabiyyatu, vocalised (seven letters): cut after the reh (joining R) and its fatha, or after the beh and its
  // kasra (joined to the yeh).
  const Cps vocalised = {0x627, 0x644, 0x652, 0x639, 0x64E, 0x631, 0x64E,
                         0x628, 0x650, 0x64A, 0x651, 0x64E, 0x629, 0x64F};
  // beh beh alef | beh beh beh: cursive attachment on both sides of the alef (joining R).
  const Cps cursive = {0x628, 0x628, 0x627, 0x628, 0x628, 0x628};
  // Six behs, cut in the middle of the cursive chain.
  const Cps behs(6, 0x628);
  const struct {
    const char* font;
    const char* language;
    const Cps& word;
    unsigned cut;
    bool contextMatters;  // shaped alone, the first piece differs (a kern, a joining form, a cursive link)
  } cases[] = {
      {kFontNotoNaskhArabicShapingRegular, "", vocalised, 7, false},
      {kFontNotoSansArabicShapingBold, "", vocalised, 7, true},
      {kFontArabicRtlTestRegular, "", cursive, 3, false},
      {kFontArabicRtlTestRegular, "ur", cursive, 3, false},
      {kFontNotoNaskhArabicShapingRegular, "", vocalised, 9, true},
      {kFontNotoSansArabicShapingBold, "", vocalised, 9, true},
      {kFontArabicRtlTestRegular, "", behs, 3, true},
      {kFontArabicRtlTestRegular, "ur", behs, 3, true},
  };
  Fixtures fixtures(false);
  for (const auto& c : cases) {
    SCOPED_TRACE(std::string(c.font) + " language '" + c.language + "'");
    const FixtureFace* font = fixtures.face(c.font);
    const ot::Plan* plan = fixtures.plan(c.font, c.language);
    ASSERT_TRUE(font && plan);
    const auto rest = static_cast<unsigned>(c.word.size()) - c.cut;
    const std::vector<int32_t> whole = shapeCodepoints(font->face, *plan, c.word);
    const std::vector<int32_t> first = shapeCodepoints(font->face, *plan, c.word, 0, rest);
    const std::vector<int32_t> second = shapeCodepoints(font->face, *plan, c.word, c.cut, 0);
    ASSERT_FALSE(whole.empty());
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    std::vector<int32_t> pieces = second;
    pieces.insert(pieces.end(), first.begin(), first.end());
    EXPECT_EQ(pieces, whole);
    const Cps firstAlone(c.word.begin(), c.word.begin() + c.cut);
    if (c.contextMatters) {
      EXPECT_NE(shapeCodepoints(font->face, *plan, firstAlone), first);
    }
  }
}

// A buffer for N slots, and a word of N letters that the font's ccmp makes 2N glyphs (Noto Sans Arabic Bold splits
// the dot off each dad): the run is refused (its reservation, 2N + 16 slots, does not fit), nothing past the storage
// is written, and the buffer shapes the next word as HarfBuzz does.
TEST(OtArabicShaping, ARunPastTheStorageIsRefused) {
  FixtureFace sans;
  ASSERT_TRUE(sans.open(kFontNotoSansArabicShapingBold, false));
  ot::Plan plan;
  ASSERT_TRUE(plan.build(sans.face, ot::Script::Arabic, ot::languageTagsFor("")));
  constexpr unsigned N = 32;
  const Cps word(N, 0x636);
  {
    BoundBuffer roomy(256);
    ASSERT_TRUE(ottest::shapeRun(sans.face, plan, word.data(), N, roomy.buffer));
    ASSERT_EQ(roomy.buffer.len(), 2 * N) << "ccmp should split a dot off every dad";
  }
  const ArabicShapingExpectation* dad = findRecorded(kFontNotoSansArabicShapingBold, "\xD8\xB6", "");
  ASSERT_NE(dad, nullptr);
  const Cps shortWord = ottest::utf32(dad->utf8);

  constexpr uint8_t CANARY = 0xA5;
  constexpr size_t CANARY_BYTES = 512;
  // N slots; then the run's own reservation, 2N + 16 slots, which it fits.
  for (const unsigned slots : {N, 2 * N + 16}) {
    SCOPED_TRACE(std::to_string(slots) + " slots");
    const size_t bytes = ot::Buffer::bytesFor(slots);
    std::vector<uint8_t> block(bytes + CANARY_BYTES, CANARY);
    ot::Buffer buffer;
    buffer.setStorage(block.data(), bytes);
    const bool fits = slots >= 2 * N + 16;
    EXPECT_EQ(ottest::shapeRun(sans.face, plan, word.data(), N, buffer), fits);
    if (fits) {
      EXPECT_EQ(buffer.len(), 2 * N);
    }
    for (size_t i = bytes; i < block.size(); i++) ASSERT_EQ(block[i], CANARY) << "byte " << i - bytes << " past";
    ASSERT_TRUE(ottest::shapeRun(sans.face, plan, shortWord.data(), static_cast<unsigned>(shortWord.size()), buffer));
    EXPECT_EQ(ottest::storedGlyphsOf(buffer), recorded(*dad));
    for (size_t i = bytes; i < block.size(); i++) ASSERT_EQ(block[i], CANARY) << "byte " << i - bytes << " past";
  }
}

// The same refusal on the growth path. A word of n letters whose ccmp or calt makes three glyphs of each (U+06B3 in
// both Noto subsets) is reserved 2n + 16 slots, and prepare() takes it; the growth past that is what canGrow
// refuses, as HarfBuzz refuses a buffer past its max_len, in a buffer of exactly those slots. Nothing past the
// storage is written, the run fails, and the buffer shapes the next word as recorded.
TEST(OtArabicShaping, AGrowthPastTheReservationIsRefused) {
  constexpr unsigned N = 20;
  const Cps word(N, 0x6B3);
  for (const char* const fontName : {kFontNotoSansArabicShapingBold, kFontNotoNaskhArabicShapingRegular}) {
    SCOPED_TRACE(fontName);
    FixtureFace face;
    ASSERT_TRUE(face.open(fontName, false));
    ot::Plan plan;
    ASSERT_TRUE(plan.build(face.face, ot::Script::Arabic, ot::languageTagsFor("")));
    {
      BoundBuffer roomy(256);
      ASSERT_TRUE(ottest::shapeRun(face.face, plan, word.data(), 5, roomy.buffer));
      ASSERT_EQ(roomy.buffer.len(), 15u) << "three glyphs a letter, so N letters outgrow the reservation 2N + 16";
      static_assert(3 * N > 2 * N + 16);
    }
    const ArabicShapingExpectation* next = nullptr;
    for (const ArabicShapingExpectation& w : kArabicShaping) {
      if (std::strcmp(w.font, fontName) == 0 && *w.language == 0 && ottest::utf32(w.utf8).size() <= 8) {
        next = &w;
        break;
      }
    }
    ASSERT_NE(next, nullptr);
    const Cps nextWord = ottest::utf32(next->utf8);

    constexpr uint8_t CANARY = 0xA5;
    constexpr size_t CANARY_BYTES = 512;
    const size_t bytes = ot::Buffer::bytesFor(2 * N + 16);
    std::vector<uint8_t> block(bytes + CANARY_BYTES, CANARY);
    ot::Buffer buffer;
    buffer.setStorage(block.data(), bytes);
    EXPECT_FALSE(ottest::shapeRun(face.face, plan, word.data(), N, buffer));
    for (size_t i = bytes; i < block.size(); i++) ASSERT_EQ(block[i], CANARY) << "byte " << i - bytes << " past";
    ASSERT_TRUE(ottest::shapeRun(face.face, plan, nextWord.data(), static_cast<unsigned>(nextWord.size()), buffer));
    EXPECT_EQ(ottest::storedGlyphsOf(buffer), recorded(*next));
    for (size_t i = bytes; i < block.size(); i++) ASSERT_EQ(block[i], CANARY) << "byte " << i - bytes << " past";
  }
}

// hb_ot_rotate_chars(): in a right-to-left run, a character whose mirror the font lacks gets rtlm's mask. No character
// has a mirror here (NoCharacterTheTablesCoverHasAMirror), so every glyph carries it; the fixtures lack rtlm, so the
// synthetic font's calt is renamed to make the mask a bit of its own.
TEST(OtArabicShaping, EveryGlyphCarriesTheRtlmMask) {
  std::vector<uint8_t> bytes = readFont(SYNTHETIC);
  renameFeature(bytes, "calt", "rtlm");
  OpenFont font;
  ASSERT_TRUE(font.open(std::move(bytes)));
  ot::Plan plan;
  ASSERT_TRUE(plan.build(font.face, ot::Script::Arabic, ot::languageTagsFor("")));
  ASSERT_NE(plan.rtlmMask, 0u);
  // Letters, a mark, a joiner, a ccmp ligature (alef madda) and a space.
  const Cps word = {0x628, 0x64E, 0x628, 0x200D, 0x627, 0x653, 0x20, 0x628};
  BoundBuffer bound(256);
  ASSERT_TRUE(ottest::shapeRun(font.face, plan, word.data(), static_cast<unsigned>(word.size()), bound.buffer));
  ASSERT_GT(bound.buffer.len(), 0u);
  for (unsigned i = 0; i < bound.buffer.len(); i++) EXPECT_NE(bound.buffer.info[i].mask & plan.rtlmMask, 0u) << i;

  // A left-to-right plan sets no rtlm.
  OpenFont naskh;
  ASSERT_TRUE(naskh.open(kFontNotoNaskhArabicShapingRegular));
  ot::Plan devanagari;
  ASSERT_TRUE(devanagari.build(naskh.face, ot::Script::Devanagari, ot::languageTagsFor("")));
  EXPECT_FALSE(devanagari.backward);
  EXPECT_EQ(devanagari.rtlmMask, 0u);
}

// Why rotate_chars swaps no character for its mirror: none the Unicode tables cover is Bidi_Mirrored, so none has a
// Bidi_Mirroring_Glyph. Their ranges lie between consecutive Bidi_Mirrored characters (Unicode 16.0, UnicodeData.txt
// field 9): the Arabic and Indic blocks between U+00BB and U+0F3A, Myanmar (deviation 14) between U+0F3D and U+169B,
// Myanmar Extended-B and -A between U+301B and U+FE59; ZWNJ and ZWJ lie between U+169C and U+2039, U+25CC between
// U+232A and U+2768. Every other code point is outside the tables altogether (unassigned to the engine), and an Arabic
// run holds none: deviation 6 leaves brackets and punctuation to bidi, which mirrors them.
TEST(OtArabicShaping, NoCharacterTheTablesCoverHasAMirror) {
  constexpr struct {
    uint32_t after, before;  // two consecutive Bidi_Mirrored characters
  } GAPS[] = {{0x00BB, 0x0F3A}, {0x0F3D, 0x169B}, {0x301B, 0xFE59}};
  for (const ot::ucd::Range& range : ot::ucd::RANGES) {
    bool inAGap = false;
    for (const auto& gap : GAPS) inAGap |= range.first > gap.after && range.last < gap.before;
    EXPECT_TRUE(inAGap) << std::hex << range.first << "-" << range.last;
  }
  for (const uint32_t bracket : {0x0028u, 0x005Bu, 0x007Bu, 0x00ABu, 0x2039u, 0xFD3Eu}) {
    EXPECT_EQ(ot::charData(bracket).genCat, ot::gc::UNASSIGNED) << std::hex << bracket;
  }
}

// propagate_attachment_offsets() follows at most 64 links (HB_MAX_NESTING_LEVEL) from where its walk starts, and in
// a right-to-left buffer the walk starts at the last glyph (HarfBuzz #5514). Seventy behs on the synthetic font give
// HarfBuzz 14.5.0's y offsets (uharfbuzz 0.56.2, scale 2133): one link is 43 next to the init or the fina and 85
// between two medi forms. With the RightToLeft flag (dflt) each beh hangs off the next: every glyph's parent is
// resolved before it and the chain resolves whole. Without it (URD) each hangs off the one before: the walk from the
// last glyph runs 64 links deep, and the 65 glyphs it reaches first miss the four links past that (43 + 3 * 85).
TEST(OtArabicShaping, ALongCursiveChainResolvesAsHarfBuzzWalksIt) {
  OpenFont font;
  ASSERT_TRUE(font.open(SYNTHETIC));
  constexpr unsigned N = 70;
  const Cps word(N, 0x628);
  for (const char* language : {"", "ur"}) {
    SCOPED_TRACE(std::string("language '") + language + "'");
    ot::Plan plan;
    ASSERT_TRUE(plan.build(font.face, ot::Script::Arabic, ot::languageTagsFor(language)));
    BoundBuffer bound(256);
    ASSERT_TRUE(ottest::shapeRun(font.face, plan, word.data(), N, bound.buffer));
    const ot::Buffer& buffer = bound.buffer;
    ASSERT_EQ(buffer.len(), N);
    // In visual order: the fina (glyph 10) first, then the medi forms (9), the init (8) last.
    std::vector<int32_t> expectedY(N);
    if (!*language) {
      expectedY[0] = 0;
      for (unsigned k = 1; k < N - 1; k++) expectedY[k] = 43 + 85 * static_cast<int32_t>(k - 1);
      expectedY[N - 1] = expectedY[N - 2] + 43;
    } else {
      expectedY[N - 1] = 0;
      expectedY[N - 2] = -43;
      for (unsigned k = N - 2; k-- > 1;) expectedY[k] = expectedY[k + 1] - 85;
      expectedY[0] = expectedY[1] - 43;
      for (unsigned k = 0; k <= 64; k++) expectedY[k] += 43 + 3 * 85;
    }
    for (unsigned k = 0; k < N; k++) {
      const uint32_t gid = k == 0 ? 10 : k == N - 1 ? 8 : 9;
      const int32_t advance = k == 0 ? 1109 : k == N - 1 ? 1067 : 1066;
      const int32_t xOffset = k == 0 ? 0 : k == N - 1 ? -213 : -107;
      EXPECT_EQ(buffer.info[k].codepoint, gid) << k;
      EXPECT_EQ(buffer.pos[k].xAdvance, advance) << k;
      EXPECT_EQ(buffer.pos[k].xOffset, xOffset) << k;
      EXPECT_EQ(buffer.pos[k].yOffset, expectedY[k]) << k;
    }
  }
}

TEST(OtBuffer, ReverseSwapsGlyphsAndTheirPositions) {
  BoundBuffer bound(8);
  ot::Buffer& buffer = bound.buffer;
  ASSERT_TRUE(buffer.prepare(8));
  for (uint32_t i = 0; i < 5; i++) {
    ot::GlyphInfo g{};
    g.codepoint = 100 + i;
    buffer.info.push_back(g);
    ot::GlyphPosition p{};
    p.xAdvance = static_cast<int32_t>(10 * i);
    p.attachChain = static_cast<int16_t>(i);
    buffer.pos.push_back(p);
  }
  buffer.reverse();
  for (uint32_t i = 0; i < 5; i++) {
    EXPECT_EQ(buffer.info[i].codepoint, 104 - i);
    EXPECT_EQ(buffer.pos[i].xAdvance, static_cast<int32_t>(10 * (4 - i)));
    EXPECT_EQ(buffer.pos[i].attachChain, static_cast<int16_t>(4 - i));
  }
}
