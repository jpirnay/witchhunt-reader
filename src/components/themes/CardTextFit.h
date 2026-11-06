#pragma once

#include <array>

// How many lines each part of a home card's text block gets, so the block fits its tile.
//
// The card stacks a title, author, series, a status line and the reading history, each wrapped
// to its own cap. Nothing compared the sum with the tile: a long title, a list of co-authors and
// a long series name each took their full cap, the block outgrew the tile, and centring it put
// the title above the selection highlight and the status line below it (#375).
//
// Pure arithmetic over line counts, with no renderer dependency, so the order in which the parts
// give way is pinned on the host. The caller wraps each part at its cap, passes the line counts
// in, and re-wraps only the parts whose count came back smaller: wrappedText() ellipsizes the
// last line it is allowed, so a shortened part still says it was cut.
namespace CardTextFit {

enum PartId { Title, Author, Series, Status, History, kPartCount };

struct Part {
  int lineHeight = 0;
  // Lines the part wraps to at its own cap; 0 when the card does not show it.
  int lines = 0;
  // Space above the part when anything is drawn above it.
  int gap = 0;
};

using Parts = std::array<Part, kPartCount>;
using Lines = std::array<int, kPartCount>;

struct Shed {
  PartId part;
  int maxLines;
};

// Least important first. The history is cut to one line before it goes, and callers draw a
// history held below its sentence as the compact form ("3h 20m · 4d"), not as half a sentence.
// Series yields before author and title: it often repeats the title. The status line carries
// the progress and is never shed.
inline constexpr Shed kGiveWayOrder[] = {
    {History, 1}, {History, 0}, {Series, 1}, {Series, 0}, {Author, 1}, {Title, 3}, {Title, 2}, {Title, 1},
};

struct Placement {
  // Where each part's first line goes, from the top of the block.
  Lines tops{};
  int height = 0;
};

// The one walk both the measurement and the draw use, so what is painted is what was measured.
constexpr Placement place(const Parts& parts, const Lines& lines, const bool tight, const int tightGap) {
  Placement placement;
  bool anyAbove = false;
  for (int i = 0; i < kPartCount; i++) {
    placement.tops[i] = placement.height;
    if (lines[i] <= 0) continue;
    if (anyAbove) placement.height += tight && tightGap < parts[i].gap ? tightGap : parts[i].gap;
    placement.tops[i] = placement.height;
    placement.height += lines[i] * parts[i].lineHeight;
    anyAbove = true;
  }
  return placement;
}

constexpr int height(const Parts& parts, const Lines& lines, const bool tight, const int tightGap) {
  return place(parts, lines, tight, tightGap).height;
}

struct Result : Placement {
  Lines lines{};
  // Every gap shrunk to the tight gap.
  bool tight = false;
  // False only when even the leanest block is taller than the space; the caller anchors it.
  bool fits = false;
};

// Spacing gives way before content does: every step is tried with the parts' own gaps, then with
// tight ones, before the next part is cut.
constexpr Result fit(const Parts& parts, const int available, const int tightGap) {
  Result result;
  for (int i = 0; i < kPartCount; i++) result.lines[i] = parts[i].lines;

  const auto settle = [&](const bool tight) {
    static_cast<Placement&>(result) = place(parts, result.lines, tight, tightGap);
    result.tight = tight;
    return result.height <= available;
  };
  const auto tryGaps = [&]() { return settle(false) || settle(true); };

  if (tryGaps()) {
    result.fits = true;
    return result;
  }
  for (const Shed& step : kGiveWayOrder) {
    if (result.lines[step.part] <= step.maxLines) continue;
    result.lines[step.part] = step.maxLines;
    if (tryGaps()) {
      result.fits = true;
      return result;
    }
  }
  return result;
}

}  // namespace CardTextFit
