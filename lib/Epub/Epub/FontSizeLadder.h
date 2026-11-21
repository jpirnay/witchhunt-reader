#pragma once

#include <TextScale.h>

#include <cstdint>

// FontSizeLadder — the body font's sibling sizes, used to snap an effective block
// font size to a REAL pre-rendered font instead of nearest-neighbor glyph scaling
// (concept from CidVonHighwind/microreader's BitmapFontSet).
//
// The app layer builds the ladder (it knows the font family and which built-in
// sizes exist) and passes it down as pure data so the Epub lib stays
// settings-agnostic — the same contract the old HeadingFonts struct had.
// Each rung is a registered fontId plus its size as a percent of the body font
// (the body font itself is the 100% rung). SD-card fonts ship a single loaded
// size, so their ladder is empty and everything resolves to the scale fallback
// (their body ID may itself carry a base scale when the card lacks the chosen
// size -- SdCardFontManager::ensureSizeAlias -- which compounds like any other).
//
// Deterministic from the body fontId by construction, so it is deliberately NOT
// part of the section-cache property hash (fontId already is). That holds WITHIN a
// firmware version and not across one: adding or removing a rung changes which face
// a heading resolves to for an unchanged body fontId, so the hash still matches a
// cache laid out by the old ladder. Changing the shipped rungs is therefore a cache
// format change and MUST bump SECTION_FILE_VERSION -- adding 20/22/24/26 pt did, and so did
// turning 20 pt from a real face into a scaled one, which changed every one of those rungs' IDs.
// Some rungs have no face of their own: 20/22/24/26 pt render the 18 pt master scaled (see
// GfxRenderer::insertScaledFont). This struct does not need to know which, and that is worth
// stating because an earlier note here said the opposite.
//
// The worry was double resampling: resolve() landing on a synthesised rung and then applying a
// residual on top, scaling an already-scaled glyph. That would be real if the scale lived in the
// glyph data. It lives on the FONT ID instead, and GfxRenderer compounds it with whatever residual
// the caller passes -- drawTextScaled(id24, ..., 1.1f) is ONE resample of the 18 pt master at
// 1.33 x 1.1, never a resample of a resample. So a synthesised rung is as good a source as a real
// one, resolve() can treat every rung alike, and no per-rung flag is needed.
//
// What does still hold: the rung's sizePct is the rung's nominal size, so the residual resolve()
// hands back is correct whether the rung is real or scaled. And kMaxRungs is a CAPACITY, not a
// description of the shipped set -- see the note on it.
struct FontSizeLadder {
  // Capacity, not a description of the shipped set -- naming the sizes here is what let this go
  // stale. addRung() drops anything past it SILENTLY, so a family that grew a size simply lost
  // its largest rung and every heading that wanted it resampled from a smaller face instead.
  // Exactly that happened when the 24 pt rung was added. The app asserts its own ladder fits
  // (see buildReaderFontSizeLadder), which is the check that would have caught it.
  static constexpr int kMaxRungs = 9;

  struct Rung {
    int32_t fontId = 0;    // 32-bit font-id hash (fontIds.h); never truncate
    uint16_t sizePct = 0;  // rung size as percent of the body font (body = 100)
  };

  Rung rungs[kMaxRungs] = {};
  int8_t count = 0;

  // A family drawn natively at any size (a TTF family) has no rungs: every size is a face of its own at a whole
  // number of pixels per em, font ID nativeBase | px (SdCardFontManager hands out that window of 256 IDs and makes
  // a face on the first lookup of one). Whole pixels keep the leftover scale inside the dead zone from about 17 px
  // up, so nothing is resampled; above nativeMaxPx the largest face is drawn scaled. Also derived from the body
  // fontId alone (the family's identity is in both), so not hashed either.
  int32_t nativeBase = 0;       // 0: no native sizes
  uint32_t nativeBodyPpem = 0;  // the body face's pixels per em, 26.6
  uint8_t nativeMinPx = 0;
  uint8_t nativeMaxPx = 0;

  void setNative(const int32_t base, const uint32_t bodyPpem26_6, const uint8_t minPx, const uint8_t maxPx) {
    if (base == 0 || bodyPpem26_6 == 0 || minPx == 0 || maxPx < minPx) return;
    nativeBase = base;
    nativeBodyPpem = bodyPpem26_6;
    nativeMinPx = minPx;
    nativeMaxPx = maxPx;
  }
  bool native() const { return nativeBase != 0; }

  void addRung(const int32_t fontId, const uint16_t sizePct) {
    if (count >= kMaxRungs || fontId == 0 || sizePct == 0) return;
    rungs[count].fontId = fontId;
    rungs[count].sizePct = sizePct;
    ++count;
  }

  // The size of a font resolve() returned, relative to the body (1.0 for the body font, fontId 0):
  // with a resolved block's residual it gives back the block's size, which the parser needs to put
  // a body-relative (rem, px) span into a block of another size.
  TextScale scaleOf(const int32_t fontId) const {
    if (fontId == 0) return TextScale();
    if (native() && (fontId & ~0xFF) == nativeBase) return TextScale::ratio((fontId & 0xFF) * 64, nativeBodyPpem);
    for (int i = 0; i < count; ++i) {
      if (rungs[i].fontId == fontId) return TextScale::percent(rungs[i].sizePct);
    }
    return TextScale();
  }

  // Residual dead zone: a leftover scale this close to 1.0 is snapped to exactly 1.0,
  // so the block renders NATIVE glyphs of the chosen rung instead of resampling every
  // glyph for a size difference below the ~5% typographic just-noticeable threshold.
  // Kept at 3% so deliberate fine gradients (Alice's 99/98/97… mouse tale) only lose
  // their first, imperceptible steps. Applied to the final residual regardless of rung
  // (including the body font and the empty-ladder SD path).
  static constexpr int kResidualDeadZonePct = 3;

  // Result of snapping a desired size to the ladder. fontId == 0 means "use the
  // body font"; residual is the remaining scale applied on top of the chosen font
  // so the exact desired size is reached (1.0 when the rung matches exactly).
  struct Resolved {
    int32_t fontId = 0;
    TextScale residual;
  };

  // Whether a residual is inside the dead zone, so close to 1 that it is drawn at 1.
  static bool inDeadZone(const TextScale residual) {
    return residual > TextScale::percent(100 - kResidualDeadZonePct) &&
           residual < TextScale::percent(100 + kResidualDeadZonePct);
  }

  // Snap `size` (a multiple of the body size) to the nearest rung. An empty
  // ladder, or a nearest rung of 100%, keeps the body font with the full scale
  // as residual — exactly the legacy scale-only behavior.
  Resolved resolve(const TextScale size) const {
    Resolved r = native() ? resolveNative(size) : resolveExact(size);
    if (inDeadZone(r.residual)) r.residual = TextScale();
    return r;
  }

 private:
  Resolved resolveNative(const TextScale size) const {
    Resolved r;
    r.residual = size;
    // Within the dead zone of the body size, the body face itself.
    if (size.raw() == 0 || inDeadZone(size)) return r;
    // The size in pixels, in 64ths (the face's 26.6) times 2^20: exact, and in range for any size a book asks for.
    const int64_t target = static_cast<int64_t>(nativeBodyPpem) * size.raw();
    const int64_t pixel = int64_t{64} << TextScale::kShift;
    const int px = target > nativeMaxPx * pixel   ? nativeMaxPx
                   : target > nativeMinPx * pixel ? static_cast<int>((target + pixel / 2) / pixel)
                                                  : nativeMinPx;
    r.fontId = nativeBase | px;
    r.residual = TextScale::ratio(target, px * pixel);
    return r;
  }

  Resolved resolveExact(const TextScale size) const {
    Resolved r;
    r.residual = size;
    if (count == 0 || size.raw() == 0) return r;

    // Distances in 100ths of TextScale's grid: |rung% x 2^20 - 100 x size|, exact.
    const int64_t wanted = static_cast<int64_t>(size.raw()) * 100;
    int best = -1;
    int64_t bestDiff = 0;
    for (int i = 0; i < count; ++i) {
      const int64_t rung = static_cast<int64_t>(rungs[i].sizePct) << TextScale::kShift;
      const int64_t diff = rung > wanted ? rung - wanted : wanted - rung;
      if (best < 0 || diff < bestDiff) {
        best = i;
        bestDiff = diff;
      }
    }
    if (best < 0 || rungs[best].sizePct == 100) return r;  // body font is the closest — pure scale

    r.fontId = rungs[best].fontId;
    r.residual = TextScale::ratio(wanted, static_cast<int64_t>(rungs[best].sizePct) << TextScale::kShift);
    return r;
  }
};
