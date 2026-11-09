#include "OtPlan.h"

#include <Memory.h>  // witchhunt: deviation 1

#include <cstring>  // witchhunt: deviation 13

#include "OtShaperInternal.h"  // witchhunt: deviation 13 - consonantPositionFromFace()

namespace ot {

namespace {

constexpr unsigned NOT_FOUND = 0xFFFF;

// OpenType script tags tried in order: Indic v3 (shaped by USE), v2, v1.
// Sinhala has one, and (witchhunt: deviation 11) Arabic: HarfBuzz tries only arab, then DFLT, dflt, latn.
// witchhunt: deviation 14 - Myanmar has no mym3 (hb_ot_all_tags_from_script): mym2, then mymr.
constexpr uint32_t SCRIPT_TAGS[][3] = {
    {tag("dev3"), tag("dev2"), tag("deva")},
    {tag("bng3"), tag("bng2"), tag("beng")},
    {tag("gur3"), tag("gur2"), tag("guru")},
    {tag("gjr3"), tag("gjr2"), tag("gujr")},
    {tag("ory3"), tag("ory2"), tag("orya")},
    {tag("tml3"), tag("tml2"), tag("taml")},
    {tag("tel3"), tag("tel2"), tag("telu")},
    {tag("knd3"), tag("knd2"), tag("knda")},
    {tag("mlm3"), tag("mlm2"), tag("mlym")},
    {tag("sinh"), 0, 0},
    {tag("arab"), 0, 0},
    {tag("mym2"), tag("mymr"), 0},  // witchhunt: deviation 14
};

// Binary search in a RecordList (count, then tag + offset records), as
// HarfBuzz's bfind; NOT_FOUND when absent.
unsigned findRecord(const Table& list, const uint32_t t) {
  int lo = 0;
  int hi = static_cast<int>(list.u16(0)) - 1;
  while (lo <= hi) {
    const int mid = static_cast<int>((static_cast<unsigned>(lo) + static_cast<unsigned>(hi)) / 2);
    const uint32_t k = list.u32(2 + 6 * mid);
    if (t < k) {
      hi = mid - 1;
    } else if (t > k) {
      lo = mid + 1;
    } else {
      return static_cast<unsigned>(mid);
    }
  }
  return NOT_FOUND;
}

// hb_ot_layout_table_select_script(): the requested tags, then DFLT, dflt,
// latn.
Table selectScript(const Table& layout, const uint32_t* tags, uint32_t* chosen) {
  const Table scripts = layout.offset16(4);
  unsigned index = NOT_FOUND;
  *chosen = 0;
  for (unsigned i = 0; i < 3 && index == NOT_FOUND; i++) {
    if (tags[i] && (index = findRecord(scripts, tags[i])) != NOT_FOUND) *chosen = tags[i];
  }
  for (const uint32_t fallback : {tag("DFLT"), tag("dflt"), tag("latn")}) {
    if (index == NOT_FOUND && (index = findRecord(scripts, fallback)) != NOT_FOUND) *chosen = fallback;
  }
  if (index == NOT_FOUND || index >= scripts.u16(0)) return Table();
  return scripts.offset16(2 + 6 * index + 4);
}

// hb_ot_layout_script_select_language(): the requested language systems,
// then dflt, then the script's default LangSys.
Table selectLangSys(const Table& script, const uint32_t* languageTags, uint32_t* key) {
  const Table records = script.at(2);  // langSysCount + records, a RecordList
  unsigned index = NOT_FOUND;
  *key = 0;
  for (unsigned i = 0; i < 3 && languageTags && languageTags[i] && index == NOT_FOUND; i++) {
    if ((index = findRecord(records, languageTags[i])) != NOT_FOUND) *key = languageTags[i];
  }
  if (index == NOT_FOUND && (index = findRecord(records, tag("dflt"))) != NOT_FOUND) *key = tag("dflt");
  if (index == NOT_FOUND) return script.offset16(0);
  if (index >= script.u16(2)) return Table();
  return script.offset16(4 + 6 * index + 4);
}

// A compiled plan (docs/file-formats.md, CPpl).
namespace cppl {
constexpr uint16_t VERSION = 1;
constexpr uint32_t HEADER_BYTES = 4;
constexpr uint32_t RECORD_BYTES = 14;  // script u8, reserved u8, GSUB key u32, GPOS key u32, offset u32
constexpr uint32_t MASKS = 12;         // after chosenScript u32, shaper u8, reserved u8[3], globalMask u32
constexpr uint32_t WOULD_STAGES = MASKS + 4 * MF_COUNT;
constexpr uint32_t TABLES = WOULD_STAGES + 8;  // wouldStage u8[5], reserved u8[3]
constexpr uint32_t STAGE_BYTES = 4;            // lastLookup u16, pause u8, reserved u8
constexpr uint32_t LOOKUP_BYTES = 8;           // index u16, flags u8, reserved u8, mask u32
constexpr uint8_t MAX_PAUSE = static_cast<uint8_t>(Pause::UseReorder);

// Whether a stage may run `pause`: as the planner emits them, Indic pauses
// only in Indic plans and USE pauses only in USE plans, both after GSUB
// stages. Other pauses would run a shaper's code without its data.
// witchhunt: deviation 14 - the Myanmar pauses lie above MAX_PAUSE: no CPpl
// names the Myanmar shaper (read() rejects every shaper above Use).
bool pauseAllowed(const ShaperKind shaper, const int table, const uint8_t pause) {
  if (pause == static_cast<uint8_t>(Pause::None)) return true;
  if (table != GSUB || pause > MAX_PAUSE) return false;
  return pause <= static_cast<uint8_t>(Pause::IndicFinalReordering) ? shaper == ShaperKind::Indic
                                                                    : shaper == ShaperKind::Use;
}
}  // namespace cppl

}  // namespace

LanguageSystem selectLanguageSystem(const Table& layout, const Script script, const uint32_t* languageTags) {
  LanguageSystem ls;
  const Table scriptTable = selectScript(layout, SCRIPT_TAGS[static_cast<unsigned>(script)], &ls.chosenScript);
  ls.langSys = selectLangSys(scriptTable, languageTags, &ls.key);
  return ls;
}

bool isShapedScriptTag(const uint32_t scriptTag) {  // witchhunt: deviation 4
  if (scriptTag == 0) return false;                 // witchhunt: deviation 4 - SCRIPT_TAGS pads with 0
  for (const auto& tags : SCRIPT_TAGS) {            // witchhunt: deviation 4
    for (const uint32_t t : tags) {                 // witchhunt: deviation 4
      if (t == scriptTag) return true;              // witchhunt: deviation 4
    }  // witchhunt: deviation 4
  }  // witchhunt: deviation 4
  return false;  // witchhunt: deviation 4
}  // witchhunt: deviation 4

bool Plan::load(const Face& face, const Script s, const uint32_t* languageTags) {
  if (read(face, s, languageTags)) return true;
  *this = Plan();  // safe to shape with: no lookups, no shaper
  return false;
}

bool Plan::read(const Face& face, const Script s, const uint32_t* languageTags) {
  listsFit_ = true;  // witchhunt: deviation 1
  const Table& compiled = face.compiledPlans();
  if (compiled.u16(0) != cppl::VERSION) return false;
  const uint32_t gsubKey = selectLanguageSystem(face.layout(GSUB), s, languageTags).key;
  const uint32_t gposKey = selectLanguageSystem(face.layout(GPOS), s, languageTags).key;
  Table body;
  for (uint16_t i = 0; i < compiled.u16(2) && body.empty(); i++) {
    const uint32_t record = cppl::HEADER_BYTES + cppl::RECORD_BYTES * i;
    if (compiled.u8(record) == static_cast<uint8_t>(s) && compiled.u32(record + 2) == gsubKey &&
        compiled.u32(record + 6) == gposKey) {
      body = compiled.at(compiled.u32(record + 10));
    }
  }
  if (body.empty() || body.u8(4) > static_cast<uint8_t>(ShaperKind::Use)) return false;
  // INDIC_CONFIGS has no entry for Sinhala, which shapes with USE.
  if (body.u8(4) == static_cast<uint8_t>(ShaperKind::Indic) &&
      static_cast<unsigned>(s) >= sizeof(INDIC_CONFIGS) / sizeof(INDIC_CONFIGS[0])) {
    return false;
  }

  script = s;
  chosenScript = body.u32(0);
  shaper = static_cast<ShaperKind>(body.u8(4));
  globalMask = body.u32(8);
  uint32_t masks[MF_COUNT];
  for (int i = 0; i < MF_COUNT; i++) masks[i] = body.u32(cppl::MASKS + 4 * i);
  setFeatureMasks(masks);
  for (int i = 0; i < WS_COUNT; i++) wouldStage[i] = body.u8(cppl::WOULD_STAGES + i);

  uint32_t at = cppl::TABLES;
  for (int t = GSUB; t <= GPOS; t++) {
    const uint16_t stageCount = body.u16(at);
    const uint16_t lookupCount = body.u16(at + 2);
    at += 4;
    const uint32_t bytes = stageCount * cppl::STAGE_BYTES + lookupCount * cppl::LOOKUP_BYTES;
    if (lookupCount > MAX_PLANNED_LOOKUPS || !body.has(at, bytes)) return false;
    if (!heapAvailable(lookupCount * sizeof(PlannedLookup) + stageCount * sizeof(Stage))) return false;
    stages[t].clear();
    if (!allocateLists(t, lookupCount, stageCount)) return false;  // witchhunt: deviation 1 - both lists
    for (uint16_t i = 0; i < stageCount; i++, at += cppl::STAGE_BYTES) {
      const Stage stage{body.u16(at), static_cast<Pause>(body.u8(at + 2))};
      const uint16_t previous = stages[t].empty() ? 0 : stages[t].back().lastLookup;
      if (stage.lastLookup < previous || stage.lastLookup > lookupCount ||
          !cppl::pauseAllowed(shaper, t, body.u8(at + 2))) {
        return false;
      }
      stages[t].push_back(stage);
    }
    lookups[t].clear();
    for (uint16_t i = 0; i < lookupCount; i++, at += cppl::LOOKUP_BYTES) {
      const uint16_t index = body.u16(at);
      if (index >= face.lookupCount(t)) return false;
      lookups[t].push_back(PlannedLookup{index, body.u8(at + 2), body.u32(at + 4)});
    }
  }
  if (!listsFit_) return false;  // witchhunt: deviation 1
  return finish(face);           // witchhunt: deviation 13 - false: the lookup digests' block was refused
}

void Plan::setFeatureMasks(const uint32_t* masks) {
  for (int i = 0; i < INDIC_MASK_COUNT; i++) indicMasks[i] = masks[MF_RPHF + i];
  useRphfMask = masks[MF_RPHF];
  static constexpr MaskedFeature TOPOGRAPHICAL[4] = {MF_ISOL, MF_INIT, MF_MEDI, MF_FINA};
  for (int i = 0; i < 4; i++) {
    const uint32_t mask = masks[TOPOGRAPHICAL[i]];
    useTopographicalMasks[i] = mask == globalMask ? 0 : mask;
  }
}

bool Plan::finish(const Face& face) {
  applyGpos = !face.layout(GPOS).empty();
  fallbackGlyphClasses = !face.hasGlyphClasses();
  zeroMarks = shaper != ShaperKind::Indic;
  adjustMarkPositioningWhenZeroing = !applyGpos;
  if (!copyLookupDigests(face)) return false;  // witchhunt: deviation 13
  buildFilters(face);
  fillSubtableGroups(face);                        // witchhunt: deviation 18
  std::memset(positions_, 0, sizeof(positions_));  // witchhunt: deviation 13 - the memo, empty
  if (shaper == ShaperKind::Indic) {
    indicConfig = &INDIC_CONFIGS[static_cast<unsigned>(script)];
    isOldSpec = (chosenScript & 0xFF) != '2';
    // would_substitute() ignores context in new-spec fonts, except Malayalam
    // (as Uniscribe does, per HarfBuzz's data_create_indic()).
    zeroContext_ = !isOldSpec && script != Script::Malayalam;
    for (int feature = 0; feature < WS_COUNT; feature++) collectWould(static_cast<WouldFeature>(feature));
  }
  return true;
}

// witchhunt: deviation 13 - each planned lookup's digest into the plan's own
// block, GSUB's then GPOS's: the font's CPac record, else computed as the
// CPac's writers compute it (lookupDigest()). Every planned lookup has one:
// its index is below the font's lookup count, and a CPac holds a record for
// each. False when the heap check or the allocation refuses the block, which
// the plan cannot do without.
//
// witchhunt: deviation 18 - the block also holds the subtable groups' masks and the planned lookups' first groups,
// sized here from the lookups' subtable counts (no more than NO_GROUPS groups) and filled by fillSubtableGroups().
// witchhunt: deviation 19 - and the LigatureSubst lookups' second masks with the GSUB lookups' slots, filled here.
bool Plan::copyLookupDigests(const Face& face) {
  digestBlock_.reset();
  lookupDigests_.bind(nullptr, 0);
  groupMasks_.bind(nullptr, 0);     // witchhunt: deviation 18
  groupStarts_.bind(nullptr, 0);    // witchhunt: deviation 18
  secondMasks_.bind(nullptr, 0);    // witchhunt: deviation 19
  secondSlots_.bind(nullptr, 0);    // witchhunt: deviation 19
  lookupOffsets_.bind(nullptr, 0);  // witchhunt: deviation 20
  const size_t planned = lookups[GSUB].size() + lookups[GPOS].size();
  const size_t bytes = planned * DIGEST_BYTES;
  if (bytes == 0) return true;
  size_t groups = 0;  // witchhunt: deviation 18
  for (int t = GSUB; t <= GPOS; t++) {
    for (const PlannedLookup& lookup : lookups[t]) {
      const unsigned subtables = Face::subtableCount(face.lookup(t, lookup.index));
      if (subtables > SUBTABLE_GROUP) groups += (subtables + SUBTABLE_GROUP - 1) / SUBTABLE_GROUP;
    }
  }
  if (groups >= NO_GROUPS) groups = 0;                              // witchhunt: deviation 18 - past its index: none
  const size_t groupBytes = groups ? groups * 8 + planned * 2 : 0;  // witchhunt: deviation 18
  // witchhunt: deviation 19 - a slot of second mask per planned LigatureSubst lookup, at most 255 of them.
  size_t ligatures = 0;
  for (const PlannedLookup& lookup : lookups[GSUB]) ligatures += isLigatureLookup(face, lookup.index);
  if (ligatures > 255) ligatures = 255;
  const size_t secondBytes = ligatures * 8;
  const size_t slotBytes = ligatures ? lookups[GSUB].size() : 0;
  // witchhunt: deviation 20 - the lookup offsets last, aligned for their uint16_t.
  const size_t offsetsAt = (bytes + secondBytes + groupBytes + slotBytes + 1) & ~static_cast<size_t>(1);
  const size_t total = offsetsAt + planned * sizeof(uint16_t);
  if (!heapAvailable(total)) return false;
  digestBlock_ = makeUniqueNoThrow<uint8_t[]>(total);
  if (!digestBlock_) return false;
  lookupDigests_.bind(digestBlock_.get(), bytes);
  lookupDigests_.resize(bytes);
  lookupOffsets_.bind(reinterpret_cast<uint16_t*>(digestBlock_.get() + offsetsAt), planned);  // witchhunt: deviation 20
  for (int t = GSUB; t <= GPOS; t++) {                                                        // witchhunt: deviation 20
    const Table list = face.layout(t).offset16(8);
    for (const PlannedLookup& lookup : lookups[t]) {
      const Table found = face.lookup(t, lookup.index);
      lookupOffsets_.push_back(found.empty() ? 0 : static_cast<uint16_t>(found.data() - list.data()));
    }
  }
  // witchhunt: deviation 19 - each LigatureSubst lookup's second mask, while slots last.
  if (ligatures) {
    secondMasks_.bind(digestBlock_.get() + bytes, secondBytes);
    secondSlots_.bind(digestBlock_.get() + bytes + secondBytes + groupBytes, slotBytes);
    for (const PlannedLookup& lookup : lookups[GSUB]) {
      uint8_t slot = 0;
      if (isLigatureLookup(face, lookup.index) && secondMasks_.resize(secondMasks_.size() + 8)) {
        writeLowMask(ligatureSeconds(face, lookup.index), secondMasks_.data() + secondMasks_.size() - 8);
        slot = static_cast<uint8_t>(secondMasks_.size() / 8);
      }
      secondSlots_.push_back(slot);
    }
  }
  // witchhunt: deviation 18 - the groups after the digests: the masks (8 B each, so the starts stay aligned), then
  // the starts.
  if (groups) {
    groupMasks_.bind(digestBlock_.get() + bytes + secondBytes, groups * 8);
    groupStarts_.bind(reinterpret_cast<uint16_t*>(digestBlock_.get() + bytes + secondBytes + groups * 8), planned);
  }
  uint8_t* record = lookupDigests_.data();
  for (int t = GSUB; t <= GPOS; t++) {
    const FilterRecords* const stored = face.filters(t);
    for (const PlannedLookup& lookup : lookups[t]) {
      if (stored) {
        std::memcpy(record, stored->lookupDigests + DIGEST_BYTES * lookup.index, DIGEST_BYTES);
      } else {
        writeDigest(ot::lookupDigest(face, t, lookup.index), record);
      }
      record += DIGEST_BYTES;
    }
  }
  return true;
}

// witchhunt: deviation 13 - planned lookup `i` of `table`'s record in lookupDigests_.
OT_HOT const uint8_t* Plan::digestRecord(const int table, const size_t i) const {
  return lookupDigests_.data() + DIGEST_BYTES * (table == GSUB ? i : lookups[GSUB].size() + i);
}

// witchhunt: deviation 18 - each planned lookup of more than SUBTABLE_GROUP subtables, when its table has subtable
// records, gets the low masks of each SUBTABLE_GROUP of them ORed: applyLookupSubtables() reads a group's records only
// when its mask may hold the glyph. NotoSansDevanagari's pres/cjct lookup is 114 contexts of one first glyph each,
// whose records (2.7 KB of CPac in flash) a glyph it may start at read in full; over a Hindi chapter the lines of CPac
// read per word fall from 68 to 38.
void Plan::fillSubtableGroups(const Face& face) {
  if (groupStarts_.capacity() == 0) return;
  groupMasks_.clear();
  groupStarts_.clear();
  size_t k = 0;
  for (int t = GSUB; t <= GPOS; t++) {
    for (size_t i = 0; i < lookups[t].size(); i++, k++) {
      groupStarts_.push_back(NO_GROUPS);
      const unsigned subtables = Face::subtableCount(face.lookup(t, lookups[t][i].index));
      const uint8_t* const records = settings(t, i).subtableDigests;
      if (subtables <= SUBTABLE_GROUP || !records) continue;
      const unsigned groups = (subtables + SUBTABLE_GROUP - 1) / SUBTABLE_GROUP;
      const size_t first = groupMasks_.size() / 8;
      if (!groupMasks_.resize(groupMasks_.size() + groups * 8)) continue;  // past the block: no groups
      groupStarts_[k] = static_cast<uint16_t>(first);
      for (unsigned g = 0; g < groups; g++) {
        uint64_t mask = 0;
        for (unsigned s = g * SUBTABLE_GROUP; s < subtables && s < (g + 1) * SUBTABLE_GROUP; s++) {
          mask |= digestRecordLowMask(records + DIGEST_BYTES * s);
        }
        writeLowMask(mask, groupMasks_.data() + 8 * (first + g));
      }
    }
  }
}

// Subtable filters: from the font's CPac table when it has one, else computed
// on the heap when the heap check allows (else subtables run unfiltered).
void Plan::buildFilters(const Face& face) {
  for (int t = GSUB; t <= GPOS; t++) {
    allocateFilters(t, 0, 0);  // witchhunt: deviation 1 - frees the table's computed filters (upstream clear()s them)
    fontFilters_[t] = face.filters(t);
    if (fontFilters_[t]) continue;
    // 64-bit: planned lookups may share one lookup of 65535 subtables.
    uint64_t subtables = 0;
    for (const PlannedLookup& lookup : lookups[t]) subtables += Face::subtableCount(face.lookup(t, lookup.index));
    // witchhunt: deviation 13 - no lookup digests: the plan holds them (copyLookupDigests()).
    const uint64_t bytes = subtables * DIGEST_BYTES + lookups[t].size() * sizeof(uint32_t);
    if (bytes > UINT32_MAX || !heapAvailable(static_cast<size_t>(bytes))) continue;
    if (!allocateFilters(t, lookups[t].size(), static_cast<size_t>(subtables))) continue;  // witchhunt: deviation 1
    for (size_t i = 0; i < lookups[t].size(); i++) {
      subtableStarts_[t].push_back(static_cast<uint32_t>(subtableDigests_[t].size() / DIGEST_BYTES));
      appendSubtableDigests(face, t, lookups[t][i].index, subtableDigests_[t]);
    }
    if (!filtersFit_) allocateFilters(t, 0, 0);  // witchhunt: deviation 1 - a refused growth: unfiltered
  }
}

// witchhunt: deviation 1 - one block for a table's lists, `lookupCount`
// planned lookups then `stageCount` stages, bound empty with listsFit_ as
// their flag. False when the block is refused, with nothing bound.
bool Plan::allocateLists(const int table, const size_t lookupCount, const size_t stageCount) {
  static_assert(alignof(Stage) <= alignof(PlannedLookup) && sizeof(PlannedLookup) % alignof(Stage) == 0);
  listBlock_[table].reset();
  lookups[table].bind(nullptr, 0, &listsFit_);
  stages[table].bind(nullptr, 0, &listsFit_);
  const size_t bytes = lookupCount * sizeof(PlannedLookup) + stageCount * sizeof(Stage);
  if (bytes == 0) return true;
  listBlock_[table] = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!listBlock_[table]) return false;
  auto* const lookupStart = reinterpret_cast<PlannedLookup*>(listBlock_[table].get());
  lookups[table].bind(lookupStart, lookupCount, &listsFit_);
  stages[table].bind(reinterpret_cast<Stage*>(lookupStart + lookupCount), stageCount, &listsFit_);
  return true;
}

// witchhunt: deviation 1 - one block for a table's computed filters, for
// `lookupCount` planned lookups of `subtableCount` subtables in all: the
// subtable starts, then the subtable digests, bound empty with filtersFit_ as
// their flag. False, with nothing bound, when there is nothing to filter (no
// lookups: it only frees) or the block is refused.
bool Plan::allocateFilters(const int table, const size_t lookupCount, const size_t subtableCount) {
  filterBlock_[table].reset();
  subtableStarts_[table].bind(nullptr, 0);
  subtableDigests_[table].bind(nullptr, 0);
  if (lookupCount == 0) return false;
  const size_t startBytes = lookupCount * sizeof(uint32_t);
  const size_t subtableBytes = subtableCount * DIGEST_BYTES;
  filterBlock_[table] = makeUniqueNoThrow<uint8_t[]>(startBytes + subtableBytes);
  if (!filterBlock_[table]) return false;
  uint8_t* const block = filterBlock_[table].get();
  filtersFit_ = true;
  subtableStarts_[table].bind(reinterpret_cast<uint32_t*>(block), lookupCount, &filtersFit_);
  subtableDigests_[table].bind(block + startBytes, subtableBytes, &filtersFit_);
  return true;
}

OT_HOT LookupSettings Plan::settings(const int table, const size_t i, const bool resolve) const {
  const PlannedLookup& lookup = lookups[table][i];
  LookupSettings out;
  out.mask = lookup.mask;
  out.autoZwnj = lookup.flags & lookupbits::AUTO_ZWNJ;
  out.autoZwj = lookup.flags & lookupbits::AUTO_ZWJ;
  out.random = lookup.flags & lookupbits::RANDOM;
  out.perSyllable = lookup.flags & lookupbits::PER_SYLLABLE;
  readDigest(digestRecord(table, i), out.digest);  // witchhunt: deviation 13 - the plan's copy
  if (const FilterRecords* r = fontFilters_[table]) {
    if (resolve) {
      out.subtableDigests = r->subtableDigests + DIGEST_BYTES * r->firstSubtable(lookup.index);
    } else {
      out.fontRecords = r;  // witchhunt: deviation 4
    }
  } else if (!subtableStarts_[table].empty()) {  // witchhunt: deviation 13 - the filter block holds subtables only
    out.subtableDigests = subtableDigests_[table].data() + DIGEST_BYTES * subtableStarts_[table][i];
  }
  // witchhunt: deviation 20 - where the lookup is.
  const size_t planned = table == GSUB ? i : lookups[GSUB].size() + i;
  if (!testBypassCaches && planned < lookupOffsets_.size()) out.lookupOffset = lookupOffsets_[planned];
  // witchhunt: deviation 19 - a LigatureSubst lookup's second mask.
  if (table == GSUB && !testBypassCaches && i < secondSlots_.size() && secondSlots_[i]) {
    out.secondMask = readLowMask(secondMasks_.data() + 8 * (secondSlots_[i] - 1));
  }
  // witchhunt: deviation 18 - the lookup's group masks, once fillSubtableGroups() has made them.
  const size_t k = table == GSUB ? i : lookups[GSUB].size() + i;
  if ((out.subtableDigests || out.fontRecords) && !testBypassCaches && k < groupStarts_.size() &&
      groupStarts_[k] != NO_GROUPS) {
    out.subtableGroups = groupMasks_.data() + 8 * groupStarts_[k];
  }
  return out;
}

// witchhunt: deviation 13 - applyTable()'s digest test, on the plan's copy of
// the digest, so it comes before settings() reads anything of the font.
OT_HOT bool Plan::mayApply(const int table, const size_t i, const Digest& buffer) const {
  return testBypassCaches || readDigest(digestRecord(table, i)).mayIntersect(buffer);
}

// witchhunt: deviation 16 - applyLookup() starts only at a glyph its digest may hold, of its mask, that its flags do
// not ignore. A lookup no glyph of the buffer passes the first two for starts nowhere: applyLookup() would only walk
// the buffer to its end, and no pass reads the index it leaves.
OT_HOT bool Plan::mayStart(const int table, const size_t i, const Buffer& buffer) const {
  if (testBypassCaches) return true;
  const Digest digest = readDigest(digestRecord(table, i));
  const uint32_t mask = lookups[table][i].mask;
  for (const GlyphInfo& g : buffer.info) {
    if ((g.mask & mask) && digest.mayHave(g.codepoint)) return true;
  }
  return false;
}

// witchhunt: deviation 13 - see positions_. A glyph past 16 bits (no font has
// one) walks every time.
uint8_t Plan::consonantPosition(const Face& face, const uint32_t consonant, const uint32_t virama) const {
  if (testBypassCaches || consonant > UINT16_MAX || virama > UINT16_MAX) {
    return consonantPositionFromFace(face, *this, consonant, virama);
  }
  const uint32_t key = consonant | virama << 16;
  const size_t slot = (consonant ^ virama) % POSITION_SLOTS;
  if (positions_[slot] == 0 || positionKeys_[slot] != key) {
    positionKeys_[slot] = key;
    positions_[slot] = consonantPositionFromFace(face, *this, consonant, virama);
  }
  return positions_[slot];
}

size_t Plan::memoryBytes() const {
  size_t bytes = sizeof(Plan) + lookupDigests_.capacity();  // witchhunt: deviation 13 - the lookup digests
  bytes += groupMasks_.capacity() + groupStarts_.capacity() * sizeof(uint16_t);  // witchhunt: deviation 18
  bytes += secondMasks_.capacity() + secondSlots_.capacity();                    // witchhunt: deviation 19
  bytes += lookupOffsets_.capacity() * sizeof(uint16_t);                         // witchhunt: deviation 20
  for (int t = GSUB; t <= GPOS; t++) {
    bytes += lookups[t].capacity() * sizeof(PlannedLookup) + stages[t].capacity() * sizeof(Stage) +
             subtableStarts_[t].capacity() * sizeof(uint32_t) + subtableDigests_[t].capacity();
  }
  return bytes;
}

// The lookups of the stage the feature is applied in (get_stage_lookups()).
void Plan::collectWould(const WouldFeature feature) {
  WouldSubstituteLookups& w = would_[feature];
  w = WouldSubstituteLookups();
  const unsigned stage = wouldStage[feature];
  if (stage == NO_STAGE || stage > stages[GSUB].size()) return;
  // Stages end in order within lookups[GSUB] (read() and the builder check),
  // and a plan has at most MAX_PLANNED_LOOKUPS of them.
  w.start = stage ? stages[GSUB][stage - 1].lastLookup : 0;
  w.end = static_cast<uint16_t>(stage < stages[GSUB].size() ? stages[GSUB][stage].lastLookup : lookups[GSUB].size());
}

// witchhunt: deviation 13 - each lookup's digest is the plan's copy, which
// every plan holds: HarfBuzz always has it (format-3 contexts leave their
// first glyph to it), where the port fell back to "any first glyph" when a
// plan had no filters.
bool Plan::wouldSubstitute(const Face& face, const WouldFeature feature, const uint32_t* glyphs,
                           const unsigned count) const {
  const WouldSubstituteLookups& w = would_[feature];
  for (size_t i = w.start; i < w.end; i++) {
    if (ot::wouldSubstitute(face, lookups[GSUB][i].index, readDigest(digestRecord(GSUB, i)), glyphs, count,
                            zeroContext_)) {
      return true;
    }
  }
  return false;
}

}  // namespace ot
