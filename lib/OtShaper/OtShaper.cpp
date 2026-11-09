#include "OtShaper.h"

#include <algorithm>
#include <cstring>

#include "OtShaperInternal.h"
#include "OtUnicodeData.h"

// The shaping pipeline (HarfBuzz's hb_ot_shape_internal): Unicode
// properties, vowel constraints, normalization, character categories, GSUB
// stages with the shaper's pauses, then positioning and hiding of
// default-ignorable characters.

namespace ot {

namespace {

// Most glyphs a run may become while shaping, relative to its length.
// Real text grows by a few glyphs at most (split vowels, dotted circles);
// the cap stops pathological fonts and bounds the buffer's reservation.
constexpr unsigned MAX_LENGTH_FACTOR = 2;
constexpr unsigned MAX_LENGTH_EXTRA = 16;
// Nested-lookup budget (HarfBuzz's max_ops), relative to the run length.
constexpr int MAX_OPS_FACTOR = 64;
constexpr int MAX_OPS_MIN = 1024;

void runPause(const Pause pause, const Face& face, const Plan& plan, Buffer& buffer) {
  switch (pause) {
    case Pause::None:
      break;
    case Pause::IndicSetupSyllables:
      indicSetupSyllables(buffer);
      break;
    case Pause::IndicInitialReordering:
      indicInitialReordering(face, plan, buffer);
      break;
    case Pause::IndicFinalReordering:
      indicFinalReordering(face, plan, buffer);
      break;
    case Pause::UseSetupSyllables:
      useSetupSyllables(plan, buffer);
      break;
    case Pause::UseClearSubstitutionFlags:
      for (GlyphInfo& g : buffer.info) g.glyphProps &= ~props::SUBSTITUTED;
      break;
    case Pause::UseRecordRphf:
      useRecordRphf(plan, buffer);
      break;
    case Pause::UseRecordPref:
      useRecordPref(buffer);
      break;
    case Pause::UseReorder:
      useReorder(face, buffer);
      break;
    case Pause::MyanmarSetupSyllables:  // witchhunt: deviation 14
      myanmarSetupSyllables(buffer);
      break;
    case Pause::MyanmarReorder:  // witchhunt: deviation 14
      myanmarReorder(face, buffer);
      break;
  }
}

// hb_ot_map_t::apply(): every stage's lookups, then its pause.
void applyTable(const Face& face, const Scale& scale, const Plan& plan, Buffer& buffer, const int table) {
  size_t i = 0;
  for (const Stage& stage : plan.stages[table]) {
    for (; i < stage.lastLookup; i++) {
      // Only lookups that can start at a glyph the buffer holds.
      // witchhunt: deviation 13 - tested on the plan's copy of the digest, before settings() reads the font.
      if (!plan.mayApply(table, i, buffer.digest)) continue;
      // witchhunt: deviation 16 - and only when a glyph may start it, before settings() reads the font.
      if (!plan.mayStart(table, i, buffer)) continue;
      const LookupSettings settings = plan.settings(table, i);
      applyLookup(face, scale, buffer, table, plan.lookups[table][i].index, settings);
      if (!buffer.successful) return;
    }
    if (stage.pause == Pause::None) continue;
    runPause(stage.pause, face, plan, buffer);
    if (!buffer.successful) return;
    buffer.refreshDigest();
  }
}

void zeroMarkWidths(Buffer& buffer, const bool adjustOffsets) {
  for (unsigned i = 0; i < buffer.len(); i++) {
    if (!buffer.info[i].isMark()) continue;
    GlyphPosition& p = buffer.pos[i];
    if (adjustOffsets) {
      p.xOffset -= p.xAdvance;
      p.yOffset -= p.yAdvance;
    }
    p.xAdvance = 0;
    p.yAdvance = 0;
  }
}

// hb_ot_position(): hb_ot_position_default and hb_ot_position_plan, then a
// backward run reversed into visual order.
void position(const Face& face, const Scale& scale, const Plan& plan, Buffer& buffer) {
  const unsigned len = buffer.len();
  buffer.pos.assign(len, GlyphPosition{});
  for (unsigned i = 0; i < len; i++) buffer.pos[i].xAdvance = scale.emScaleX(face.advance(buffer.info[i].codepoint));
  // USE zeroes mark advances before GPOS, the default and Arabic shapers after it.
  // witchhunt: deviation 11 - marks keep their place over the glyph before only in a forward run (a backward run's
  // hang over the next one once reversed), and the Arabic shaper zeroes late (BY_GDEF_LATE).
  // witchhunt: deviation 14 - the Myanmar shaper zeroes early (BY_GDEF_EARLY), as USE does.
  const bool adjustOffsets = plan.adjustMarkPositioningWhenZeroing && !buffer.backward;
  if (plan.zeroMarks && (plan.shaper == ShaperKind::Use || plan.shaper == ShaperKind::Myanmar)) {
    zeroMarkWidths(buffer, adjustOffsets);
  }
  if (plan.applyGpos) applyTable(face, scale, plan, buffer, GPOS);
  if (plan.zeroMarks && (plan.shaper == ShaperKind::Default || plan.shaper == ShaperKind::Arabic)) {
    zeroMarkWidths(buffer, adjustOffsets);
  }
  if (buffer.hasDefaultIgnorables) {
    for (unsigned i = 0; i < len; i++) {
      if (buffer.info[i].isDefaultIgnorable()) {
        buffer.pos[i].xAdvance = buffer.pos[i].yAdvance = 0;
        buffer.pos[i].xOffset = 0;
      }
    }
  }
  propagateAttachmentOffsets(buffer);
  if (buffer.backward) buffer.reverse();  // witchhunt: deviation 11
}

// hb_ot_rotate_chars(): in a backward run, each character becomes its
// Bidi_Mirroring_Glyph where the font has that, else takes rtlm's mask.
// witchhunt: deviation 11 - no character the Unicode tables cover
// (ucd::RANGES, ZWNJ, ZWJ, U+25CC) is Bidi_Mirrored, and a run holds no
// other (deviation 6 leaves brackets and punctuation to bidi, which mirrors
// them): no character is swapped, every one takes the mask.
void rotateChars(const Plan& plan, Buffer& buffer) {
  if (!buffer.backward) return;
  for (GlyphInfo& g : buffer.info) g.mask |= plan.rtlmMask;
}

// hb_ot_hide_default_ignorables(): joiners become an invisible space (or go,
// in a font without one).
void hideDefaultIgnorables(const Face& face, Buffer& buffer) {
  if (!buffer.hasDefaultIgnorables) return;
  uint32_t space;
  if (face.nominalGlyph(' ', &space)) {
    for (GlyphInfo& g : buffer.info) {
      if (g.isDefaultIgnorable()) g.codepoint = space;
    }
    return;
  }
  unsigned j = 0;
  for (unsigned i = 0; i < buffer.len(); i++) {
    if (buffer.info[i].isDefaultIgnorable()) continue;
    buffer.info[j] = buffer.info[i];
    buffer.pos[j] = buffer.pos[i];
    j++;
  }
  buffer.info.resize(j);
  buffer.pos.resize(j);
}

}  // namespace

void insertDottedCircles(const Face& face, Buffer& buffer, const uint8_t brokenSyllableType, const uint8_t category,
                         const uint8_t rephaCategory, const bool setPosition, const uint8_t position) {
  if (!buffer.hasBrokenSyllable) return;
  uint32_t glyph;
  if (!face.nominalGlyph(DOTTED_CIRCLE, &glyph)) return;
  buffer.scratch.clear();
  unsigned i = 0;
  unsigned lastSyllable = 0;
  const unsigned len = buffer.len();
  const auto push = [&buffer](const GlyphInfo& g) {
    if (buffer.scratch.size() >= buffer.maxLength) {
      buffer.successful = false;
      return;
    }
    buffer.scratch.push_back(g);
  };
  while (i < len && buffer.successful) {
    const unsigned syllable = buffer.info[i].syllable;
    if (lastSyllable == syllable || buffer.info[i].syllableType() != brokenSyllableType ||
        (buffer.partialContext && (buffer.info[i].mask & CONTEXT_MASK))) {
      push(buffer.info[i++]);
      continue;
    }
    lastSyllable = syllable;
    GlyphInfo dc{};
    dc.codepoint = glyph;
    dc.category = category;
    if (setPosition) dc.position = position;
    dc.mask = buffer.info[i].mask;
    dc.syllable = buffer.info[i].syllable;
    while (i < len && lastSyllable == buffer.info[i].syllable && buffer.info[i].category == rephaCategory) {
      push(buffer.info[i++]);
    }
    push(dc);
  }
  if (buffer.successful) buffer.info.swap(buffer.scratch);
}

void findSyllables(Buffer& buffer, const uint8_t* classes, const uint8_t classCount, const uint8_t* transitions,
                   const uint8_t* accept, const uint8_t otherType, const uint8_t brokenType) {
  const unsigned len = buffer.len();
  unsigned serial = 1;
  unsigned p = 0;
  while (p < len) {
    uint8_t found = otherType;
    unsigned length = scanSyllable(
        classes, classCount, transitions, accept, len - p,
        [&](const unsigned i) { return buffer.info[p + i].category; }, &found);
    if (length == 0) length = 1;
    const uint8_t type = found;
    if (type == brokenType) buffer.hasBrokenSyllable = true;
    for (unsigned i = p; i < p + length; i++) buffer.info[i].syllable = static_cast<uint8_t>((serial << 4) | type);
    serial++;
    if (serial == 16) serial = 1;
    p += length;
  }
}

bool shape(const Face& face, const Scale& scale, const Plan& plan, const uint32_t* codepoints, const unsigned count,
           Buffer& buffer, const RunContext& context) {
  if (!buffer.prepare(count * MAX_LENGTH_FACTOR + MAX_LENGTH_EXTRA)) return false;
  buffer.partialContext = context.partial;
  buffer.backward = plan.backward;  // witchhunt: deviation 11
  if (!count) return true;
  buffer.maxOps = std::max(static_cast<int>(count) * MAX_OPS_FACTOR, MAX_OPS_MIN);

  // hb_set_unicode_props().
  for (unsigned i = 0; i < count; i++) {
    GlyphInfo g{};
    g.codepoint = codepoints[i];
    g.mask = plan.globalMask;
    if (i < context.before || i + context.after >= count) g.mask |= CONTEXT_MASK;
    setUnicodeProps(g, buffer);
    if (g.isZwj()) g.unicodeProps |= uprops::CONTINUATION;
    buffer.info.push_back(g);
  }

  // witchhunt: deviation 11 - the Arabic shaper has no preprocess_text. witchhunt: deviation 14 - nor has Myanmar's.
  if (plan.shaper == ShaperKind::Indic || plan.shaper == ShaperKind::Use) {
    preprocessVowelConstraints(plan.script, buffer);
  }
  rotateChars(plan, buffer);  // witchhunt: deviation 11
  normalize(face, plan, buffer);
  if (!buffer.successful) return false;

  if (plan.shaper == ShaperKind::Indic) {
    indicSetupMasks(buffer);
  } else if (plan.shaper == ShaperKind::Use) {
    useSetupMasks(buffer);
  } else if (plan.shaper == ShaperKind::Arabic) {
    arabicSetupMasks(plan, buffer);  // witchhunt: deviation 11
  } else if (plan.shaper == ShaperKind::Myanmar) {
    myanmarSetupMasks(buffer);  // witchhunt: deviation 14
  }

  // Map to glyphs and set their properties (hb_ot_layout_substitute_start).
  for (GlyphInfo& g : buffer.info) {
    g.codepoint = g.glyphIndex;
    g.glyphProps = face.glyphProps(g.codepoint);
    g.ligProps = 0;
  }
  buffer.refreshDigest();
  if (plan.fallbackGlyphClasses) {
    for (GlyphInfo& g : buffer.info) {
      g.glyphProps = (g.genCat() != gc::NON_SPACING_MARK || g.isDefaultIgnorable()) ? props::BASE_GLYPH : props::MARK;
    }
  }

  applyTable(face, scale, plan, buffer, GSUB);
  if (!buffer.successful) return false;
  position(face, scale, plan, buffer);
  hideDefaultIgnorables(face, buffer);
  if (context.before || context.after) {
    // Every glyph keeps its source codepoint's mask: drop the context's.
    unsigned kept = 0;
    for (unsigned i = 0; i < buffer.len(); i++) {
      if (buffer.info[i].mask & CONTEXT_MASK) continue;
      buffer.info[kept] = buffer.info[i];
      buffer.pos[kept] = buffer.pos[i];
      kept++;
    }
    buffer.info.resize(kept);
    buffer.pos.resize(kept);
  }
  return buffer.successful;
}

unsigned maxRunLength(const Buffer& buffer) {
  const size_t capacity = buffer.info.capacity();
  return capacity < MAX_LENGTH_EXTRA ? 0 : static_cast<unsigned>((capacity - MAX_LENGTH_EXTRA) / MAX_LENGTH_FACTOR);
}

const uint32_t* languageTagsFor(const char* bcp47) {
  static constexpr uint32_t NONE[1] = {0};
  if (!bcp47 || !*bcp47) return NONE;
  char primary[4] = {};
  for (size_t n = 0; bcp47[n] && bcp47[n] != '-' && bcp47[n] != '_'; n++) {
    if (n >= 3) return NONE;
    const char c = bcp47[n];
    primary[n] = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  }
  for (const auto& l : ucd::LANGUAGES) {
    if (strcmp(l.code, primary) == 0) return l.tags;
  }
  return NONE;
}

}  // namespace ot
