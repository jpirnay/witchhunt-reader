#include "EpdFont.h"

#include <Utf8.h>

#include <algorithm>

#include "SmallCaps.h"

// Scale a 12.4 fixed-point advance by the small-caps factor, rounding to nearest.
static inline int32_t scaleAdvanceFP(const int32_t advanceFP) {
  return static_cast<int32_t>(advanceFP * smallCaps::SCALE + 0.5f);
}

void EpdFont::getTextBounds(const char* string, const int startX, const int startY, int* minX, int* minY, int* maxX,
                            int* maxY, const bool useSmallCaps) const {
  *minX = startX;
  *minY = startY;
  *maxX = startX;
  *maxY = startY;

  if (*string == '\0') {
    return;
  }

  int lastBaseX = startX;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int lastBaseAdvanceFP = 0;  // 12.4 fixed-point
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap
  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string)))) {
    const bool isCombining = utf8IsCombiningMark(cp);

    if (!isCombining) {
      cp = applyLigatures(cp, string);
    }

    // Small-caps: fold lowercase to uppercase and mark this glyph for scaled metrics.
    const bool folded = useSmallCaps && !isCombining && smallCaps::fold(cp);

    const EpdGlyph* glyph = getGlyph(cp);
    if (!glyph) {
      lastBaseX += fp4::toPixel(prevAdvanceFP);  // flush pending advance before resetting
      prevCp = 0;
      prevAdvanceFP = 0;
      continue;
    }

    // Folded glyphs are drawn at smallCaps::SCALE, so all their metrics scale to match.
    const int glyphLeft = folded ? static_cast<int>(glyph->left * smallCaps::SCALE) : glyph->left;
    const int glyphWidth = folded ? static_cast<int>(glyph->width * smallCaps::SCALE + 0.5f) : glyph->width;
    const int glyphTop = folded ? static_cast<int>(glyph->top * smallCaps::SCALE) : glyph->top;
    const int glyphHeight = folded ? static_cast<int>(glyph->height * smallCaps::SCALE + 0.5f) : glyph->height;

    const int raiseBy = isCombining ? combiningMark::raiseAboveBase(glyphTop, glyphHeight, lastBaseTop) : 0;

    if (!isCombining && prevCp != 0) {
      auto kernFP = static_cast<int32_t>(getKerning(prevCp, cp));  // 4.4 fixed-point kern
      if (folded) kernFP = scaleAdvanceFP(kernFP);
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);
    }

    const int glyphBaseX =
        isCombining ? combiningMark::centerOver(lastBaseX, lastBaseLeft, lastBaseWidth, glyphLeft, glyphWidth)
                    : lastBaseX;
    const int glyphBaseY = startY - raiseBy;

    *minX = std::min(*minX, glyphBaseX + glyphLeft);
    *maxX = std::max(*maxX, glyphBaseX + glyphLeft + glyphWidth);
    *minY = std::min(*minY, glyphBaseY + glyphTop - glyphHeight);
    *maxY = std::max(*maxY, glyphBaseY + glyphTop);

    if (!isCombining) {
      lastBaseLeft = glyphLeft;
      lastBaseWidth = glyphWidth;
      lastBaseAdvanceFP = folded ? scaleAdvanceFP(glyph->advanceX) : glyph->advanceX;  // 12.4 fixed-point
      lastBaseTop = glyphTop;
      prevAdvanceFP = lastBaseAdvanceFP;
      prevCp = cp;
    }
  }
}

void EpdFont::getTextDimensions(const char* string, int* w, int* h, const bool useSmallCaps) const {
  int minX = 0, minY = 0, maxX = 0, maxY = 0;

  getTextBounds(string, 0, 0, &minX, &minY, &maxX, &maxY, useSmallCaps);

  *w = maxX - minX;
  *h = maxY - minY;
}

static uint8_t lookupKernClass(const EpdKernClassEntry* entries, const uint16_t count, const uint32_t cp) {
  if (!entries || count == 0 || cp > 0xFFFF) {
    return 0;
  }

  const auto target = static_cast<uint16_t>(cp);
  const auto* end = entries + count;

  // lower_bound: exact-key lookup. Finds the first entry with codepoint >= target,
  // then the equality check confirms an exact match exists.
  const auto it = std::lower_bound(
      entries, end, target, [](const EpdKernClassEntry& entry, uint16_t value) { return entry.codepoint < value; });

  if (it != end && it->codepoint == target) {
    return it->classId;
  }

  return 0;
}

int8_t EpdFont::getKerning(const uint32_t leftCp, const uint32_t rightCp) const {
  if (!data->kernMatrix) {
    return 0;
  }
  const uint8_t lc = lookupKernClass(data->kernLeftClasses, data->kernLeftEntryCount, leftCp);
  if (lc == 0) return 0;
  const uint8_t rc = lookupKernClass(data->kernRightClasses, data->kernRightEntryCount, rightCp);
  if (rc == 0) return 0;
  return data->kernMatrix[(lc - 1) * data->kernRightClassCount + (rc - 1)];
}

uint32_t EpdFont::getLigature(const uint32_t leftCp, const uint32_t rightCp) const {
  const auto* pairs = data->ligaturePairs;
  const auto count = data->ligaturePairCount;
  if (!pairs || count == 0 || leftCp > 0xFFFF || rightCp > 0xFFFF) {
    return 0;
  }

  const uint32_t key = (leftCp << 16) | rightCp;
  const auto* end = pairs + count;

  // lower_bound: exact-key lookup. Finds the first entry with pair >= key,
  // then the equality check confirms an exact match exists.
  const auto it =
      std::lower_bound(pairs, end, key, [](const EpdLigaturePair& pair, uint32_t value) { return pair.pair < value; });

  if (it != end && it->pair == key) {
    return it->ligatureCp;
  }

  return 0;
}

uint32_t EpdFont::applyLigatures(uint32_t cp, const char*& text) const {
  if (!data->ligaturePairs || data->ligaturePairCount == 0) {
    return cp;
  }
  while (true) {
    const auto saved = reinterpret_cast<const uint8_t*>(text);
    const uint32_t nextCp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text));
    if (nextCp == 0) break;
    const uint32_t lig = getLigature(cp, nextCp);
    if (lig == 0) {
      text = reinterpret_cast<const char*>(saved);
      break;
    }
    cp = lig;
  }
  return cp;
}

const EpdGlyph* EpdFont::getGlyph(const uint32_t cp) const {
  const int count = data->intervalCount;
  if (count == 0 && !data->glyphMissHandler) return nullptr;

  if (count > 0) {
    const EpdUnicodeInterval* intervals = data->intervals;
    const auto* end = intervals + count;

    // upper_bound: range lookup. Finds the first interval with first > cp, so the
    // interval just before it is the last one with first <= cp. That's the only
    // candidate that could contain cp. Then we verify cp <= candidate.last.
    const auto it = std::upper_bound(
        intervals, end, cp, [](uint32_t value, const EpdUnicodeInterval& interval) { return value < interval.first; });

    if (it != intervals) {
      const auto& interval = *(it - 1);
      if (cp <= interval.last) {
        return &data->glyph[interval.offset + (cp - interval.first)];
      }
    }
  }

  // Codepoint not in interval table — try on-demand loading (SD card fonts).
  if (data->glyphMissHandler) {
    const EpdGlyph* loaded = data->glyphMissHandler(data->glyphMissCtx, cp);
    if (loaded) return loaded;
  }

  if (cp != REPLACEMENT_GLYPH) {
    return getGlyph(REPLACEMENT_GLYPH);
  }
  return nullptr;
}
