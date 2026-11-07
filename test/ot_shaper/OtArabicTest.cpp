#include <OtShaper.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ArabicExpected.h"
#include "OtShaperInternal.h"

// The Arabic plan (step 2 PR 4a): the lookups, stages and form masks lib/OtShaper plans for the Arabic fixtures
// (test/fixtures/fonts, make_arabic_fixtures.py), checked against the lookups HarfBuzz 14.5.0 plans for them.

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
    font.open("NotoNaskhArabic-Shaping-Regular.ttf");
    plan.build(font.face, ot::Script::Arabic, ot::languageTagsFor("ar"));
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
