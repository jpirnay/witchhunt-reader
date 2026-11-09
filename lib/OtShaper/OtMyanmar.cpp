#include "OtShaperInternal.h"
#include "OtSort.h"
#include "OtSyllableMachines.h"

// The Myanmar shaper: syllable segmentation, then one reordering before the
// basic-shaping GSUB features. A port of HarfBuzz's hb-ot-shaper-myanmar.cc
// (setup_masks_myanmar, setup_syllables_myanmar, reorder_myanmar) and
// hb-ot-shaper-myanmar-machine.rl, which follow Microsoft's Myanmar script
// development spec. HarfBuzz is licensed under the "Old MIT" license; see
// COPYING in this directory. Not ported: the Zawgyi shaper (only the private
// script Qaag selects it) and unsafe_to_break (no clusters here, deviation 6).
// Ported from Somnius/CrossInk-Indic (commit dbfb96a24 by Lef, MIT), reworked
// here for the nothrow engine: the shared syllable scan (findSyllables) and
// the Myanmar categories' names (OtCategories.h).
// witchhunt: deviation 14

namespace ot {

namespace {

// Vowels and placeholders count as consonants: they cannot occur in a
// consonant syllable, and treating them so lets broken clusters reorder like
// consonant syllables (CONSONANT_FLAGS_MYANMAR).
constexpr uint64_t CONSONANT_FLAGS =
    flag(icat::C) | flag(icat::CS) | flag(icat::Ra) | flag(mcat::IV) | flag(mcat::GB) | flag(icat::DOTTEDCIRCLE);

// hb_syllabic_insert_dotted_circles()' repha category for Myanmar: none (-1), a value no glyph carries.
constexpr uint8_t NO_REPHA = 0xFF;

bool isConsonant(const GlyphInfo& info) {
  if (info.ligated()) return false;  // if it ligated, all bets are off
  return hasBit64(CONSONANT_FLAGS, info.category);
}

// initial_reordering_consonant_syllable().
void reorderConsonantSyllable(Buffer& buffer, const unsigned start, const unsigned end) {
  auto& info = buffer.info;
  unsigned base = end;
  bool hasReph = false;
  unsigned limit = start;
  // Kinzi (Ra, Asat, virama) at the start moves after the base.
  if (start + 3 <= end && info[start].category == icat::Ra && info[start + 1].category == mcat::As &&
      info[start + 2].category == icat::H) {
    limit += 3;
    base = start;
    hasReph = true;
  }
  if (!hasReph) base = limit;
  for (unsigned i = limit; i < end; i++) {
    if (isConsonant(info[i])) {
      base = i;
      break;
    }
  }

  unsigned i = start;
  for (; i < start + (hasReph ? 3 : 0); i++) info[i].position = ipos::AFTER_MAIN;
  for (; i < base; i++) info[i].position = ipos::PRE_C;
  if (i < end) {
    info[i].position = ipos::BASE_C;
    i++;
  }
  uint8_t pos = ipos::AFTER_MAIN;
  for (; i < end; i++) {
    const uint8_t cat = info[i].category;
    if (cat == mcat::MedialRa) {  // medial ra: pre-base
      info[i].position = ipos::PRE_C;
    } else if (cat == mcat::VPre) {  // left matra
      info[i].position = ipos::PRE_M;
    } else if (cat == mcat::VS) {
      info[i].position = info[i - 1].position;
    } else if (pos == ipos::AFTER_MAIN && cat == mcat::VBlw) {
      pos = ipos::BELOW_C;
      info[i].position = pos;
    } else if (pos == ipos::BELOW_C && cat == icat::A) {
      info[i].position = ipos::BEFORE_SUB;
    } else if (pos == ipos::BELOW_C && cat == mcat::VBlw) {
      info[i].position = pos;
    } else if (pos == ipos::BELOW_C) {
      pos = ipos::AFTER_SUB;
      info[i].position = pos;
    } else {
      info[i].position = pos;
    }
  }

  stableSort(info.data() + start, info.data() + end,
             [](const GlyphInfo& a, const GlyphInfo& b) { return a.position < b.position; });

  // Flip a sequence of left matras back into logical order, keeping each
  // matra's variation selector after it (harfbuzz#3863).
  unsigned firstLeftMatra = end;
  unsigned lastLeftMatra = end;
  for (unsigned j = start; j < end; j++) {
    if (info[j].position == ipos::PRE_M) {
      if (firstLeftMatra == end) firstLeftMatra = j;
      lastLeftMatra = j;
    }
  }
  if (firstLeftMatra < lastLeftMatra) {
    buffer.reverseRange(firstLeftMatra, lastLeftMatra + 1);
    unsigned k = firstLeftMatra;
    for (unsigned j = k; j <= lastLeftMatra; j++) {
      if (info[j].category == mcat::VPre) {
        buffer.reverseRange(k, j + 1);
        k = j + 1;
      }
    }
  }
}

}  // namespace

void myanmarSetupMasks(Buffer& buffer) {
  // No masks: only the categories (positions are set while reordering).
  for (GlyphInfo& g : buffer.info) g.category = charData(g.codepoint).indicCategory;
}

void myanmarSetupSyllables(Buffer& buffer) {
  using namespace machines;
  findSyllables(buffer, MYANMAR_CLASSES, MYANMAR_CLASS_COUNT, MYANMAR_TRANSITIONS, MYANMAR_ACCEPT, msyl::NON_MYANMAR,
                msyl::BROKEN);
}

void myanmarReorder(const Face& face, Buffer& buffer) {
  insertDottedCircles(face, buffer, msyl::BROKEN, icat::DOTTEDCIRCLE, NO_REPHA, false, 0);
  if (!buffer.successful) return;
  forEachSyllable(buffer, [&](const unsigned start, const unsigned end) {
    // A broken cluster has its dotted circle now: it reorders as a consonant syllable.
    const uint8_t type = buffer.info[start].syllableType();
    if (type == msyl::CONSONANT || type == msyl::BROKEN) reorderConsonantSyllable(buffer, start, end);
  });
}

}  // namespace ot
