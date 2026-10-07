#pragma once

#include <cstddef>
#include <cstdint>

#include "CrossPointSettings.h"

/// The reader sizes one font family offers, in points, ascending and without repeats.
///
/// A stored reader size (CrossPointSettings::fontPointSize, a book's fontSizeOverride) need not be
/// one the active family offers. It may have been chosen under another family, or read from an
/// older settings file. Everything that shows, steps or draws a stored size goes through snap(),
/// so the settings row, the size buttons and the page cannot disagree. The stored value itself is
/// never rewritten to the snapped one, so switching back to the family it was chosen under brings
/// it back.
///
/// Fixed capacity and returned by value: the reader resolves its font through snap() while it lays
/// out pages, where nothing may allocate.
struct ReaderSizeList {
  static constexpr uint8_t kCapacity = 48;
  uint8_t points[kCapacity] = {};
  uint8_t count = 0;

  /// What Bookerly and Noto Sans offer: the reader ladder, CrossPointSettings::FONT_SIZE_RUNGS.
  static ReaderSizeList builtin() {
    static_assert(CrossPointSettings::FONT_SIZE_RUNG_COUNT <= kCapacity, "the ladder must fit the list");
    ReaderSizeList list;
    for (const auto& rung : CrossPointSettings::FONT_SIZE_RUNGS) list.points[list.count++] = rung.points;
    return list;
  }

  /// Position of the size closest to `pt`. Ties go to the smaller, so the answer does not depend
  /// on which side of a gap a value sits.
  uint8_t indexOf(const uint8_t pt) const {
    uint8_t best = 0;
    int bestDiff = 256;
    for (uint8_t i = 0; i < count; ++i) {
      const int diff = points[i] > pt ? points[i] - pt : pt - points[i];
      if (diff < bestDiff) {
        best = i;
        bestDiff = diff;
      }
    }
    return best;
  }

  /// The size closest to `pt`; `pt` itself when it is offered.
  uint8_t snap(const uint8_t pt) const { return count == 0 ? pt : points[indexOf(pt)]; }

  /// `delta` sizes along the list from snap(pt), clamped at both ends rather than wrapped: a pinch
  /// that has reached the largest size should stay there.
  uint8_t step(const uint8_t pt, const int delta) const {
    if (count == 0) return pt;
    int target = indexOf(pt) + delta;
    if (target < 0) target = 0;
    if (target > count - 1) target = count - 1;
    return points[target];
  }

  /// The size after snap(pt), wrapping from the largest to the smallest (BTN_CYCLE_FONT_SIZE).
  uint8_t next(const uint8_t pt) const { return count == 0 ? pt : points[(indexOf(pt) + 1) % count]; }
};
