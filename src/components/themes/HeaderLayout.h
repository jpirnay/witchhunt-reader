#pragma once

#include <algorithm>

#include "components/themes/BaseTheme.h"

// Where a screen's header sits in its content area, and where the body below it starts.
//
// `content` is what UITheme::getContentRect() leaves once the hint strips are taken out, so its
// top is not always 0: in PortraitInverted the bottom strip lies across the logical top, and in
// landscape the side boxes can. A header placed at a bare y lands under that strip, and a body
// placed at a bare y lands in the header once the header outgrows the constant -- headerHeight
// grows with the UI font size, and Lyra's is nearly twice Base's (the OPDS book detail drew its
// cover and title over the header that way). Both are measured from content.y here instead.
//
// Pure arithmetic on the theme metrics, so it is host-testable.
namespace HeaderLayout {

// What getContentRect() takes from the content for a column of hint boxes on its logical left or
// right edge -- the side keys' boxes in portrait, the front buttons' strip in landscape: the column,
// and HINT_COLUMN_GAP beside it. Without the gap, rows, rules and tab bars that span the content ran
// right up to the boxes' border (the reader menu's tab bar and the Starred Pages header rule in
// landscape), and in Base, whose side boxes sit 4 px in from the panel edge inside their 30 px
// column, 4 px under them. A strip across the top or bottom needs no gap: the header's topPadding
// and the bodies' own spacing already keep clear of it.
inline constexpr int HINT_COLUMN_GAP = 8;

inline int columnReserve(const int columnWidth) { return columnWidth > 0 ? columnWidth + HINT_COLUMN_GAP : 0; }

// The rest of `content` from `top` down.
inline Rect below(const Rect& content, const int top) {
  return Rect{content.x, top, content.width, content.y + content.height - top};
}

// The band BaseTheme::drawHeader() draws in: one topPadding into the content area, headerHeight tall.
inline Rect header(const Rect& content, const ThemeMetrics& metrics) {
  return Rect{content.x, content.y + metrics.topPadding, content.width, metrics.headerHeight};
}

// The body under that header, after the theme's verticalSpacing.
inline Rect body(const Rect& content, const ThemeMetrics& metrics) {
  const Rect band = header(content, metrics);
  return below(content, band.y + band.height + metrics.verticalSpacing);
}

// A header subtitle counts when it has text: a null or empty one reserves nothing.
inline bool hasText(const char* text) { return text != nullptr && text[0] != '\0'; }

// The line Base draws a header subtitle in: the foot of `area`, which is the content with every
// hint strip out (getContentRect(renderer, true, true)), so the line clears them in any orientation
// and on any panel height. It used to be a fixed y = 738, the foot of the X4's portrait content: on
// the X3 that ran into the hint strip, on the T5S3 it landed mid-body, and in landscape off screen.
// Empty (height 0) for a theme that draws the subtitle in the band.
inline Rect subtitleLine(const Rect& area, const ThemeMetrics& metrics) {
  return Rect{area.x, area.y + area.height - metrics.headerSubtitleLine, area.width, metrics.headerSubtitleLine};
}

// `body` ending where `line` starts, if that is higher: a body under a header with a subtitle stops
// above the subtitle's line.
inline Rect above(const Rect& body, const Rect& line) {
  const int bottom = std::min(body.y + body.height, line.y);
  return Rect{body.x, body.y, body.width, bottom - body.y};
}

// The reader's own overlays (the menu, the chapter and footnote lists, the pickers) draw a bare bold
// title instead of the theme header: no clock or battery over the book. It sits OVERLAY_TITLE_TOP
// into the content area, and a body under it keeps OVERLAY_TITLE_GAP from the title's line.
inline constexpr int OVERLAY_TITLE_TOP = 15;
inline constexpr int OVERLAY_TITLE_GAP = 5;

inline int overlayTitleY(const Rect& content) { return content.y + OVERLAY_TITLE_TOP; }

// Where the title's line ends. `titleLineHeight` is the title font's line height, which grows with
// the UI font size.
inline int overlayTitleBottom(const Rect& content, const int titleLineHeight) {
  return overlayTitleY(content) + titleLineHeight;
}

inline Rect overlayBody(const Rect& content, const int titleLineHeight) {
  return below(content, overlayTitleBottom(content, titleLineHeight) + OVERLAY_TITLE_GAP);
}

}  // namespace HeaderLayout
