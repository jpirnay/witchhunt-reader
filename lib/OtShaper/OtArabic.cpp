#include <algorithm>

#include "OtShaperInternal.h"

// The Arabic shaper's joining and mark reordering: a port of HarfBuzz's
// hb-ot-shaper-arabic.cc (arabic_joining, setup_masks_arabic_plan and
// reorder_marks_arabic). HarfBuzz is licensed under the "Old MIT" license; see
// COPYING in this directory. Not ported: the fallback shaper, stch and the
// Mongolian variation selectors.
// witchhunt: deviation 11

namespace ot {

namespace {

using namespace arabic;

struct StateEntry {
  uint8_t prevAction;
  uint8_t currAction;
  uint8_t nextState;
};

// Columns: jt_U, jt_L, jt_R, jt_D, jg_ALAPH, jg_DALATH_RISH (JoiningType U..DalathRish).
constexpr unsigned STATE_COLUMNS = 6;
constexpr StateEntry STATE_TABLE[][STATE_COLUMNS] = {
    // State 0: prev was U, not willing to join.
    {{NONE, NONE, 0}, {NONE, ISOL, 2}, {NONE, ISOL, 1}, {NONE, ISOL, 2}, {NONE, ISOL, 1}, {NONE, ISOL, 6}},
    // State 1: prev was R or ISOL/ALAPH, not willing to join.
    {{NONE, NONE, 0}, {NONE, ISOL, 2}, {NONE, ISOL, 1}, {NONE, ISOL, 2}, {NONE, FIN2, 5}, {NONE, ISOL, 6}},
    // State 2: prev was D/L in ISOL form, willing to join.
    {{NONE, NONE, 0}, {NONE, ISOL, 2}, {INIT, FINA, 1}, {INIT, FINA, 3}, {INIT, FINA, 4}, {INIT, FINA, 6}},
    // State 3: prev was D in FINA form, willing to join.
    {{NONE, NONE, 0}, {NONE, ISOL, 2}, {MEDI, FINA, 1}, {MEDI, FINA, 3}, {MEDI, FINA, 4}, {MEDI, FINA, 6}},
    // State 4: prev was FINA ALAPH, not willing to join.
    {{NONE, NONE, 0}, {NONE, ISOL, 2}, {MED2, ISOL, 1}, {MED2, ISOL, 2}, {MED2, FIN2, 5}, {MED2, ISOL, 6}},
    // State 5: prev was FIN2/FIN3 ALAPH, not willing to join.
    {{NONE, NONE, 0}, {NONE, ISOL, 2}, {ISOL, ISOL, 1}, {ISOL, ISOL, 2}, {ISOL, FIN2, 5}, {ISOL, ISOL, 6}},
    // State 6: prev was DALATH/RISH, not willing to join.
    {{NONE, NONE, 0}, {NONE, ISOL, 2}, {NONE, ISOL, 1}, {NONE, ISOL, 2}, {NONE, FIN3, 5}, {NONE, ISOL, 6}},
};

// The modifier combining marks (HarfBuzz's modifier_combining_marks).
constexpr uint32_t MODIFIER_COMBINING_MARKS[] = {
    0x0654, 0x0655, 0x0658, 0x06DC, 0x06E3, 0x06E7, 0x06E8, 0x08CA, 0x08CB, 0x08CD, 0x08CE, 0x08CF, 0x08D3, 0x08F3,
};

OT_WARM bool isModifierCombiningMark(const uint32_t cp) {
  return std::find(std::begin(MODIFIER_COMBINING_MARKS), std::end(MODIFIER_COMBINING_MARKS), cp) !=
         std::end(MODIFIER_COMBINING_MARKS);
}

// HB_MODIFIED_COMBINING_CLASS_CCC22 (defined as 25) and CCC26: below all the Arabic classes, folded back to
// 220/230 by HarfBuzz's fallback mark positioning.
constexpr uint8_t MODIFIED_CCC22 = 25;
constexpr uint8_t MODIFIED_CCC26 = 26;

}  // namespace

// arabic_joining(). The context code points are in the buffer (CONTEXT_MASK), so they run through the machine
// like the rest: HarfBuzz seeds the state from the nearest non-transparent context character instead, which
// leaves the same actions for the run's own glyphs.
OT_WARM void arabicSetupMasks(const Plan& plan, Buffer& buffer) {
  const unsigned count = buffer.len();
  constexpr unsigned NO_PREV = ~0u;
  unsigned prev = NO_PREV, state = 0;

  for (unsigned i = 0; i < count; i++) {
    GlyphInfo& info = buffer.info[i];
    const JoiningType type = joiningType(info.codepoint);
    if (type == JoiningType::T) {
      info.position = NONE;
      continue;
    }
    const StateEntry& entry = STATE_TABLE[state][static_cast<unsigned>(type)];
    if (entry.prevAction != NONE && prev != NO_PREV) buffer.info[prev].position = entry.prevAction;
    info.position = entry.currAction;
    prev = i;
    state = entry.nextState;
  }

  for (unsigned i = 0; i < count; i++) buffer.info[i].mask |= plan.arabicMasks[buffer.info[i].position];
}

// reorder_marks_arabic(). There are no clusters to merge.
OT_WARM void reorderMarksArabic(Buffer& buffer, unsigned start, const unsigned end) {
  GlyphInfo* const info = buffer.info.data();
  unsigned i = start;
  for (unsigned cc = 220; cc <= 230; cc += 10) {
    while (i < end && info[i].combiningClass() < cc) i++;
    if (i == end) break;
    if (info[i].combiningClass() > cc) continue;

    unsigned j = i;
    while (j < end && info[j].combiningClass() == cc && isModifierCombiningMark(info[j].codepoint)) j++;
    if (i == j) continue;

    // Shift it: [i, j) to the front of [start, j).
    std::rotate(info + start, info + i, info + j);

    // Renumber the class so the reordered sequence is still sorted.
    const unsigned newStart = start + j - i;
    const uint8_t newCc = cc == 220 ? MODIFIED_CCC22 : MODIFIED_CCC26;
    while (start < newStart) {
      info[start].setModifiedCombiningClass(newCc);
      start++;
    }
    i = j;
  }
}

}  // namespace ot
