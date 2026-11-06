#include <InflateReader.h>
#include <OtFilterWriter.h>
#include <OtShaper.h>
#include <OtUnicodeData.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "OtShaperInternal.h"

// lib/OtShaper on the .cpfont layout fonts in test/complex_shaper/data. Its
// output is checked against HarfBuzz glyph for glyph by ComplexShaperTest
// (and at scale by compare_harfbuzz.py); these tests cover the parts that
// test cannot see: the precomputed lookup filters and plans, and hostile fonts.
// The plan's memos are checked on the four Indic fixture books' words, with
// their subset fonts (test/fixtures/fonts).

namespace {

std::vector<uint8_t> readFixture(const char* name) {
  std::vector<uint8_t> bytes;
  if (FILE* f = std::fopen((std::string(SHAPING_FIXTURE_DIR) + "/" + name).c_str(), "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
  }
  return bytes;
}

// Table views of an sfnt in memory; `withFilters` false drops CPac.
ot::FaceTables tablesOf(const std::vector<uint8_t>& font, const bool withFilters = true) {
  ot::FaceTables t;
  ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &t);
  if (!withFilters) t[ot::FaceTables::CPAC] = ot::Table();
  return t;
}

std::vector<uint32_t> utf32(const char* utf8) {
  std::vector<uint32_t> out;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p) {
    uint32_t cp = *p;
    int n = cp < 0x80 ? 1 : (cp >> 5) == 6 ? 2 : (cp >> 4) == 14 ? 3 : 4;
    cp = n == 1 ? cp : n == 2 ? cp & 0x1F : n == 3 ? cp & 0x0F : cp & 0x07;
    for (int k = 1; k < n && p[k]; k++) cp = (cp << 6) | (p[k] & 0x3F);
    out.push_back(cp);
    p += n;
  }
  return out;
}

struct Fixture {
  const char* file;
  ot::Script script;
  std::vector<const char*> words;
};

const std::vector<Fixture>& fixtures() {
  static const std::vector<Fixture> all = {
      {"NotoSansBengali-Regular.layout",
       ot::Script::Bengali,
       {"শ্রীঈশ্বরচন্দ্র", "কর্ত্তৃক", "সর্ব্বোৎকৃষ্ট", "বিদ্যাসাগর", "বাক্‌শক্তি", "হৃদয়", "ক্ষি", "র্যা"}},
      {"NotoSansDevanagari-Regular.layout",
       ot::Script::Devanagari,
       {"हिन्दी", "प्रार्थना", "र्‍य", "संस्कृतम्", "क़िला", "द्ध्र्य"}},
      {"NotoSansTamil-Regular.layout", ot::Script::Tamil, {"பொன்னியின்", "ஸ்ரீ", "க்ஷ"}},
      {"NotoSansSinhala-Regular.layout", ot::Script::Sinhala, {"ශ්‍රී", "ක්‍ෂ", "කෞ", "සිංහල"}},
  };
  return all;
}

// Glyph IDs and positions of a shaped word, flattened for comparison; the
// first `before` and last `after` codepoints are context only.
std::vector<int32_t> shapeWord(const ot::Face& face, const ot::Plan& plan, const char* word, const unsigned before = 0,
                               const unsigned after = 0, const bool partial = false) {
  ot::Scale scale;
  scale.set(2133, 33, face.upem());
  ot::Buffer buffer;
  static uint8_t storage[ot::Buffer::bytesFor(256)];  // witchhunt: deviation 1
  buffer.setStorage(storage, sizeof(storage));        // witchhunt: deviation 1
  const std::vector<uint32_t> cps = utf32(word);
  std::vector<int32_t> out;
  if (!ot::shape(face, scale, plan, cps.data(), static_cast<unsigned>(cps.size()), buffer, {before, after, partial})) {
    return out;
  }
  for (unsigned i = 0; i < buffer.len(); i++) {
    out.insert(out.end(), {static_cast<int32_t>(buffer.info[i].codepoint), buffer.pos[i].xAdvance,
                           buffer.pos[i].xOffset, buffer.pos[i].yOffset});
  }
  return out;
}

TEST(OtShaperFilters, ConverterFiltersMatchWhatTheShaperComputes) {
  for (const Fixture& fx : fixtures()) {
    const std::vector<uint8_t> font = readFixture(fx.file);
    ASSERT_FALSE(font.empty()) << fx.file;
    ot::Face face;
    ASSERT_TRUE(face.init(tablesOf(font)));
    for (int t = 0; t < 2; t++) {
      const ot::FilterRecords* r = face.filters(t);
      ASSERT_NE(r, nullptr) << fx.file << " table " << t;
      uint32_t subtable = 0;
      for (uint32_t i = 0; i < face.lookupCount(t); i++) {
        const ot::Digest computed = ot::lookupDigest(face, t, i);
        const ot::Digest stored = ot::readDigest(r->lookupDigests + ot::DIGEST_BYTES * i);
        EXPECT_EQ(0, std::memcmp(computed.masks, stored.masks, sizeof(computed.masks))) << fx.file << " lookup " << i;
        // witchhunt: deviation 1 - a FixedVector over room for the lookup's subtable records.
        std::vector<uint8_t> room(ot::Face::subtableCount(face.lookup(t, i)) * ot::DIGEST_BYTES);
        ot::FixedVector<uint8_t> subtables;
        subtables.bind(room.data(), room.size());
        ot::appendSubtableDigests(face, t, i, subtables);
        EXPECT_EQ(r->firstSubtable(i), subtable);
        EXPECT_EQ(0, std::memcmp(subtables.data(), r->subtableDigests + ot::DIGEST_BYTES * subtable, subtables.size()));
        subtable += static_cast<uint32_t>(subtables.size() / ot::DIGEST_BYTES);
      }
    }
  }
}

// Counts the heap reservations of a build, or allows the first
// gPlanReservationsLeft and refuses the rest.
int gReservations = 0;
bool countReservation(size_t) {
  gReservations++;
  return true;
}
int gPlanReservationsLeft = 0;
bool refuseFilterHeap(size_t) { return gPlanReservationsLeft-- > 0; }

// Builds `plan` with every heap reservation allowed but the last two: the
// GSUB and GPOS filter blocks Plan::build() computes for a face without CPac,
// after everything a plan cannot do without. True when the plan built with
// exactly those two refused: both tables have lookups, and neither has
// subtable filters.
bool buildWithoutFilterBlocks(ot::Plan& plan, const ot::Face& face, const ot::Script script, const uint32_t* tags) {
  ot::Plan probe;
  gReservations = 0;
  ot::setHeapCheck(countReservation);
  const bool probed = probe.build(face, script, tags);
  ot::setHeapCheck(nullptr);
  if (!probed || gReservations < 2) return false;
  gPlanReservationsLeft = gReservations - 2;
  ot::setHeapCheck(refuseFilterHeap);
  const bool built = plan.build(face, script, tags);
  ot::setHeapCheck(nullptr);
  if (!built) return false;
  for (const int t : {ot::GSUB, ot::GPOS}) {
    if (plan.lookups[t].size() == 0 || plan.settings(t, 0).subtableDigests != nullptr) return false;
  }
  return true;
}

TEST(OtShaperFilters, ShapingIsTheSameWithStoredComputedOrNoFilters) {
  for (const Fixture& fx : fixtures()) {
    const std::vector<uint8_t> font = readFixture(fx.file);
    ot::Face stored, computed;
    ASSERT_TRUE(stored.init(tablesOf(font)));
    ASSERT_TRUE(computed.init(tablesOf(font, false)));
    ot::Plan withStored, withComputed, without;
    ASSERT_TRUE(withStored.build(stored, fx.script, ot::languageTagsFor("")));
    ASSERT_TRUE(withComputed.build(computed, fx.script, ot::languageTagsFor("")));
    ASSERT_TRUE(buildWithoutFilterBlocks(without, computed, fx.script, ot::languageTagsFor(""))) << fx.file;
    for (const char* word : fx.words) {
      const std::vector<int32_t> expected = shapeWord(stored, withStored, word);
      ASSERT_FALSE(expected.empty()) << word;
      EXPECT_EQ(expected, shapeWord(computed, withComputed, word)) << word;
      EXPECT_EQ(expected, shapeWord(computed, without, word)) << word;
    }
  }
}

TEST(OtShaperFilters, MismatchedFiltersAreIgnored) {
  const std::vector<uint8_t> font = readFixture("NotoSansBengali-Regular.layout");
  ot::FaceTables tables = tablesOf(font);
  const ot::Table& original = tables[ot::FaceTables::CPAC];
  std::vector<uint8_t> cpac(original.data(), original.data() + original.length());
  ASSERT_GT(cpac.size(), 5u);
  cpac[5] ^= 1;  // GSUB lookup count no longer matches the font
  tables[ot::FaceTables::CPAC] = ot::Table(cpac.data(), static_cast<uint32_t>(cpac.size()));
  ot::Face face;
  ASSERT_TRUE(face.init(tables));
  EXPECT_EQ(face.filters(ot::GSUB), nullptr);
}

void expectSamePlan(const ot::Plan& built, const ot::Plan& loaded, const std::string& where) {
  SCOPED_TRACE(where);
  EXPECT_EQ(built.script, loaded.script);
  EXPECT_EQ(built.shaper, loaded.shaper);
  EXPECT_EQ(built.chosenScript, loaded.chosenScript);
  EXPECT_EQ(built.globalMask, loaded.globalMask);
  for (int t = 0; t < 2; t++) {
    ASSERT_EQ(built.lookups[t].size(), loaded.lookups[t].size());
    for (size_t i = 0; i < built.lookups[t].size(); i++) {
      EXPECT_EQ(built.lookups[t][i].index, loaded.lookups[t][i].index);
      EXPECT_EQ(built.lookups[t][i].flags, loaded.lookups[t][i].flags);
      EXPECT_EQ(built.lookups[t][i].mask, loaded.lookups[t][i].mask);
    }
    ASSERT_EQ(built.stages[t].size(), loaded.stages[t].size());
    for (size_t i = 0; i < built.stages[t].size(); i++) {
      EXPECT_EQ(built.stages[t][i].lastLookup, loaded.stages[t][i].lastLookup);
      EXPECT_EQ(built.stages[t][i].pause, loaded.stages[t][i].pause);
    }
  }
  EXPECT_EQ(built.applyGpos, loaded.applyGpos);
  EXPECT_EQ(built.fallbackGlyphClasses, loaded.fallbackGlyphClasses);
  EXPECT_EQ(built.zeroMarks, loaded.zeroMarks);
  EXPECT_EQ(built.indicConfig, loaded.indicConfig);
  EXPECT_EQ(built.isOldSpec, loaded.isOldSpec);
  EXPECT_EQ(0, std::memcmp(built.indicMasks, loaded.indicMasks, sizeof(built.indicMasks)));
  EXPECT_EQ(0, std::memcmp(built.wouldStage, loaded.wouldStage, sizeof(built.wouldStage)));
  EXPECT_EQ(built.useRphfMask, loaded.useRphfMask);
  EXPECT_EQ(
      0, std::memcmp(built.useTopographicalMasks, loaded.useTopographicalMasks, sizeof(built.useTopographicalMasks)));
}

// The converter compiles the plans in Python (shaping_blob.py); loading one
// must give exactly the plan the shaper builds from the font, in every
// language the firmware maps and for mixed requests.
TEST(OtShaperContext, PiecesShapedWithTheirNeighboursMatchTheWholeWord) {
  const std::vector<uint8_t> font = readFixture("NotoSansBengali-Regular.layout");
  ot::Face face;
  ASSERT_TRUE(face.init(tablesOf(font)));
  ot::Plan plan;
  ASSERT_TRUE(plan.build(face, ot::Script::Bengali, ot::languageTagsFor("")));

  // তাঁ | দের: alone, the second piece's e-kar takes its word-start form.
  const std::vector<int32_t> whole = shapeWord(face, plan, "তাঁদের");
  std::vector<int32_t> pieces = shapeWord(face, plan, "তাঁদে", 0, 2);       // দে as context
  const std::vector<int32_t> tail = shapeWord(face, plan, "তাঁদের", 3, 0);  // তাঁ as context
  ASSERT_FALSE(pieces.empty());
  ASSERT_FALSE(tail.empty());
  pieces.insert(pieces.end(), tail.begin(), tail.end());
  EXPECT_EQ(pieces, whole);
  EXPECT_NE(shapeWord(face, plan, "দের"), tail) << "the e-kar should differ without context";

  // One codepoint of a syllable as context: the lone candrabindu would look
  // like a broken cluster, but gets no dotted circle, so দের stays mid-word.
  EXPECT_EQ(shapeWord(face, plan, "ঁদের", 1, 0, true), tail);

  // A context syllable broken in the whole word too keeps its dotted circle,
  // after which the next e-kar does take its word-start form.
  const std::vector<int32_t> broken = shapeWord(face, plan, "েদের");
  std::vector<int32_t> brokenPieces = shapeWord(face, plan, "েদে", 0, 2);
  const std::vector<int32_t> brokenTail = shapeWord(face, plan, "েদের", 1, 0);
  brokenPieces.insert(brokenPieces.end(), brokenTail.begin(), brokenTail.end());
  EXPECT_EQ(brokenPieces, broken);

  // Context of the whole word leaves nothing.
  EXPECT_TRUE(shapeWord(face, plan, "তাঁদের", 3, 3).empty());
}

TEST(OtPlanCompiled, LoadedPlansMatchBuiltPlans) {
  std::vector<std::vector<uint32_t>> requests = {{0}};
  for (const auto& language : ot::ucd::LANGUAGES) requests.emplace_back(language.tags, language.tags + 3);
  requests.push_back({ot::tag("MAR "), ot::tag("NEP "), 0});
  requests.push_back({ot::tag("XXXX"), ot::tag("SAN "), ot::tag("MAR ")});
  requests.push_back({ot::tag("dflt"), 0, 0});
  for (const Fixture& fx : fixtures()) {
    const std::vector<uint8_t> font = readFixture(fx.file);
    ot::Face face;
    ASSERT_TRUE(face.init(tablesOf(font)));
    ASSERT_FALSE(face.compiledPlans().empty()) << fx.file;
    for (const std::vector<uint32_t>& tags : requests) {
      const std::string where = std::string(fx.file) + " language " + std::to_string(tags[0]);
      ot::Plan built, loaded;
      ASSERT_TRUE(built.build(face, fx.script, tags.data())) << where;
      ASSERT_TRUE(loaded.load(face, fx.script, tags.data())) << where;
      expectSamePlan(built, loaded, where);
      for (const char* word : fx.words) EXPECT_EQ(shapeWord(face, built, word), shapeWord(face, loaded, word));
    }
  }
}

TEST(OtPlanCompiled, FontsWithoutPlansOrForOtherScriptsLoadNothing) {
  const std::vector<uint8_t> font = readFixture("NotoSansTamil-Regular.layout");
  ot::FaceTables tables = tablesOf(font);
  ot::Face face;
  ASSERT_TRUE(face.init(tables));
  ot::Plan plan;
  EXPECT_FALSE(plan.load(face, ot::Script::Bengali, ot::languageTagsFor("")));
  tables[ot::FaceTables::CPPL] = ot::Table();
  ASSERT_TRUE(face.init(tables));
  EXPECT_FALSE(plan.load(face, ot::Script::Tamil, ot::languageTagsFor("")));
}

// A compiled plan body in a CPpl table, for editing: the shaper byte and, per
// table, the 4-byte stage records (lastLookup u16, pause u8, reserved u8).
struct CompiledPlanEdit {
  uint8_t* shaper;
  uint8_t* stages[2];
  uint16_t stageCount[2];
};

// A copy of `compiled` with `edit` applied to every plan body.
std::vector<uint8_t> editCompiledPlans(const ot::Table& compiled, void (*edit)(const CompiledPlanEdit&)) {
  std::vector<uint8_t> bytes(compiled.data(), compiled.data() + compiled.length());
  const auto u16 = [&](const uint32_t at) { return static_cast<uint16_t>(bytes[at] << 8 | bytes[at + 1]); };
  const auto u32 = [&](const uint32_t at) { return static_cast<uint32_t>(u16(at)) << 16 | u16(at + 2); };
  constexpr uint32_t TABLES = 12 + 4 * ot::MF_COUNT + 8;  // cppl::TABLES in OtPlan.cpp
  for (uint16_t i = 0; i < u16(2); i++) {
    const uint32_t body = u32(4 + 14 * i + 10);
    CompiledPlanEdit plan{&bytes[body + 4], {}, {}};
    uint32_t at = body + TABLES;
    for (int t = 0; t < 2; t++) {
      plan.stageCount[t] = u16(at);
      plan.stages[t] = &bytes[at + 4];
      at += 4 + 4 * u16(at) + 8 * u16(at + 2);
    }
    edit(plan);
  }
  return bytes;
}

// A malformed .cpfont must not load a plan whose pauses would run a shaper's
// code without its data (Plan::indicConfig, INDIC_CONFIGS).
TEST(OtPlanCompiled, RejectsPlansInconsistentWithTheirShaper) {
  const auto loads = [](const char* file, const ot::Script script, void (*edit)(const CompiledPlanEdit&)) {
    const std::vector<uint8_t> font = readFixture(file);
    ot::FaceTables tables = tablesOf(font);
    const std::vector<uint8_t> plans = editCompiledPlans(tables[ot::FaceTables::CPPL], edit);
    tables[ot::FaceTables::CPPL] = ot::Table(plans.data(), static_cast<uint32_t>(plans.size()));
    ot::Face face;
    EXPECT_TRUE(face.init(tables)) << file;
    ot::Plan plan;
    const bool loaded = plan.load(face, script, ot::languageTagsFor(""));
    shapeWord(face, plan, "ශ්‍රී");  // a plan that failed to load is still safe to shape with
    return loaded;
  };
  const char* tamil = "NotoSansTamil-Regular.layout";
  const char* sinhala = "NotoSansSinhala-Regular.layout";
  ASSERT_TRUE(loads(tamil, ot::Script::Tamil, [](const CompiledPlanEdit&) {}));
  ASSERT_TRUE(loads(sinhala, ot::Script::Sinhala, [](const CompiledPlanEdit&) {}));

  // Indic pauses in a plan marked Default.
  EXPECT_FALSE(loads(tamil, ot::Script::Tamil, [](const CompiledPlanEdit& plan) {
    *plan.shaper = static_cast<uint8_t>(ot::ShaperKind::Default);
  }));
  // USE pauses in a plan marked Indic.
  EXPECT_FALSE(loads(tamil, ot::Script::Tamil, [](const CompiledPlanEdit& plan) {
    for (uint16_t i = 0; i < plan.stageCount[0]; i++) {
      if (plan.stages[0][4 * i + 2] != 0) plan.stages[0][4 * i + 2] = static_cast<uint8_t>(ot::Pause::UseReorder);
    }
  }));
  // A pause after a GPOS stage.
  EXPECT_FALSE(loads(tamil, ot::Script::Tamil, [](const CompiledPlanEdit& plan) {
    ASSERT_GT(plan.stageCount[1], 0);
    plan.stages[1][2] = static_cast<uint8_t>(ot::Pause::IndicSetupSyllables);
  }));
  // The Indic shaper for Sinhala, which has no Indic config, even without pauses.
  EXPECT_FALSE(loads(sinhala, ot::Script::Sinhala, [](const CompiledPlanEdit& plan) {
    *plan.shaper = static_cast<uint8_t>(ot::ShaperKind::Indic);
    for (int t = 0; t < 2; t++) {
      for (uint16_t i = 0; i < plan.stageCount[t]; i++) plan.stages[t][4 * i + 2] = 0;
    }
  }));
}

// Truncated and corrupted fonts must shape to something or fail cleanly,
// never read out of bounds (run under -fsanitize=address to check).
TEST(OtShaperRobustness, SurvivesTruncatedAndCorruptedFonts) {
  std::mt19937 rng(1757);
  for (const Fixture& fx : fixtures()) {
    const std::vector<uint8_t> original = readFixture(fx.file);
    for (int round = 0; round < 40; round++) {
      std::vector<uint8_t> font = original;
      if (round % 2 == 0) {
        font.resize(std::uniform_int_distribution<size_t>(0, font.size())(rng));
      } else {
        for (int k = 0; k < 64; k++)
          font[std::uniform_int_distribution<size_t>(0, font.size() - 1)(rng)] = rng() & 0xFF;
      }
      ot::Face face;
      if (!face.init(tablesOf(font))) continue;
      ot::Plan built, loaded;
      built.build(face, fx.script, ot::languageTagsFor(""));
      for (const char* word : fx.words) shapeWord(face, built, word);
      if (loaded.load(face, fx.script, ot::languageTagsFor(""))) {
        for (const char* word : fx.words) shapeWord(face, loaded, word);
      }
    }
  }
  SUCCEED();
}

// Big-endian bytes of a hand-built table.
struct TableBytes {
  std::vector<uint8_t> bytes;
  TableBytes& u16(const unsigned v) {
    bytes.push_back(static_cast<uint8_t>(v >> 8));
    bytes.push_back(static_cast<uint8_t>(v));
    return *this;
  }
  TableBytes& u32(const uint32_t v) { return u16(v >> 16).u16(v & 0xFFFF); }
};

// A fixture font with its GSUB replaced by `gsub`.
bool initWithGsub(ot::Face& face, const std::vector<uint8_t>& font, const std::vector<uint8_t>& gsub) {
  ot::FaceTables tables = tablesOf(font, false);
  tables[ot::FaceTables::GSUB_TABLE] = ot::Table(gsub.data(), static_cast<uint32_t>(gsub.size()));
  return face.init(tables);
}

// A GSUB with one Extension lookup of two subtables, extending `first` and
// `second` (1: SingleSubst, `singleGlyph` + 1; 8: ReverseChainSingleSubst,
// `reverseGlyph` to itself).
std::vector<uint8_t> extensionGsub(const uint16_t first, const uint16_t second, const uint16_t singleGlyph,
                                   const uint16_t reverseGlyph) {
  constexpr uint32_t EXTENSIONS = 24, SINGLE = 40, REVERSE = 52;
  TableBytes t;
  t.u16(1).u16(0).u16(0).u16(0).u16(10);  // header: v1.0, no scripts or features, lookups at 10
  t.u16(1).u16(4);                        // 10: LookupList, one lookup at 14
  t.u16(7).u16(0).u16(2).u16(EXTENSIONS - 14).u16(EXTENSIONS + 8 - 14);  // 14: Extension lookup
  for (uint32_t i = 0; i < 2; i++) {
    const uint16_t type = i == 0 ? first : second;
    t.u16(1).u16(type).u32((type == 1 ? SINGLE : REVERSE) - (EXTENSIONS + 8 * i));  // 24, 32: ExtensionFormat1
  }
  t.u16(1).u16(6).u16(1);                                   // 40: SingleSubstFormat1, delta 1
  t.u16(1).u16(1).u16(singleGlyph);                         // 46: its coverage
  t.u16(1).u16(12).u16(0).u16(0).u16(1).u16(reverseGlyph);  // 52: ReverseChainSingleSubstFormat1
  t.u16(1).u16(1).u16(reverseGlyph);                        // 64: its coverage
  return t.bytes;
}

// HarfBuzz drops an Extension lookup whose subtables extend different types.
// Applied, such a lookup could leave its glyph unmoved and repeat forever.
TEST(OtShaperRobustness, DropsExtensionLookupsThatMixSubtableTypes) {
  const std::vector<uint8_t> font = readFixture("NotoSansTamil-Regular.layout");
  const auto apply = [&](const uint16_t first, const uint16_t second, const uint16_t singleGlyph,
                         const uint16_t reverseGlyph) {
    const std::vector<uint8_t> gsub = extensionGsub(first, second, singleGlyph, reverseGlyph);
    ot::Face face;
    EXPECT_TRUE(initWithGsub(face, font, gsub));
    ot::Buffer buffer;
    uint8_t storage[ot::Buffer::bytesFor(8)];     // witchhunt: deviation 1
    buffer.setStorage(storage, sizeof(storage));  // witchhunt: deviation 1
    EXPECT_TRUE(buffer.prepare(8));
    for (int i = 0; i < 3; i++) {
      ot::GlyphInfo g{};
      g.codepoint = 5;
      g.mask = 1;
      buffer.info.push_back(g);
    }
    ot::Scale scale;
    scale.set(2133, 33, face.upem());
    ot::LookupSettings settings;
    settings.mask = 1;
    ot::applyLookup(face, scale, buffer, ot::GSUB, 0, settings);
    std::vector<uint32_t> glyphs;
    for (const ot::GlyphInfo& g : buffer.info) glyphs.push_back(g.codepoint);
    return glyphs;
  };
  EXPECT_EQ(apply(1, 1, 5, 9), (std::vector<uint32_t>{6, 6, 6}));
  // A forward lookup whose reverse chaining subtable matches, and a reverse
  // lookup whose single substitution does.
  EXPECT_EQ(apply(1, 8, 9, 5), (std::vector<uint32_t>{5, 5, 5}));
  EXPECT_EQ(apply(8, 1, 5, 9), (std::vector<uint32_t>{5, 5, 5}));
}

// A FeatureVariations condition tree whose AND nodes all share their
// children asks for 255^8 evaluations; planning must still finish.
TEST(OtShaperRobustness, BoundsFeatureVariationConditionWork) {
  constexpr uint32_t CONDITIONS = 36, AND_BYTES = 3 + 3 * 255, LEVELS = 8;
  TableBytes t;
  t.u16(1).u16(1).u16(0).u16(0).u16(0).u32(14);        // header v1.1: no scripts, features or lookups
  t.u16(1).u16(0).u32(1).u32(30 - 14).u32(0);          // 14: FeatureVariations, one record
  t.u16(1).u32(CONDITIONS - 30);                       // 30: ConditionSet, one condition
  for (uint32_t level = 0; level < LEVELS; level++) {  // 36: ConditionAnd nodes
    t.bytes.push_back(0);
    t.bytes.push_back(3);
    t.bytes.push_back(255);
    for (int i = 0; i < 255; i++) {
      t.bytes.insert(t.bytes.end(), {0, static_cast<uint8_t>(AND_BYTES >> 8), static_cast<uint8_t>(AND_BYTES)});
    }
  }
  t.u16(1).u16(0).u16(0xC000).u16(0x4000);  // axis range -1..1: holds at the default instance
  ASSERT_EQ(t.bytes.size(), CONDITIONS + LEVELS * AND_BYTES + 8);

  const std::vector<uint8_t> font = readFixture("NotoSansTamil-Regular.layout");
  ot::Face face;
  ASSERT_TRUE(initWithGsub(face, font, t.bytes));
  ot::Plan plan;
  plan.build(face, ot::Script::Tamil, ot::languageTagsFor(""));
  shapeWord(face, plan, "தமிழ்");
}

TEST(OtShaperLanguage, MapsBcp47TagsToLanguageSystems) {
  EXPECT_EQ(ot::languageTagsFor("mr")[0], ot::tag("MAR "));
  EXPECT_EQ(ot::languageTagsFor("ne-NP")[0], ot::tag("NEP "));
  EXPECT_EQ(ot::languageTagsFor("BN_bd")[0], ot::tag("BEN "));
  EXPECT_EQ(ot::languageTagsFor("xx")[0], 0u);
  EXPECT_EQ(ot::languageTagsFor("")[0], 0u);
  EXPECT_EQ(ot::languageTagsFor(nullptr)[0], 0u);
}

// --- The plan's memos, on the four Indic fixture books -------------------------

std::vector<uint8_t> readPath(const std::string& path) {
  std::vector<uint8_t> bytes;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
  }
  return bytes;
}

// A book of test/epubs, its subset font in test/fixtures/fonts, and the
// script's Unicode block.
struct Book {
  const char* font;
  const char* epub;
  ot::Script script;
  const char* language;
  uint32_t block;
};
constexpr Book BOOKS[] = {
    {"NotoSansDevanagari-Book-Regular.ttf", "test_shaped_devanagari.epub", ot::Script::Devanagari, "hi", 0x0900},
    {"NotoSansBengali-Book-Regular.ttf", "test_shaped_bengali.epub", ot::Script::Bengali, "bn", 0x0980},
    {"NotoSansTamil-Book-Regular.ttf", "test_shaped_tamil.epub", ot::Script::Tamil, "ta", 0x0B80},
    {"NotoSansMalayalam-Book-Regular.ttf", "test_shaped_malayalam.epub", ot::Script::Malayalam, "ml", 0x0D00},
};
const Book& bengaliBook() { return BOOKS[1]; }

std::vector<uint8_t> fixtureFont(const Book& book) { return readPath(std::string(TTF_FIXTURE_DIR) + "/" + book.font); }

// An entry of a ZIP whose local headers hold the sizes (stored or deflated),
// as the fixture generators write them; empty when absent or unreadable.
std::string zipEntry(const std::vector<uint8_t>& zip, const std::string& name) {
  const auto u16 = [&zip](const size_t at) { return static_cast<uint32_t>(zip[at] | zip[at + 1] << 8); };
  const auto u32 = [&u16](const size_t at) { return u16(at) | u16(at + 2) << 16; };
  size_t at = 0;
  while (at + 30 <= zip.size() && u32(at) == 0x04034B50) {
    const uint32_t method = u16(at + 8), packed = u32(at + 18), size = u32(at + 22);
    const size_t data = at + 30 + u16(at + 26) + u16(at + 28);
    if (data + packed > zip.size()) break;
    if (std::string(reinterpret_cast<const char*>(&zip[at + 30]), u16(at + 26)) == name) {
      std::string out(size, '\0');
      if (method == 0 && packed == size) {
        std::memcpy(out.data(), &zip[data], size);
        return out;
      }
      InflateReader reader;
      if (method != 8 || !reader.init(false)) return {};
      reader.setSource(&zip[data], packed);
      return reader.read(reinterpret_cast<uint8_t*>(out.data()), size) ? out : std::string();
    }
    at = data + packed;
  }
  return {};
}

bool isJoiner(const uint32_t cp) { return cp == 0x200C || cp == 0x200D; }

// The first `limit` runs a book's words shape as: per word of its chapters'
// bodies (tags dropped, so a sign in a <b> of its own stays in its word), the
// code points from the first one in U+0900-0DFF on, through those of that
// range and the joiners, at most MAX_RUN of them.
constexpr size_t MAX_RUN = 64;
std::vector<std::vector<uint32_t>> bookRuns(const Book& book, const size_t limit) {
  const std::vector<uint8_t> zip = readPath(std::string(EPUB_FIXTURE_DIR) + "/" + book.epub);
  std::vector<std::vector<uint32_t>> runs;
  for (const char* chapter : {"OEBPS/ch1.xhtml", "OEBPS/ch2.xhtml"}) {
    const std::string xhtml = zipEntry(zip, chapter);
    std::string text;
    bool inTag = false;
    for (size_t i = xhtml.find("<body"); i < xhtml.size(); i++) {
      if (xhtml[i] == '<' || xhtml[i] == '>') {
        inTag = xhtml[i] == '<';
      } else if (!inTag) {
        text += xhtml[i];
      }
    }
    size_t start = 0;
    while (start < text.size() && runs.size() < limit) {
      const size_t end = std::min(text.find_first_of(" \t\r\n", start), text.size());
      const std::vector<uint32_t> cps = utf32(text.substr(start, end - start).c_str());
      std::vector<uint32_t> run;
      for (const uint32_t cp : cps) {
        const bool indic = cp >= 0x0900 && cp <= 0x0DFF;
        if (indic || (!run.empty() && isJoiner(cp))) {
          if (run.size() < MAX_RUN) run.push_back(cp);
        } else if (!run.empty()) {
          break;
        }
      }
      if (!run.empty()) runs.push_back(run);
      start = end + 1;
    }
  }
  return runs;
}

// A run shaped as shapeWord() shapes a word: each glyph's ID, advance and
// offsets, flattened.
std::vector<int32_t> shapeRun(const ot::Face& face, const ot::Plan& plan, const std::vector<uint32_t>& run) {
  ot::Scale scale;
  scale.set(2133, 33, face.upem());
  ot::Buffer buffer;
  static uint8_t storage[ot::Buffer::bytesFor(2 * MAX_RUN + 16)];  // witchhunt: deviation 1
  buffer.setStorage(storage, sizeof(storage));                     // witchhunt: deviation 1
  std::vector<int32_t> out;
  if (!ot::shape(face, scale, plan, run.data(), static_cast<unsigned>(run.size()), buffer)) return out;
  for (unsigned i = 0; i < buffer.len(); i++) {
    out.insert(out.end(), {static_cast<int32_t>(buffer.info[i].codepoint), buffer.pos[i].xAdvance,
                           buffer.pos[i].xOffset, buffer.pos[i].yOffset});
  }
  return out;
}

// The face of a TrueType font; with `withCpac`, plus the CPac the install
// writes into a style's block (FilterTableWriter), as the device has it.
struct TtfFace {
  std::vector<uint8_t> font;
  std::vector<uint8_t> cpac;
  ot::Face face;

  bool open(std::vector<uint8_t> bytes, const bool withCpac) {
    font = std::move(bytes);
    ot::FaceTables tables;
    if (font.empty() || !ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables)) return false;
    if (!face.init(tables)) return false;
    if (!withCpac) return face.filters(ot::GSUB) == nullptr;
    ot::FilterTableWriter writer(face);
    cpac.resize(writer.bytes());
    if (cpac.empty() || writer.write(cpac.data(), static_cast<uint32_t>(cpac.size())) != cpac.size()) return false;
    tables[ot::FaceTables::CPAC] = ot::Table(cpac.data(), static_cast<uint32_t>(cpac.size()));
    return face.init(tables) && face.filters(ot::GSUB) != nullptr && face.filters(ot::GPOS) != nullptr;
  }
};

// The glyphs initial reordering asks a position for (indicPosition BASE_C:
// the consonants, and the independent vowels and digits that start there)
// that the font maps, with their code points.
struct Consonant {
  uint32_t cp;
  uint32_t glyph;
};
std::vector<Consonant> consonantsOf(const ot::Face& face, const uint32_t block) {
  std::vector<Consonant> out;
  for (uint32_t cp = block; cp < block + 0x80; cp++) {
    uint32_t glyph;
    if (ot::charData(cp).indicPosition == ot::ipos::BASE_C && face.nominalGlyph(cp, &glyph)) out.push_back({cp, glyph});
  }
  return out;
}

// The block's viramas (indicCategory H) the font maps: one in each fixture.
std::vector<uint32_t> viramasOf(const ot::Face& face, const uint32_t block) {
  std::vector<uint32_t> out;
  for (uint32_t cp = block; cp < block + 0x80; cp++) {
    uint32_t glyph;
    if (ot::charData(cp).indicCategory == ot::icat::H && face.nominalGlyph(cp, &glyph)) out.push_back(glyph);
  }
  return out;
}

// A book's font with its CPac and a plan for the book's language.
struct BookPlan {
  TtfFace font;
  ot::Plan plan;

  bool open(const Book& book, const bool withCpac = true) {
    return font.open(fixtureFont(book), withCpac) &&
           plan.build(font.face, book.script, ot::languageTagsFor(book.language));
  }
};

// Every consonant the font maps, after its virama and before it, asked twice
// in a row (the second answer is the memo's) and again once all were asked
// (over 64 pairs: some slots were taken by others meanwhile). No fixture maps
// a second virama, so the dotted circle stands in for one: the memo is keyed
// by both glyphs.
TEST(OtShaper, ConsonantPositionMemoMatchesTheWalk) {
  int notBase = 0;  // Tamil has none: its consonants take no below- or post-base forms
  for (const Book& book : BOOKS) {
    SCOPED_TRACE(book.font);
    BookPlan bp;
    ASSERT_TRUE(bp.open(book));
    const ot::Face& face = bp.font.face;
    std::vector<uint32_t> viramas = viramasOf(face, book.block);
    ASSERT_FALSE(viramas.empty());
    if (viramas.size() < 2) {
      uint32_t circle;
      ASSERT_TRUE(face.nominalGlyph(ot::DOTTED_CIRCLE, &circle));
      viramas.push_back(circle);
    }
    const std::vector<Consonant> consonants = consonantsOf(face, book.block);
    ASSERT_GE(consonants.size(), 20u);
    for (int pass = 0; pass < 2; pass++) {
      for (const Consonant& c : consonants) {
        for (const uint32_t v : viramas) {
          const uint8_t walked = ot::consonantPositionFromFace(face, bp.plan, c.glyph, v);
          for (int ask = 0; ask < 2; ask++) {
            EXPECT_EQ(bp.plan.consonantPosition(face, c.glyph, v), walked)
                << std::hex << "U+" << c.cp << std::dec << " glyph " << c.glyph << " virama " << v << " pass " << pass
                << " ask " << ask;
          }
          notBase += pass == 0 && walked != ot::ipos::BASE_C;
        }
      }
    }
  }
  EXPECT_GE(notBase, 5) << "the walks answer base everywhere: the memo was not tested";
}

// The plan's digests are the font's CPac records, and mayApply() rejects a
// lookup only when the font's digest test would: for every planned lookup,
// against the digests of the book's first 200 runs, as code points map to
// glyphs and as they shape.
TEST(OtShaper, MayApplyRejectsOnlyWhatTheDigestRejects) {
  for (const Book& book : BOOKS) {
    SCOPED_TRACE(book.font);
    BookPlan bp;
    ASSERT_TRUE(bp.open(book));
    const std::vector<std::vector<uint32_t>> runs = bookRuns(book, 200);
    ASSERT_GE(runs.size(), 100u);
    std::vector<ot::Digest> buffers;
    for (const std::vector<uint32_t>& run : runs) {
      ot::Digest mapped, shaped;
      for (const uint32_t cp : run) {
        uint32_t glyph;
        bp.font.face.nominalGlyph(cp, &glyph);
        mapped.add(glyph);
      }
      const std::vector<int32_t> glyphs = shapeRun(bp.font.face, bp.plan, run);
      ASSERT_FALSE(glyphs.empty());
      for (size_t g = 0; g < glyphs.size(); g += 4) shaped.add(static_cast<uint32_t>(glyphs[g]));
      buffers.push_back(mapped);
      buffers.push_back(shaped);
    }
    size_t tests = 0;
    size_t rejected = 0;
    for (const int t : {ot::GSUB, ot::GPOS}) {
      const ot::FilterRecords* const stored = bp.font.face.filters(t);
      ASSERT_NE(stored, nullptr);
      for (size_t i = 0; i < bp.plan.lookups[t].size(); i++) {
        const ot::Digest font = ot::readDigest(stored->lookupDigests + ot::DIGEST_BYTES * bp.plan.lookups[t][i].index);
        const ot::Digest copy = bp.plan.settings(t, i).digest;
        EXPECT_EQ(0, std::memcmp(copy.masks, font.masks, sizeof(font.masks))) << "table " << t << " lookup " << i;
        for (const ot::Digest& buffer : buffers) {
          tests++;
          if (bp.plan.mayApply(t, i, buffer)) continue;
          rejected++;
          EXPECT_FALSE(font.mayIntersect(buffer)) << "table " << t << " lookup " << i;
        }
      }
    }
    EXPECT_GT(rejected, 0u) << "of " << tests;
  }
}

// The book's first 200 runs shaped by a plan whose memos are switched off
// (Plan::testBypassCaches) and by a normal plan, on both routes: the font's
// CPac and the digests a plan computes without it.
TEST(OtShaper, TheTwoMemosLeaveEveryGlyphAlone) {
  for (const Book& book : BOOKS) {
    for (const bool withCpac : {true, false}) {
      SCOPED_TRACE(std::string(book.font) + (withCpac ? " with CPac" : " without CPac"));
      BookPlan bp;
      ASSERT_TRUE(bp.open(book, withCpac));
      ot::Plan without;
      ASSERT_TRUE(without.build(bp.font.face, book.script, ot::languageTagsFor(book.language)));
      without.testBypassCaches = true;
      const std::vector<std::vector<uint32_t>> runs = bookRuns(book, 200);
      ASSERT_GE(runs.size(), 100u);
      for (size_t r = 0; r < runs.size(); r++) {
        const std::vector<int32_t> expected = shapeRun(bp.font.face, without, runs[r]);
        ASSERT_FALSE(expected.empty()) << "run " << r;
        EXPECT_EQ(shapeRun(bp.font.face, bp.plan, runs[r]), expected) << "run " << r;
      }
    }
  }
}

// The planned GSUB lookups [first, second) of the stage a would-substitute
// feature is applied in, as the plan finds them for wouldSubstitute().
std::pair<size_t, size_t> wouldLookups(const ot::Plan& plan, const ot::Plan::WouldFeature feature) {
  const unsigned stage = plan.wouldStage[feature];
  const auto& stages = plan.stages[ot::GSUB];
  if (stage == ot::Plan::NO_STAGE || stage > stages.size()) return {0, 0};
  return {stage ? stages[stage - 1].lastLookup : 0,
          stage < stages.size() ? stages[stage].lastLookup : plan.lookups[ot::GSUB].size()};
}

// The book's font with each two-glyph format-3 context of its blwf stage
// reversed, [virama][BA] to [BA][virama]: the order of an old-spec font's
// below-base rule. HarfBuzz's would_apply tests a format-3 context's glyphs
// from the second one on and leaves the first to the lookup's digest, so to a
// plan without that digest a virama would make every consonant below-base.
// Empty when the stage has no such context.
std::vector<uint8_t> withBlwfContextsReversed(const Book& book) {
  std::vector<uint8_t> font = fixtureFont(book);
  TtfFace bare;
  ot::Plan plan;
  if (!bare.open(font, false) || !plan.build(bare.face, book.script, ot::languageTagsFor(book.language))) return {};
  const std::pair<size_t, size_t> blwf = wouldLookups(plan, ot::Plan::WS_BLWF);
  int reversed = 0;
  for (size_t i = blwf.first; i < blwf.second; i++) {
    const ot::Table lookup = bare.face.lookup(ot::GSUB, plan.lookups[ot::GSUB][i].index);
    if (lookup.u16(0) != 5) continue;  // a ContextSubst lookup
    for (uint16_t s = 0; s < ot::Face::subtableCount(lookup); s++) {
      const ot::Table st = lookup.offset16(6 + 2 * s);
      if (st.u16(0) != 3 || st.u16(2) != 2) continue;
      // Format 3: format, glyphCount, seqLookupCount, then the coverage offsets.
      const size_t coverages = static_cast<size_t>(st.data() - bare.font.data()) + 6;
      std::swap(font[coverages], font[coverages + 2]);
      std::swap(font[coverages + 1], font[coverages + 3]);
      reversed++;
    }
  }
  return reversed ? font : std::vector<uint8_t>();
}

// F1 of the HarfBuzz triage: a plan whose filter blocks were refused still
// tests a probe's first glyph against the lookup's digest, as HarfBuzz always
// does. On the fixture as it is no probe depends on that (its blwf contexts
// start with the virama, and no context's second glyph is one); with its blwf
// contexts reversed, without the digest every consonant would be below-base.
TEST(OtShaper, ABengaliPlanWithoutFiltersPlacesConsonantsAsWithThem) {
  const Book& book = bengaliBook();
  const std::vector<uint8_t> fonts[2] = {fixtureFont(book), withBlwfContextsReversed(book)};
  const char* const names[2] = {"the fixture", "its blwf contexts reversed"};
  ASSERT_FALSE(fonts[1].empty()) << "no two-glyph format-3 context in the blwf stage";
  for (int f = 0; f < 2; f++) {
    SCOPED_TRACE(names[f]);
    TtfFace stored, bare;
    ASSERT_TRUE(stored.open(fonts[f], true));
    ASSERT_TRUE(bare.open(fonts[f], false));
    const uint32_t* const tags = ot::languageTagsFor(book.language);
    ot::Plan withFilters, computed, without;
    ASSERT_TRUE(withFilters.build(stored.face, book.script, tags));
    ASSERT_TRUE(computed.build(bare.face, book.script, tags));
    ASSERT_TRUE(buildWithoutFilterBlocks(without, bare.face, book.script, tags));
    ASSERT_NE(withFilters.indicConfig, nullptr);
    uint32_t virama;
    ASSERT_TRUE(stored.face.nominalGlyph(withFilters.indicConfig->virama, &virama));

    const std::vector<Consonant> consonants = consonantsOf(stored.face, book.block);
    ASSERT_GE(consonants.size(), 20u);
    int below = 0;
    int differ = 0;
    for (const Consonant& c : consonants) {
      const unsigned expected = ot::consonantPositionFromFace(stored.face, withFilters, c.glyph, virama);
      const unsigned fromComputed = ot::consonantPositionFromFace(bare.face, computed, c.glyph, virama);
      const unsigned fromNone = ot::consonantPositionFromFace(bare.face, without, c.glyph, virama);
      EXPECT_EQ(fromComputed, expected) << std::hex << "U+" << c.cp << std::dec << " glyph " << c.glyph;
      EXPECT_EQ(fromNone, expected) << std::hex << "U+" << c.cp << std::dec << " glyph " << c.glyph
                                    << ": without filters";
      below += expected == ot::ipos::BELOW_C;
      differ += fromNone != expected;
    }
    EXPECT_GT(below, 0);
    EXPECT_LT(below, 6) << "of " << consonants.size() << " consonants";
    EXPECT_EQ(differ, 0) << "of " << consonants.size() << " consonants";
  }
}

}  // namespace
