#pragma once

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
