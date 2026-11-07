#include <Memory.h>  // witchhunt: deviation 1

#include <algorithm>
#include <memory>  // witchhunt: deviation 1

#include "OtPlan.h"
#include "OtSort.h"

namespace ot {

namespace {

constexpr unsigned NOT_FOUND = 0xFFFF;
constexpr unsigned GLOBAL_BIT_SHIFT = 31;
constexpr uint32_t GLOBAL_BIT_MASK = 1u << GLOBAL_BIT_SHIFT;
constexpr unsigned MAX_BITS = 8;  // HB_OT_MAP_MAX_BITS
// Feature bits start where HarfBuzz's do (it keeps three glyph flags below
// them), so fonts run out of mask bits exactly as they would in HarfBuzz.
constexpr unsigned FIRST_FEATURE_BIT = 4;
// witchhunt: deviation 1 - the builder's scratch (PlanBuilder::bindScratch):
// the most features a shaper requests (USE asks for 47) and stages a table
// takes (Indic's GSUB has 17). Fixed and independent of the font, so the
// block asks no heap check.
constexpr size_t MAX_FEATURES = 64;
constexpr size_t MAX_STAGES = 32;

// Feature flags (hb_ot_map_feature_flags_t).
namespace ff {
constexpr uint8_t GLOBAL = 0x01;
constexpr uint8_t HAS_FALLBACK = 0x02;
constexpr uint8_t MANUAL_ZWNJ = 0x04;
constexpr uint8_t MANUAL_ZWJ = 0x08;
constexpr uint8_t MANUAL_JOINERS = MANUAL_ZWNJ | MANUAL_ZWJ;
constexpr uint8_t GLOBAL_MANUAL_JOINERS = GLOBAL | MANUAL_JOINERS;
constexpr uint8_t GLOBAL_HAS_FALLBACK = GLOBAL | HAS_FALLBACK;
constexpr uint8_t RANDOM = 0x20;
constexpr uint8_t PER_SYLLABLE = 0x40;
}  // namespace ff

unsigned bitStorage(unsigned v) {
  unsigned bits = 0;
  for (; v; v >>= 1) bits++;
  return bits;
}

uint32_t featureTag(const Table& layout, const unsigned featureIndex) {
  const Table features = layout.offset16(6);
  if (featureIndex >= features.u16(0)) return 0;
  return features.u32(2 + 6 * featureIndex);
}

// Condition tables one FeatureVariations table may have evaluated. Records
// and conditions can share subtrees, so a small table could otherwise ask for
// exponential work; HarfBuzz's sanitizer rejects such a table, and here no
// record's conditions hold once the budget is spent.
constexpr int MAX_CONDITION_EVALUATIONS = 4096;

// A FeatureVariations condition at the default instance (every normalized
// axis coordinate 0), as HarfBuzz evaluates it when no variations are set.
bool conditionHolds(const Table& condition, int& budget, const unsigned depth = 0) {
  if (depth > 8 || --budget < 0) return false;
  switch (condition.u16(0)) {
    case 1:  // axis range
      return condition.s16(4) <= 0 && 0 <= condition.s16(6);
    case 2:  // value: the default plus a variation delta, which is zero here
      return condition.s16(2) > 0;
    case 3:    // and
    case 4: {  // or
      const bool isAnd = condition.u16(0) == 3;
      for (uint8_t i = 0; i < condition.u8(2); i++) {
        const bool holds = conditionHolds(condition.at(condition.u24(3 + 3 * i)), budget, depth + 1);
        if (isAnd != holds) return holds;
      }
      return isAnd;
    }
    case 5:  // negate
      return !conditionHolds(condition.at(condition.u24(2)), budget, depth + 1);
    default:
      return false;
  }
}

// The FeatureTableSubstitution of the first FeatureVariations record whose
// conditions hold at the default instance; empty when none does.
Table defaultFeatureSubstitution(const Table& layout) {
  if (layout.u16(2) < 1) return Table();
  const Table variations = layout.offset32(10);
  int budget = MAX_CONDITION_EVALUATIONS;
  for (uint32_t i = 0; i < variations.u32(4); i++) {
    const uint32_t record = 8 + 8 * i;
    const Table set = variations.offset32(record);
    bool all = true;
    for (uint16_t c = 0; c < set.u16(0) && all; c++) all = conditionHolds(set.offset32(2 + 4 * c), budget);
    if (budget < 0) return Table();
    if (all) return variations.offset32(record + 4);
  }
  return Table();
}

// The Feature table of `featureIndex`, as substituted at the default instance.
Table featureTable(const Table& layout, const Table& substitution, const unsigned featureIndex) {
  for (uint16_t i = 0; i < substitution.u16(4); i++) {
    if (substitution.u16(6 + 6 * i) == featureIndex) return substitution.offset32(6 + 6 * i + 2);
  }
  const Table features = layout.offset16(6);
  if (featureIndex >= features.u16(0)) return Table();
  return features.offset16(2 + 6 * featureIndex + 4);
}

}  // namespace

// hb_ot_map_builder_t's feature list: features in the order the shaper
// requests them, and the GSUB/GPOS stages (runs of lookups between pauses).
class FeatureBuilder {
 public:
  struct Info {
    uint32_t tag;
    unsigned seq;  // request order, for a stable sort
    unsigned maxValue;
    uint8_t flags;
    unsigned defaultValue;
    unsigned stage[2];
  };

  FixedVector<Info> infos;       // witchhunt: deviation 1
  FixedVector<Pause> pauses[2];  // witchhunt: deviation 1
  unsigned currentStage[2] = {0, 0};
  bool isSimple = true;  // no shaper features: HarfBuzz sorts only after merging

  void add(const uint32_t t, const uint8_t flags = 0, const unsigned value = 1) {
    infos.push_back(Info{t,
                         static_cast<unsigned>(infos.size() + 1),
                         value,
                         flags,
                         (flags & ff::GLOBAL) ? value : 0,
                         {currentStage[GSUB], currentStage[GPOS]}});
  }
  void enable(const uint32_t t, const uint8_t flags = 0, const unsigned value = 1) {
    add(t, flags | ff::GLOBAL, value);
  }
  void disable(const uint32_t t) { add(t, ff::GLOBAL, 0); }
  void pause(const int table, const Pause p) {
    pauses[table].push_back(p);
    currentStage[table]++;
  }

  // Merges repeated features (the earliest stage wins; a non-global request
  // makes the feature non-global).
  void sortAndMerge() {
    if (!isSimple) {
      stableSort(infos.data(), infos.data() + infos.size(),
                 [](const Info& a, const Info& b) { return a.tag != b.tag ? a.tag < b.tag : a.seq < b.seq; });
    }
    if (infos.empty()) return;
    size_t j = 0;
    for (size_t i = 1; i < infos.size(); i++) {
      if (infos[i].tag != infos[j].tag) {
        infos[++j] = infos[i];
        continue;
      }
      if (infos[i].flags & ff::GLOBAL) {
        infos[j].flags |= ff::GLOBAL;
        infos[j].maxValue = infos[i].maxValue;
        infos[j].defaultValue = infos[i].defaultValue;
      } else {
        infos[j].flags &= ~ff::GLOBAL;
        infos[j].maxValue = std::max(infos[j].maxValue, infos[i].maxValue);
      }
      infos[j].flags |= infos[i].flags & ff::HAS_FALLBACK;
      infos[j].stage[GSUB] = std::min(infos[j].stage[GSUB], infos[i].stage[GSUB]);
      infos[j].stage[GPOS] = std::min(infos[j].stage[GPOS], infos[i].stage[GPOS]);
    }
    infos.resize(j + 1);
  }
};

namespace {

// hb_ot_shape_collect_features(), before the shaper's features. frac/numr/
// dnom and the private Harf/HARF/Buzz/BUZZ tags do nothing for Indic text;
// they are requested because HarfBuzz requests them, which fixes the stage
// and mask bit of every other feature.
// witchhunt: deviation 11 - `backward` (a right-to-left run) asks for rtla and rtlm, which the text's mask sets, in
// place of ltra and ltrm.
void collectDefaultFeatures(FeatureBuilder& b, const bool backward) {
  b.enable(tag("rvrn"));
  b.pause(GSUB, Pause::None);
  if (backward) {
    b.enable(tag("rtla"));
    b.add(tag("rtlm"));
  } else {
    b.enable(tag("ltra"));
    b.enable(tag("ltrm"));
  }
  b.add(tag("frac"));
  b.add(tag("numr"));
  b.add(tag("dnom"));
  b.enable(tag("rand"), ff::RANDOM, MAX_FEATURE_VALUE);
  b.enable(tag("Harf"));
  b.enable(tag("HARF"));
}

// The common and horizontal features, after the shaper's.
void collectCommonFeatures(FeatureBuilder& b) {
  b.enable(tag("Buzz"));
  b.enable(tag("BUZZ"));
  b.add(tag("abvm"), ff::GLOBAL);
  b.add(tag("blwm"), ff::GLOBAL);
  b.add(tag("ccmp"), ff::GLOBAL);
  b.add(tag("locl"), ff::GLOBAL);
  b.add(tag("mark"), ff::GLOBAL_MANUAL_JOINERS);
  b.add(tag("mkmk"), ff::GLOBAL_MANUAL_JOINERS);
  b.add(tag("rlig"), ff::GLOBAL);
  b.add(tag("calt"), ff::GLOBAL);
  b.add(tag("clig"), ff::GLOBAL);
  b.add(tag("curs"), ff::GLOBAL);
  b.add(tag("dist"), ff::GLOBAL);
  b.add(tag("kern"), ff::GLOBAL_HAS_FALLBACK);
  b.add(tag("liga"), ff::GLOBAL);
  b.add(tag("rclt"), ff::GLOBAL);
}

// collect_features_indic(): each basic feature in its own stage, then the
// presentation features after final reordering.
void collectIndicFeatures(FeatureBuilder& b) {
  b.pause(GSUB, Pause::IndicSetupSyllables);
  b.enable(tag("locl"), ff::PER_SYLLABLE);
  b.enable(tag("ccmp"), ff::PER_SYLLABLE);
  b.pause(GSUB, Pause::IndicInitialReordering);
  static constexpr struct {
    uint32_t tag;
    uint8_t flags;
  } BASIC[] = {
      {tag("nukt"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("akhn"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("rphf"), ff::MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("rkrf"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("pref"), ff::MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("blwf"), ff::MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("abvf"), ff::MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("half"), ff::MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("pstf"), ff::MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("vatu"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("cjct"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
  };
  for (const auto& f : BASIC) {
    b.add(f.tag, f.flags);
    b.pause(GSUB, Pause::None);
  }
  b.pause(GSUB, Pause::IndicFinalReordering);
  static constexpr struct {
    uint32_t tag;
    uint8_t flags;
  } OTHER[] = {
      {tag("init"), ff::MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("pres"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("abvs"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("blws"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("psts"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("haln"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
  };
  for (const auto& f : OTHER) b.add(f.tag, f.flags);
}

// override_features_indic(): no 'liga', and a final GSUB stage (HarfBuzz
// frees the syllable data there; no per-syllable lookup runs after it).
void overrideIndicFeatures(FeatureBuilder& b) {
  b.disable(tag("liga"));
  b.pause(GSUB, Pause::None);
}

// witchhunt: deviation 11 - arabic_features[], in the order data_create_arabic numbers the forms' masks.
constexpr uint32_t ARABIC_FORM_TAGS[7] = {tag("isol"), tag("fina"), tag("fin2"), tag("fin3"),
                                          tag("medi"), tag("med2"), tag("init")};

// witchhunt: deviation 11 - collect_features_arabic() (hb-ot-shaper-arabic.cc, HarfBuzz 14.5.0; MIT, see COPYING).
// Its three pauses that run code (record_stch, deallocate_buffer_var, arabic_fallback_shape) are Pause::None: neither
// stch nor the fallback shaping is ported, and the buffer variable needs no freeing here. `hasRclt`: whether the
// language system has rclt, which then runs with liga and the common features instead of after a pause of its own
// (HarfBuzz issue 1573).
void collectArabicFeatures(FeatureBuilder& b, const bool hasRclt) {
  b.enable(tag("stch"));
  b.pause(GSUB, Pause::None);  // record_stch
  b.enable(tag("ccmp"), ff::MANUAL_ZWJ);
  b.enable(tag("locl"), ff::MANUAL_ZWJ);
  b.pause(GSUB, Pause::None);
  // arabic_features[]: the Syriac three (fin2, fin3, med2) have no fallback.
  for (const uint32_t t : ARABIC_FORM_TAGS) {
    const bool syriac = (t & 0xFF) == '2' || (t & 0xFF) == '3';
    b.add(t, static_cast<uint8_t>(ff::MANUAL_ZWJ | (syriac ? 0 : ff::HAS_FALLBACK)));
    b.pause(GSUB, Pause::None);
  }
  b.pause(GSUB, Pause::None);  // deallocate_buffer_var
  b.enable(tag("rlig"), ff::MANUAL_ZWJ | ff::HAS_FALLBACK);
  b.pause(GSUB, Pause::None);  // arabic_fallback_shape
  b.enable(tag("calt"), ff::MANUAL_ZWJ);
  if (!hasRclt) b.pause(GSUB, Pause::None);
  b.enable(tag("liga"), ff::MANUAL_ZWJ);
  b.enable(tag("clig"), ff::MANUAL_ZWJ);
  // cswh is commented out in HarfBuzz too (off by default).
  b.enable(tag("mset"), ff::MANUAL_ZWJ);
}

// collect_features_use().
void collectUseFeatures(FeatureBuilder& b) {
  b.pause(GSUB, Pause::UseSetupSyllables);
  b.enable(tag("locl"), ff::PER_SYLLABLE);
  b.enable(tag("ccmp"), ff::PER_SYLLABLE);
  b.enable(tag("nukt"), ff::PER_SYLLABLE);
  b.enable(tag("akhn"), ff::MANUAL_ZWJ | ff::PER_SYLLABLE);
  b.pause(GSUB, Pause::UseClearSubstitutionFlags);
  b.add(tag("rphf"), ff::MANUAL_ZWJ | ff::PER_SYLLABLE);
  b.pause(GSUB, Pause::UseRecordRphf);
  b.pause(GSUB, Pause::UseClearSubstitutionFlags);
  b.enable(tag("pref"), ff::MANUAL_ZWJ | ff::PER_SYLLABLE);
  b.pause(GSUB, Pause::UseRecordPref);
  for (const uint32_t t : {tag("rkrf"), tag("abvf"), tag("blwf"), tag("half"), tag("pstf"), tag("vatu"), tag("cjct")}) {
    b.enable(t, ff::MANUAL_ZWJ | ff::PER_SYLLABLE);
  }
  b.pause(GSUB, Pause::UseReorder);
  b.pause(GSUB, Pause::None);  // HarfBuzz frees the syllable data here
  for (const uint32_t t : {tag("isol"), tag("init"), tag("medi"), tag("fina")}) b.add(t);
  b.pause(GSUB, Pause::None);
  for (const uint32_t t : {tag("abvs"), tag("blws"), tag("haln"), tag("pres"), tag("psts")}) {
    b.enable(t, ff::MANUAL_ZWJ);
  }
}

}  // namespace

// hb_ot_map_builder_t::compile() for one script and language: which
// requested features the font has, their mask bits, and each stage's lookups.
class PlanBuilder {
 public:
  PlanBuilder(Plan& plan, const Face& face) : plan_(plan), face_(face) {}

  bool build(const Script script, const uint32_t* languageTags) {
    plan_.listsFit_ = true;  // witchhunt: deviation 1
    plan_.script = script;
    plan_.backward = script == Script::Arabic;  // witchhunt: deviation 11
    for (int t = GSUB; t <= GPOS; t++) {
      TableSystem& ts = systems_[t];
      ts.ls = selectLanguageSystem(face_.layout(t), script, languageTags);
      ts.requiredIndex = ts.ls.langSys.empty() ? NOT_FOUND : ts.ls.langSys.u16(2);
      ts.requiredTag = ts.requiredIndex == NOT_FOUND ? 0 : featureTag(face_.layout(t), ts.requiredIndex);
    }
    plan_.chosenScript = systems_[GSUB].ls.chosenScript;
    chooseShaper();

    FeatureBuilder b;
    if (!bindScratch(b)) return false;          // witchhunt: deviation 1
    collectDefaultFeatures(b, plan_.backward);  // witchhunt: deviation 11
    if (plan_.shaper == ShaperKind::Indic) {
      b.isSimple = false;
      collectIndicFeatures(b);
    } else if (plan_.shaper == ShaperKind::Use) {
      b.isSimple = false;
      collectUseFeatures(b);
    } else if (plan_.shaper == ShaperKind::Arabic) {  // witchhunt: deviation 11
      b.isSimple = false;
      collectArabicFeatures(b, hasFeature(tag("rclt")));
    }
    collectCommonFeatures(b);
    if (plan_.shaper == ShaperKind::Indic) overrideIndicFeatures(b);
    b.sortAndMerge();

    unsigned requiredStage[2] = {0, 0};
    mapFeatures(b, requiredStage);
    b.pause(GSUB, Pause::None);
    b.pause(GPOS, Pause::None);
    if (!scratchFits_) return false;  // witchhunt: deviation 1 - before planLookups() reads `pauses`
    if (!planLookups(b, requiredStage)) return false;

    uint32_t masks[MF_COUNT];
    for (int i = 0; i < MF_COUNT; i++) masks[i] = oneMask(MASKED_FEATURE_TAGS[i]);
    plan_.setFeatureMasks(masks);
    // witchhunt: deviation 11 - the Arabic forms' masks in HarfBuzz's order (data_create_arabic), and rtlm's.
    for (int i = 0; i < 7; i++) plan_.arabicMasks[i] = oneMask(ARABIC_FORM_TAGS[i]);
    plan_.arabicMasks[7] = 0;
    plan_.rtlmMask = oneMask(tag("rtlm"));
    static constexpr uint32_t WOULD_TAGS[Plan::WS_COUNT] = {tag("rphf"), tag("pref"), tag("blwf"), tag("pstf"),
                                                            tag("vatu")};
    for (int i = 0; i < Plan::WS_COUNT; i++) plan_.wouldStage[i] = gsubStage(WOULD_TAGS[i]);
    if (!plan_.listsFit_) return false;  // witchhunt: deviation 1
    features_.bind(nullptr, 0);          // witchhunt: deviation 1 - freed before finish() computes the filters
    b.infos.bind(nullptr, 0);            // witchhunt: deviation 1
    b.pauses[GSUB].bind(nullptr, 0);     // witchhunt: deviation 1
    b.pauses[GPOS].bind(nullptr, 0);     // witchhunt: deviation 1
    scratch_.reset();                    // witchhunt: deviation 1
    return plan_.finish(face_);          // witchhunt: deviation 13 - false: the lookup digests' block was refused
  }

 private:
  struct TableSystem {
    LanguageSystem ls;
    unsigned requiredIndex = NOT_FOUND;
    uint32_t requiredTag = 0;
  };
  struct FeatureMap {
    uint32_t tag;
    unsigned index[2];  // feature index in GSUB and GPOS, or 0xFFFF
    uint8_t stage[2];
    uint8_t lookupFlags;
    uint8_t shift;
    uint32_t mask;
  };

  // hb_ot_shaper_categorize().
  void chooseShaper() {
    const uint32_t chosen = plan_.chosenScript;
    if (plan_.script == Script::Arabic) {
      // witchhunt: deviation 11 - the Arabic shaper whatever the script tag, DFLT included (HarfBuzz does fallback
      // shaping for Arabic text).
      plan_.shaper = ShaperKind::Arabic;
    } else if (chosen == tag("DFLT") || chosen == tag("latn")) {
      plan_.shaper = ShaperKind::Default;
    } else if (plan_.script == Script::Sinhala || (chosen & 0xFF) == '3') {
      plan_.shaper = ShaperKind::Use;
    } else {
      plan_.shaper = ShaperKind::Indic;
    }
  }

  // The language system's features of table `t`; earlier entries win.
  // witchhunt: deviation 11 - lifted out of mapFeatures() for hasFeature().
  unsigned findFeatureIndex(const int t, const uint32_t featureT) const {
    const Table& langSys = systems_[t].ls.langSys;
    for (uint16_t i = 0; i < langSys.u16(4); i++) {
      const uint16_t featureIndex = langSys.u16(6 + 2 * i);
      if (featureTag(face_.layout(t), featureIndex) == featureT) return featureIndex;
    }
    return NOT_FOUND;
  }

  // witchhunt: deviation 11 - hb_ot_map_builder_t::has_feature(): whether the language system has the feature in GSUB
  // or GPOS (the required feature does not count, as in HarfBuzz).
  bool hasFeature(const uint32_t featureT) const {
    return findFeatureIndex(GSUB, featureT) != NOT_FOUND || findFeatureIndex(GPOS, featureT) != NOT_FOUND;
  }

  // First half: which requested features the font has, and the mask bits
  // each gets.
  void mapFeatures(const FeatureBuilder& builder, unsigned* requiredStage) {
    features_.clear();  // witchhunt: deviation 1 - bound by bindScratch(), no reservation
    plan_.globalMask = GLOBAL_BIT_MASK;
    unsigned nextBit = FIRST_FEATURE_BIT;
    for (const FeatureBuilder::Info& info : builder.infos) {
      const bool usesGlobalBit = (info.flags & ff::GLOBAL) && info.maxValue == 1;
      const unsigned bitsNeeded = usesGlobalBit ? 0 : std::min(MAX_BITS, bitStorage(info.maxValue));
      if (!info.maxValue || nextBit + bitsNeeded >= GLOBAL_BIT_SHIFT) continue;  // disabled, or out of bits
      FeatureMap m{};
      bool found = false;
      for (int t = GSUB; t <= GPOS; t++) {
        if (systems_[t].requiredTag == info.tag) requiredStage[t] = info.stage[t];
        m.index[t] = findFeatureIndex(t, info.tag);
        found |= m.index[t] != NOT_FOUND;
      }
      if (!found && !(info.flags & ff::HAS_FALLBACK)) continue;
      m.tag = info.tag;
      m.stage[GSUB] = static_cast<uint8_t>(info.stage[GSUB]);
      m.stage[GPOS] = static_cast<uint8_t>(info.stage[GPOS]);
      m.lookupFlags = static_cast<uint8_t>((info.flags & ff::MANUAL_ZWNJ ? 0 : lookupbits::AUTO_ZWNJ) |
                                           (info.flags & ff::MANUAL_ZWJ ? 0 : lookupbits::AUTO_ZWJ) |
                                           (info.flags & ff::RANDOM ? lookupbits::RANDOM : 0) |
                                           (info.flags & ff::PER_SYLLABLE ? lookupbits::PER_SYLLABLE : 0));
      if (usesGlobalBit) {
        m.shift = GLOBAL_BIT_SHIFT;
        m.mask = GLOBAL_BIT_MASK;
      } else {
        m.shift = static_cast<uint8_t>(nextBit);
        m.mask = (1u << (nextBit + bitsNeeded)) - (1u << nextBit);
        nextBit += bitsNeeded;
        plan_.globalMask |= (info.defaultValue << m.shift) & m.mask;
      }
      features_.push_back(m);
    }
    if (builder.isSimple) {
      stableSort(features_.data(), features_.data() + features_.size(),
                 [](const FeatureMap& a, const FeatureMap& b) { return a.tag < b.tag; });
    }
  }

  // Second half: each stage's lookups, sorted by index with duplicates merged.
  bool planLookups(const FeatureBuilder& builder, const unsigned* requiredStage) {
    for (int t = GSUB; t <= GPOS; t++) {
      const Table& layout = face_.layout(t);
      const Table substitution = defaultFeatureSubstitution(layout);
      const uint16_t lookupCount = face_.lookupCount(t);
      const auto featureLookups = [&](const unsigned featureIndex) {
        return featureIndex == NOT_FOUND ? Table() : featureTable(layout, substitution, featureIndex);
      };

      // witchhunt: deviation 1 - room in one block (allocateLists()) for every
      // lookup the features list, so the list never grows.
      size_t total = featureLookups(systems_[t].requiredIndex).u16(2);
      for (const FeatureMap& m : features_) total += featureLookups(m.index[t]).u16(2);
      if (total > MAX_PLANNED_LOOKUPS) return false;
      if (!heapAvailable(total * sizeof(PlannedLookup) + builder.currentStage[t] * sizeof(Stage))) return false;
      if (!plan_.allocateLists(t, total, builder.currentStage[t])) return false;  // witchhunt: deviation 1
      FixedVector<PlannedLookup>& list = plan_.lookups[t];                        // witchhunt: deviation 1
      list.clear();
      plan_.stages[t].clear();

      const auto addLookups = [&](const unsigned featureIndex, const uint32_t mask, const uint8_t flags) {
        const Table feature = featureLookups(featureIndex);
        for (uint16_t i = 0; i < feature.u16(2); i++) {
          const uint16_t index = feature.u16(4 + 2 * i);
          if (index < lookupCount) list.push_back(PlannedLookup{index, flags, mask});
        }
      };

      size_t last = 0;
      for (unsigned stage = 0; stage < builder.currentStage[t]; stage++) {
        if (systems_[t].requiredIndex != NOT_FOUND && requiredStage[t] == stage) {
          addLookups(systems_[t].requiredIndex, GLOBAL_BIT_MASK, lookupbits::AUTO_ZWNJ | lookupbits::AUTO_ZWJ);
        }
        for (const FeatureMap& m : features_) {
          if (m.stage[t] == stage) addLookups(m.index[t], m.mask, m.lookupFlags);
        }
        if (last + 1 < list.size()) {
          stableSort(list.data() + last, list.data() + list.size(),
                     [](const PlannedLookup& a, const PlannedLookup& b) { return a.index < b.index; });
          // A lookup in several features: masks unite, auto-joiner flags must
          // all agree, random and per-syllable keep the first.
          size_t j = last;
          for (size_t i = j + 1; i < list.size(); i++) {
            if (list[i].index != list[j].index) {
              list[++j] = list[i];
              continue;
            }
            list[j].mask |= list[i].mask;
            const uint8_t joiners = lookupbits::AUTO_ZWNJ | lookupbits::AUTO_ZWJ;
            list[j].flags =
                static_cast<uint8_t>((list[j].flags & ~joiners) | (list[j].flags & list[i].flags & joiners));
          }
          list.resize(j + 1);
        }
        last = list.size();
        plan_.stages[t].push_back(Stage{static_cast<uint16_t>(last), builder.pauses[t][stage]});
      }
    }
    return true;
  }

  // Bit value of `t` in glyph masks; 0 when the font lacks the feature.
  uint32_t oneMask(const uint32_t t) const {
    for (const FeatureMap& f : features_) {
      if (f.tag == t) return (1u << f.shift) & f.mask;
    }
    return 0;
  }

  // The GSUB stage `t` is applied in; Plan::NO_STAGE when the font lacks it.
  uint8_t gsubStage(const uint32_t t) const {
    uint8_t stage = Plan::NO_STAGE;
    for (const FeatureMap& f : features_) {
      if (f.tag == t) stage = f.stage[GSUB];
    }
    return stage;
  }

  // witchhunt: deviation 1 - the scratch, one block for the build, freed with
  // the builder: `b`'s infos and pauses and features_, bound empty with
  // scratchFits_ as their flag. False when the block is refused.
  bool bindScratch(FeatureBuilder& b) {
    static_assert(alignof(FeatureMap) <= alignof(FeatureBuilder::Info));
    static_assert(sizeof(FeatureBuilder::Info) % alignof(FeatureMap) == 0);
    scratch_ = makeUniqueNoThrow<uint8_t[]>(SCRATCH_BYTES);
    if (!scratch_) return false;
    b.infos.bind(reinterpret_cast<FeatureBuilder::Info*>(scratch_.get()), MAX_FEATURES, &scratchFits_);
    features_.bind(reinterpret_cast<FeatureMap*>(scratch_.get() + FEATURES_AT), MAX_FEATURES, &scratchFits_);
    auto* const pauses = reinterpret_cast<Pause*>(scratch_.get() + PAUSES_AT);
    for (int t = GSUB; t <= GPOS; t++) b.pauses[t].bind(pauses + t * MAX_STAGES, MAX_STAGES, &scratchFits_);
    return true;
  }

  Plan& plan_;
  const Face& face_;
  TableSystem systems_[2];
  FixedVector<FeatureMap> features_;  // witchhunt: deviation 1

  // witchhunt: deviation 1 - the scratch's layout (about 3.1 KB), its block
  // and its flag.
  static constexpr size_t FEATURES_AT = MAX_FEATURES * sizeof(FeatureBuilder::Info);
  static constexpr size_t PAUSES_AT = FEATURES_AT + MAX_FEATURES * sizeof(FeatureMap);
  static constexpr size_t SCRATCH_BYTES = PAUSES_AT + 2 * MAX_STAGES * sizeof(Pause);
  std::unique_ptr<uint8_t[]> scratch_;
  bool scratchFits_ = true;
};

bool Plan::build(const Face& face, const Script script, const uint32_t* languageTags) {
  if (PlanBuilder(*this, face).build(script, languageTags)) return true;
  *this = Plan();  // safe to shape with: no lookups, no shaper
  return false;
}

}  // namespace ot
