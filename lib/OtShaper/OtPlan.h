#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>  // witchhunt: deviation 1 - in place of <vector>

#include "OtBuffer.h"
#include "OtCategories.h"
#include "OtFace.h"
#include "OtFixedVector.h"  // witchhunt: deviation 1
#include "OtLayout.h"

// How one font shapes one script in one language: which lookups run, in
// which stages, with which masks, and which shaper reorders between them.
// Mirrors HarfBuzz's hb_ot_map_builder_t / hb_ot_shape_plan_t.
namespace ot {

// witchhunt: deviation 11 - Arabic last: a compiled plan (CPpl) names a shaper by this value, and Plan::read() rejects
// everything above Use. witchhunt: deviation 14 - Myanmar after it.
enum class ShaperKind : uint8_t { Default, Indic, Use, Arabic, Myanmar };

// Work done between GSUB stages (HarfBuzz pause callbacks).
enum class Pause : uint8_t {
  None,
  IndicSetupSyllables,
  IndicInitialReordering,
  IndicFinalReordering,
  UseSetupSyllables,
  UseClearSubstitutionFlags,
  UseRecordRphf,
  UseRecordPref,
  UseReorder,
  // witchhunt: deviation 14 - after the CPpl format's pauses (cppl::MAX_PAUSE), which no compiled plan may name.
  MyanmarSetupSyllables,
  MyanmarReorder,
};

// Indic scripts shaped here, in Unicode block order from U+0900, and (deviation 11) Arabic after them, so the Indic
// values, which index tables, are unchanged; (deviation 14) Myanmar after Arabic, so a CPpl's values keep their
// meaning.
enum class Script : uint8_t {
  Devanagari,
  Bengali,
  Gurmukhi,
  Gujarati,
  Oriya,
  Tamil,
  Telugu,
  Kannada,
  Malayalam,
  Sinhala,
  Arabic,  // witchhunt: deviation 11
  Myanmar  // witchhunt: deviation 14
};

// witchhunt: deviation 4 - whether a GSUB or GPOS script tag is one a plan
// tries for a script above (OtPlan.cpp's SCRIPT_TAGS, old and new tags): the
// install writes a CPac only for a font whose layout names one.
bool isShapedScriptTag(uint32_t scriptTag);

// Most lookups a plan takes per table; fonts with more are not shaped.
constexpr size_t MAX_PLANNED_LOOKUPS = 4096;

struct PlannedLookup {
  uint16_t index;
  uint8_t flags;  // lookupbits::*
  uint32_t mask;
};

namespace lookupbits {
constexpr uint8_t AUTO_ZWNJ = 0x01;     // skip ZWNJ when matching context
constexpr uint8_t AUTO_ZWJ = 0x02;      // skip ZWJ when matching input
constexpr uint8_t RANDOM = 0x04;        // 'rand': pick alternates at random
constexpr uint8_t PER_SYLLABLE = 0x08;  // match within one syllable
}  // namespace lookupbits

struct Stage {
  uint16_t lastLookup;  // lookups before this index belong to this or earlier stages
  Pause pause;          // run after the stage's lookups
};

class PlanBuilder;

// Features whose mask bit the shapers set on glyphs themselves, in the order
// compiled plans store their masks (docs/file-formats.md, CPpl).
enum MaskedFeature {
  MF_RPHF,
  MF_PREF,
  MF_BLWF,
  MF_ABVF,
  MF_HALF,
  MF_PSTF,
  MF_INIT,
  MF_ISOL,
  MF_MEDI,
  MF_FINA,
  MF_COUNT
};
constexpr uint32_t MASKED_FEATURE_TAGS[MF_COUNT] = {tag("rphf"), tag("pref"), tag("blwf"), tag("abvf"), tag("half"),
                                                    tag("pstf"), tag("init"), tag("isol"), tag("medi"), tag("fina")};

class Plan {
 public:
  // Plans `script` from the font's GSUB and GPOS (OtPlanBuild.cpp).
  // `languageTags` are the OpenType language systems to try, in order
  // (zero-terminated, up to three). False when the font has more lookups
  // than a plan takes or the heap check refuses the plan's memory; a plan
  // that failed to build or load is empty, and shapes nothing.
  // witchhunt: deviation 1 - also false when the builder's scratch or a list
  // block is refused, and (deviation 13) when the lookup digests' block is; a
  // refused filter block leaves only its table's subtables unfiltered.
  bool build(const Face& face, Script script, const uint32_t* languageTags);
  // The same plan, read from the font's CPpl table, where the .cpfont
  // converter stored it. False when the font has none for this script and
  // language, or it is malformed.
  // witchhunt: deviation 1 - also false when a list block or (deviation 13)
  // the lookup digests' block is refused.
  bool load(const Face& face, Script script, const uint32_t* languageTags);

  // Lookup settings (flags, mask, filters) of planned lookup `i` of `table`.
  LookupSettings settings(int table, size_t i) const;
  // witchhunt: deviation 13 - whether planned lookup `i` of `table` may start
  // at a glyph the buffer holds (`buffer` is its digest): the digest test of
  // HarfBuzz's apply loop, on the plan's own copy of the lookup's digest, so
  // a lookup it rejects costs no read of the font.
  bool mayApply(int table, size_t i, const Digest& buffer) const;
  // witchhunt: deviation 16 - whether a glyph of `buffer` passes planned
  // lookup `i` of `table`'s digest and carries its mask, both the plan's: the
  // first two of applyLookup()'s tests of a start glyph, so a lookup no glyph
  // passes them is skipped before anything of the font is read for it.
  bool mayStart(int table, size_t i, const Buffer& buffer) const;
  // witchhunt: deviation 13 - consonantPositionFromFace() of the glyphs
  // `consonant` and `virama`, remembered: the walk runs on a miss only.
  uint8_t consonantPosition(const Face& face, uint32_t consonant, uint32_t virama) const;
  // Heap the plan holds, for memory statistics.
  size_t memoryBytes() const;

  Script script = Script::Devanagari;
  ShaperKind shaper = ShaperKind::Default;
  uint32_t chosenScript = 0;  // the GSUB script tag the plan follows
  uint32_t globalMask = 0;
  FixedVector<PlannedLookup> lookups[2];  // witchhunt: deviation 1
  FixedVector<Stage> stages[2];           // witchhunt: deviation 1
  bool applyGpos = false;
  bool fallbackGlyphClasses = false;  // no GDEF classes: guess them from Unicode
  bool zeroMarks = false;             // shaper zeroes mark advances
  bool adjustMarkPositioningWhenZeroing = false;

  // Indic shaper.
  const IndicConfig* indicConfig = nullptr;
  bool isOldSpec = false;  // the font uses v1 script tags (deva, beng, ...)
  enum IndicMask { RPHF, PREF, BLWF, ABVF, HALF, PSTF, INIT, INDIC_MASK_COUNT };
  uint32_t indicMasks[INDIC_MASK_COUNT] = {};
  enum WouldFeature { WS_RPHF, WS_PREF, WS_BLWF, WS_PSTF, WS_VATU, WS_COUNT };
  static constexpr uint8_t NO_STAGE = 0xFF;
  // GSUB stage of each would-substitute feature; NO_STAGE when the font lacks it.
  uint8_t wouldStage[WS_COUNT] = {NO_STAGE, NO_STAGE, NO_STAGE, NO_STAGE, NO_STAGE};
  // Whether the lookups of the stage of `feature` would substitute `glyphs`.
  bool wouldSubstitute(const Face& face, WouldFeature feature, const uint32_t* glyphs, unsigned count) const;

  // witchhunt: deviation 11 - the Arabic shaper: the masks of the seven forms in HarfBuzz's order (isol, fina, fin2,
  // fin3, medi, med2, init; index 7 is "none" and stays 0), 0 for a form the font lacks and has no fallback for
  // (fin2, fin3, med2); the mask of rtlm, which the text sets; and whether the plan runs right to left (every
  // Arabic run does, so the script decides). They are kept out of MaskedFeature, whose order is the CPpl format's.
  uint32_t arabicMasks[8] = {};
  uint32_t rtlmMask = 0;
  bool backward = false;

  // Universal Shaping Engine.
  uint32_t useRphfMask = 0;
  uint32_t useTopographicalMasks[4] = {};  // isol, init, medi, fina

  // witchhunt: deviation 13 - test use only. True switches off both caches:
  // the consonant memo (consonantPosition() walks on every call) and the
  // digest pre-filter (mayApply() lets every lookup through), so a test can
  // shape with and without them. False in every plan the firmware makes.
  // witchhunt: deviation 16 - and the start-glyph pre-filter (mayStart()).
  // witchhunt: deviation 18 - and the subtable groups (settings() gives none).
  bool testBypassCaches = false;

 private:
  friend class PlanBuilder;

  // The planned GSUB lookups [start, end) of the stage a feature is applied in.
  struct WouldSubstituteLookups {
    uint16_t start = 0;
    uint16_t end = 0;
  };

  // load() until the plan is complete; false when anything is missing or malformed.
  bool read(const Face& face, Script script, const uint32_t* languageTags);
  // `masks` in MaskedFeature order, as the builder computes them.
  void setFeatureMasks(const uint32_t* masks);
  // Everything that follows from the lookups, stages and masks: GPOS and
  // mark handling, lookup filters and the shapers' data.
  // witchhunt: deviation 13 - false when the lookup digests' block is refused.
  bool finish(const Face& face);
  bool copyLookupDigests(const Face& face);  // witchhunt: deviation 13
  void buildFilters(const Face& face);
  void fillSubtableGroups(const Face& face);  // witchhunt: deviation 18
  void collectWould(WouldFeature feature);
  const uint8_t* digestRecord(int table, size_t i) const;  // witchhunt: deviation 13 - in lookupDigests_

  bool zeroContext_ = false;
  WouldSubstituteLookups would_[WS_COUNT];
  // Subtable filters: the font's CPac records, or computed here (per planned
  // lookup where its subtable digests start, and the subtable digests).
  const FilterRecords* fontFilters_[2] = {nullptr, nullptr};
  FixedVector<uint32_t> subtableStarts_[2];  // witchhunt: deviation 1
  FixedVector<uint8_t> subtableDigests_[2];  // witchhunt: deviation 1
  // witchhunt: deviation 13 - every planned lookup's digest, GSUB's then
  // GPOS's in lookups[] order: copied from the font's CPac, else computed.
  // mayApply(), settings() and wouldSubstitute() read these, never the font's
  // records, and a plan cannot do without them (the block is refused: the plan
  // fails), so a would-substitute probe always has the digest HarfBuzz tests
  // its first glyph against.
  FixedVector<uint8_t> lookupDigests_;
  // witchhunt: deviation 18 - the subtable groups (LookupSettings::subtableGroups) of every planned lookup of more than
  // SUBTABLE_GROUP subtables, in the digest block after the digests: their masks, 8 B a group, and per planned lookup
  // (GSUB's then GPOS's) the index of its first group, NO_GROUPS for a lookup without. Filled from the subtable
  // records once buildFilters() has them; a table without records has no groups.
  static constexpr uint16_t NO_GROUPS = 0xFFFF;
  FixedVector<uint8_t> groupMasks_;
  FixedVector<uint16_t> groupStarts_;

  // witchhunt: deviation 13 - consonantPosition()'s memo, direct-mapped: one
  // slot per (consonant ^ virama) % POSITION_SLOTS, keyed by the two glyph IDs
  // (not code points: locl and ccmp run before initial reordering). It
  // remembers a pure function of the plan and the font, so the output cannot
  // change; upstream HarfBuzz (to 15.1.0 and main) walks every time. mutable:
  // a const plan fills it while it shapes. A plan is used by one section build
  // at a time, single-threaded under RenderLock. finish() empties it.
  static constexpr size_t POSITION_SLOTS = 64;
  mutable uint32_t positionKeys_[POSITION_SLOTS] = {};  // consonant | virama << 16
  mutable uint8_t positions_[POSITION_SLOTS] = {};      // 0 (ipos::START, never an answer): empty

  // witchhunt: deviation 1 - the arrays above live in blocks the plan owns,
  // each from makeUniqueNoThrow right after the heap check: per table one for
  // the lists and one for the computed filters, and one for the lookup
  // digests (deviation 13). Each allocate*() frees the table's old block
  // first, and binds its arrays empty (false: bound to nothing). A plan is
  // move-only, and `*this = Plan()` frees the blocks. A flag cleared by a
  // refused growth is checked once the arrays are filled; build() and load()
  // bind them afresh, so a moved plan's flags never matter.
  bool allocateLists(int table, size_t lookupCount, size_t stageCount);       // binds lookups[table], stages[table]
  bool allocateFilters(int table, size_t lookupCount, size_t subtableCount);  // binds the two filter arrays
  std::unique_ptr<uint8_t[]> listBlock_[2];
  std::unique_ptr<uint8_t[]> filterBlock_[2];
  std::unique_ptr<uint8_t[]> digestBlock_;  // witchhunt: deviation 13 - lookupDigests_
  bool listsFit_ = true;                    // cleared by a refused growth of lookups[] or stages[]
  bool filtersFit_ = true;                  // cleared by a refused growth of a filter array; allocateFilters() sets it
};

// The script and language system a plan follows in one layout table
// (hb_ot_layout_table_select_script, hb_ot_layout_script_select_language).
struct LanguageSystem {
  uint32_t chosenScript = 0;
  Table langSys;
  // Identifies the choice within the font: the matched LangSys tag, 'dflt',
  // or 0 for the script's default LangSys. Compiled plans are keyed by it.
  uint32_t key = 0;
};
LanguageSystem selectLanguageSystem(const Table& layout, Script script, const uint32_t* languageTags);

}  // namespace ot
