#pragma once

#include <FreeInkUIGfxRenderer.h>

#include "UITheme.h"

inline freeink::ui::ThemeTokens uiThemeTokens(const freeink::ui::GfxRendererTarget& target) {
  namespace fui = freeink::ui;
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  fui::ThemeTokens tokens = fui::themeTokensForLineHeight(target.lineHeight(fui::GfxRendererTarget::FONT_BODY));
  tokens.headerHeight = static_cast<int16_t>(metrics.headerHeight);
  tokens.footerHeight = static_cast<int16_t>(metrics.buttonHintsHeight);
  tokens.listSidePadding = static_cast<int16_t>(metrics.contentSidePadding);
  tokens.listScrollWidth = static_cast<int16_t>(metrics.scrollBarWidth);
  tokens.listScrollInset = static_cast<int16_t>(metrics.scrollBarRightOffset);
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  tokens.listTouchMinRowHeight = static_cast<int16_t>(metrics.listRowHeight);
#endif
  return tokens;
}
