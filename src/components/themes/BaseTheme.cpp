#include "BaseTheme.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "I18n.h"
#include "RecentBooksStore.h"
#include "components/ListLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int homeMenuMargin = 20;
constexpr int homeMarginTop = 30;
constexpr int subtitleY = 738;

// Helper: draw battery icon at given position
void drawBatteryIcon(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight, uint16_t percentage) {
  // Draw battery outline (shared code)
  BaseTheme::drawBatteryOutline(renderer, x, y, battWidth, rectHeight);

  const bool charging = gpio.isUsbConnected();

  // The +1 is to round up, so that we always fill at least one pixel
  const int maxFillWidth = battWidth - 5;
  const int fillHeight = rectHeight - 4;
  if (maxFillWidth <= 0 || fillHeight <= 0) {
    return;
  }
  int filledWidth = percentage * maxFillWidth / 100 + 1;
  if (filledWidth > maxFillWidth) {
    filledWidth = maxFillWidth;
  }

  // When charging, ensure minimum fill so lightning bolt is fully visible
  constexpr int minFillForBolt = 8;
  if (charging && filledWidth < minFillForBolt) {
    filledWidth = std::min(minFillForBolt, maxFillWidth);
  }

  renderer.fillRect(x + 2, y + 2, filledWidth, fillHeight);

  // Draw lightning bolt when charging (white/inverted on black fill for visibility)
  if (charging) {
    BaseTheme::drawBatteryLightningBolt(renderer, x + 4, y + 2);
  }
}

int progressBarPixelHeight(const uint8_t progressBar, const uint8_t thickness, const ThemeMetrics& metrics) {
  const int reservedHeight = UITheme::getProgressBarHeight(progressBar, thickness);
  return std::max(0, reservedHeight - metrics.progressBarMarginTop);
}

int statusBarProgressPercent(const uint8_t progressBar, const float bookProgress, const int currentPage,
                             const int pageCount) {
  if (progressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS) {
    return std::clamp(static_cast<int>(std::lround(bookProgress)), 0, 100);
  }
  const int chapterProgress =
      (pageCount > 0) ? static_cast<int>((static_cast<float>(currentPage) / pageCount) * 100) : 0;
  return std::clamp(chapterProgress, 0, 100);
}
}  // namespace

void BaseTheme::drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight) {
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rectHeight - 5);
}

void BaseTheme::drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY) {
  // Draw lightning bolt (white/inverted on black fill for visibility)
  renderer.drawLine(boltX + 4, boltY + 0, boltX + 5, boltY + 0, false);
  renderer.drawLine(boltX + 3, boltY + 1, boltX + 4, boltY + 1, false);
  renderer.drawLine(boltX + 2, boltY + 2, boltX + 5, boltY + 2, false);
  renderer.drawLine(boltX + 3, boltY + 3, boltX + 4, boltY + 3, false);
  renderer.drawLine(boltX + 2, boltY + 4, boltX + 3, boltY + 4, false);
  renderer.drawLine(boltX + 1, boltY + 5, boltX + 4, boltY + 5, false);
  renderer.drawLine(boltX + 2, boltY + 6, boltX + 3, boltY + 6, false);
  renderer.drawLine(boltX + 1, boltY + 7, boltX + 2, boltY + 7, false);
}

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + BaseTheme::batteryPercentSpacing + BaseMetrics::values.batteryWidth,
                      rect.y, percentageText.c_str());
  }

  drawBatteryIcon(renderer, rect.x, y, BaseMetrics::values.batteryWidth, rect.height, percentage);
}

int BaseTheme::statusBarBatteryWidth(const GfxRenderer& renderer, const ThemeMetrics& metrics,
                                     const bool showPercentage) {
  int width = metrics.batteryWidth;
  if (showPercentage) {
    char percentageText[8];
    snprintf(percentageText, sizeof(percentageText), "%u%%",
             static_cast<unsigned>(powerManager.getBatteryPercentage()));
    width += batteryPercentSpacing + renderer.getTextWidth(SMALL_FONT_ID, percentageText);
  }
  return width;
}

void BaseTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Right aligned: percentage on left, icon on right (UI headers)
  // rect.x is already positioned for the icon (drawHeader calculated it)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
    // Clear the area where we're going to draw the text to prevent ghosting
    const auto textHeight = renderer.getTextHeight(SMALL_FONT_ID);
    renderer.fillRect(rect.x - textWidth - BaseTheme::batteryPercentSpacing, rect.y, textWidth, textHeight, false);
    // Draw text to the left of the icon
    renderer.drawText(SMALL_FONT_ID, rect.x - textWidth - BaseTheme::batteryPercentSpacing, rect.y,
                      percentageText.c_str());
  }

  // Icon is already at correct position from rect.x
  drawBatteryIcon(renderer, rect.x, y, BaseMetrics::values.batteryWidth, rect.height, percentage);
}

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current,
                                const size_t total) const {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  LOG_DBG("UI", "Drawing progress bar: current=%u, total=%u, percent=%d", current, total, percent);
  // Draw outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

void BaseTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  // The strip is laid out in panel coordinates so each box lands beside the button it names. In
  // PortraitInverted the panel is simply turned 180°, so drawing it inverted instead of upright
  // keeps the boxes on the same panel edge AND lets the reader read the labels — drawing them
  // upright there put every word upside down. (The landscape modes are a different problem: their
  // strip is a vertical column, which needs rotated text rather than a rotated page.)
  const bool inverted = orig_orientation == GfxRenderer::Orientation::PortraitInverted;
  renderer.setDrawOrientation(inverted ? GfxRenderer::Orientation::PortraitInverted
                                       : GfxRenderer::Orientation::Portrait);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 106;
  constexpr int buttonHeight = BaseMetrics::values.buttonHintsHeight;
  constexpr int buttonY = BaseMetrics::values.buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  // X3 has wider screen in portrait (528 vs 480), use more spacing
  constexpr int x4ButtonPositions[] = {25, 130, 245, 350};
  constexpr int x3ButtonPositions[] = {38, 154, 268, 384};
  const int* buttonPositions = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  // Inverted flips both axes, so the strip's panel-bottom band becomes the top one and each slot
  // mirrors across the width. Labels keep their hardware index — the mirroring is what carries
  // each box back to its own button.
  const int stripY = inverted ? 0 : pageHeight - buttonY;

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = inverted ? pageWidth - buttonPositions[i] - buttonWidth : buttonPositions[i];
      renderer.fillRect(x, stripY, buttonWidth, buttonHeight, false);
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, stripY + textYOffset, labels[i]);
      renderer.drawRect(x, stripY, buttonWidth, buttonHeight);
    }
  }

  renderer.setDrawOrientation(orig_orientation);
}

void BaseTheme::drawSideButtonHints(GfxRenderer& renderer, const char* upBtn, const char* downBtn) const {
  // Like drawButtonHints: the geometry below is written in PANEL coordinates, so each box stays
  // beside the button it names whichever way the device is held. The side buttons sit on the
  // panel's right edge (BTN_UP nearest the top) — which is the edge getContentRect() reserves in
  // every orientation, while the logical right edge is a different one in three of the four.
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  const bool inverted = orig_orientation == GfxRenderer::Orientation::PortraitInverted;
  renderer.setDrawOrientation(inverted ? GfxRenderer::Orientation::PortraitInverted
                                       : GfxRenderer::Orientation::Portrait);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = BaseMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 4;

  // Panel -> drawing space. Upright, that is the identity; inverted, both axes mirror, and these
  // three wrappers are the only place that has to know. Text is anchored at the left edge of its
  // rotated column and at the END of the run (it extends upwards), so its anchor mirrors to the
  // opposite corner of the box it occupies rather than point-for-point.
  const auto mx = [&](const int x) { return inverted ? screenWidth - 1 - x : x; };
  const auto my = [&](const int y) { return inverted ? screenHeight - 1 - y : y; };
  const auto line = [&](const int x1, const int y1, const int x2, const int y2) {
    renderer.drawLine(mx(x1), my(y1), mx(x2), my(y2));
  };
  const auto rect = [&](const int x, const int y, const int w, const int h) {
    renderer.drawRect(inverted ? screenWidth - x - w : x, inverted ? screenHeight - y - h : y, w, h);
  };
  const auto textCW = [&](const int x, const int y, const char* text) {
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, text);
    const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
    renderer.drawTextRotated90CW(SMALL_FONT_ID, inverted ? screenWidth - x - textHeight : x,
                                 inverted ? screenHeight - 1 - y + textWidth : y, text);
  };

  if (gpio.deviceIsX3()) {
    // X3 layout: Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (upBtn != nullptr && upBtn[0] != '\0') {
      const int leftX = buttonMargin;
      rect(leftX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, upBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      textCW(leftX + (buttonWidth - textHeight) / 2, x3ButtonY + (buttonHeight + textWidth) / 2, upBtn);
    }

    if (downBtn != nullptr && downBtn[0] != '\0') {
      const int rightX = screenWidth - buttonMargin - buttonWidth;
      rect(rightX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, downBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      textCW(rightX + (buttonWidth - textHeight) / 2, x3ButtonY + (buttonHeight + textWidth) / 2, downBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    constexpr int topButtonY = 345;
    const char* labels[] = {upBtn, downBtn};
    const int x = screenWidth - buttonMargin - buttonWidth;

    if (upBtn != nullptr && upBtn[0] != '\0') {
      line(x, topButtonY, x + buttonWidth - 1, topButtonY);
      line(x, topButtonY, x, topButtonY + buttonHeight - 1);
      line(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);
    }

    if ((upBtn != nullptr && upBtn[0] != '\0') || (downBtn != nullptr && downBtn[0] != '\0')) {
      line(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);
    }

    if (downBtn != nullptr && downBtn[0] != '\0') {
      line(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);
      line(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + 2 * buttonHeight - 1);
      line(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1, topButtonY + 2 * buttonHeight - 1);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topButtonY + i * buttonHeight;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
        textCW(x + (buttonWidth - textHeight) / 2, y + (buttonHeight + textWidth) / 2, labels[i]);
      }
    }
  }

  renderer.setDrawOrientation(orig_orientation);
}

void BaseTheme::drawListOverflowArrows(const GfxRenderer& renderer, const Rect rect) {
  constexpr int indicatorWidth = 20;
  constexpr int arrowSize = 6;
  constexpr int margin = 15;  // Offset from right edge

  const int centerX = rect.x + rect.width - indicatorWidth / 2 - margin;
  const int indicatorTop = rect.y;  // Offset to avoid overlapping side button hints
  const int indicatorBottom = rect.y + rect.height - arrowSize;

  // Draw up arrow at top (^) - narrow point at top, wide base at bottom
  for (int i = 0; i < arrowSize; ++i) {
    const int lineWidth = 1 + i * 2;
    const int startX = centerX - i;
    renderer.drawLine(startX, indicatorTop + i, startX + lineWidth - 1, indicatorTop + i);
  }

  // Draw down arrow at bottom (v) - wide base at top, narrow point at bottom
  for (int i = 0; i < arrowSize; ++i) {
    const int lineWidth = 1 + (arrowSize - 1 - i) * 2;
    const int startX = centerX - (arrowSize - 1 - i);
    renderer.drawLine(startX, indicatorBottom - arrowSize + 1 + i, startX + lineWidth - 1,
                      indicatorBottom - arrowSize + 1 + i);
  }
}

// Variable-height rows: a title too long for one line is wrapped over up to
// view.maxTitleLines lines and its row grows by one line height per extra line. Shared by every
// theme — see BaseTheme::wrappedListStyle() for the per-theme look.
void BaseTheme::drawWrappedList(const GfxRenderer& renderer, const Rect rect, const int itemCount,
                                const int selectedIndex, const std::function<std::string(int index)>& rowTitle,
                                const std::function<UIIcon(int index)>& rowIcon, ListViewState& view) const {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const WrappedListStyle style = wrappedListStyle();
  const int baseRowHeight = metrics.listRowHeight;
  if (itemCount <= 0 || rect.height < baseRowHeight || rowTitle == nullptr) {
    view.visibleRows = 0;
    return;
  }

  const int maxLines = std::min(view.maxTitleLines, maxWrappedTitleLines);
  const int lineStep = renderer.getLineHeight(UI_10_FONT_ID);

  // Reserve the scroll bar strip unconditionally. Whether the list scrolls depends on the wrapped
  // row heights, which depend on the text width, which would depend on the bar — reserving first
  // breaks that circle, and the bar is still only DRAWN when the rows really do not all fit.
  const int contentWidth = rect.width - (style.scrollBarWidth + style.scrollBarRightOffset);
  const int iconGap = (rowIcon != nullptr && style.iconSize > 0) ? style.iconSize + style.hPadding : 0;
  const int textX = rect.x + metrics.contentSidePadding + style.hPadding + iconGap;
  const int textWidth = contentWidth - metrics.contentSidePadding * 2 - style.hPadding * 2 - iconGap;
  if (textWidth <= 0) {
    view.visibleRows = 0;
    return;
  }

  // Separators keep the plain row height; only real titles can grow.
  const auto rowHeightFor = [&](const int index) {
    const std::string title = rowTitle(index);
    if (UITheme::isSeparatorTitle(title)) return baseRowHeight;
    const int lines = static_cast<int>(renderer.wrappedText(UI_10_FONT_ID, title.c_str(), textWidth, maxLines).size());
    return baseRowHeight + (lines > 1 ? (lines - 1) * lineStep : 0);
  };

  const ListLayout::Window window =
      ListLayout::computeWindow(itemCount, selectedIndex, rect.height, view.firstVisible, rowHeightFor);
  view.visibleRows = window.count;
  if (window.count <= 0) return;

  // Scroll position: how far the window's top row is through the rows that can be a top row.
  if (window.count < itemCount) {
    if (style.scrollBarWidth > 0) {
      const int barHeight = std::max(8, rect.height * window.count / itemCount);
      const int barY = rect.y + ((rect.height - barHeight) * window.first) / (itemCount - window.count);
      const int barX = rect.x + rect.width - style.scrollBarRightOffset;
      renderer.drawLine(barX, rect.y, barX, rect.y + rect.height, true);
      renderer.fillRect(barX - style.scrollBarWidth, barY, style.scrollBarWidth, barHeight, true);
    } else {
      drawListOverflowArrows(renderer, rect);
    }
  }

  for (int row = 0; row < window.count; row++) {
    const int index = window.first + row;
    const int rowY = rect.y + window.top[row];
    const int rowHeight = window.height[row];

    std::string title = rowTitle(index);
    if (UITheme::isSeparatorTitle(title)) {
      title = UITheme::stripSeparatorTitle(title);
      drawListSeparator(
          renderer,
          Rect{rect.x + metrics.contentSidePadding, rowY, contentWidth - metrics.contentSidePadding * 2, rowHeight},
          textX, textWidth, title);
      continue;
    }

    const bool selected = (index == selectedIndex);
    if (selected) {
      const int selX = style.fullWidthSelection ? rect.x : rect.x + metrics.contentSidePadding;
      const int selWidth = style.fullWidthSelection ? rect.width : contentWidth - metrics.contentSidePadding * 2;
      if (style.cornerRadius > 0) {
        renderer.fillRoundedRect(selX, rowY, selWidth, rowHeight, style.cornerRadius,
                                 style.selectionIsBlack ? Color::Black : Color::LightGray);
      } else {
        renderer.fillRect(selX, rowY, selWidth, rowHeight);
      }
    }
    // Only a black fill needs the text knocked out of it; a light-gray one keeps black text.
    const bool textBlack = !(selected && style.selectionIsBlack);

    int lineY = rowY + style.titleTextOffsetY;
    for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, title.c_str(), textWidth, maxLines)) {
      renderer.drawText(UI_10_FONT_ID, textX, lineY, line.c_str(), textBlack);
      lineY += lineStep;
    }

    if (iconGap > 0) {
      // Aligned with the first line, not the middle of a grown row, so icons stay on one baseline.
      const uint8_t* iconBitmap = rowIconBitmap(rowIcon(index), style.iconSize);
      if (iconBitmap != nullptr) {
        const int iconX = rect.x + metrics.contentSidePadding + style.hPadding;
        const int iconY = rowY + (baseRowHeight - style.iconSize) / 2;
        if (textBlack) {
          renderer.drawIcon(iconBitmap, iconX, iconY, style.iconSize, style.iconSize);
        } else {
          renderer.drawIconInverted(iconBitmap, iconX, iconY, style.iconSize, style.iconSize);
        }
      }
    }
  }
}

void BaseTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         ListViewState* view) const {
  // Wrapping only covers title (+ icon) rows: a subtitle or a value column has its own fixed
  // geometry inside the row, so those lists stay on the classic fixed-height path.
  if (view != nullptr && view->wraps() && rowSubtitle == nullptr && rowValue == nullptr) {
    drawWrappedList(renderer, rect, itemCount, selectedIndex, rowTitle, rowIcon, *view);
    return;
  }
  int rowHeight =
      (rowSubtitle != nullptr) ? BaseMetrics::values.listWithSubtitleRowHeight : BaseMetrics::values.listRowHeight;
  int pageItems = rect.height / rowHeight;
  // A fixed-height list still reports its page size, so Left/Right page by what is really on
  // screen rather than by a guess.
  if (view != nullptr) view->visibleRows = std::min(pageItems, itemCount);

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    drawListOverflowArrows(renderer, rect);
  }

  bool selectedIsSeparator = false;
  if (selectedIndex >= 0 && selectedIndex < itemCount) {
    selectedIsSeparator = UITheme::isSeparatorTitle(rowTitle(selectedIndex));
  }

  // Draw selection
  int contentWidth = rect.width - 5;
  if (selectedIndex >= 0 && !selectedIsSeparator) {
    renderer.fillRect(rect.x, rect.y + selectedIndex % pageItems * rowHeight - 2, rect.width, rowHeight);
  }
  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    int textWidth = contentWidth - BaseMetrics::values.contentSidePadding * 2 - (rowValue != nullptr ? 60 : 0);

    // Draw name
    auto itemName = rowTitle(i);
    const bool isSeparator = UITheme::isSeparatorTitle(itemName);
    if (isSeparator) {
      itemName = UITheme::stripSeparatorTitle(itemName);
      drawListSeparator(renderer,
                        Rect{rect.x + BaseMetrics::values.contentSidePadding, itemY,
                             contentWidth - BaseMetrics::values.contentSidePadding * 2, rowHeight},
                        rect.x + BaseMetrics::values.contentSidePadding, textWidth, itemName);
      continue;
    }

    auto font = (rowSubtitle != nullptr) ? UI_12_FONT_ID : UI_10_FONT_ID;
    auto item = renderer.truncatedText(font, itemName.c_str(), textWidth);
    renderer.drawText(font, rect.x + BaseMetrics::values.contentSidePadding, itemY, item.c_str(), i != selectedIndex);

    if (rowSubtitle != nullptr) {
      // Draw subtitle; if the text is newline-separated (author\nseries), join with • for single-line display
      std::string subtitleText = rowSubtitle(i);
      const auto nl = subtitleText.find('\n');
      if (nl != std::string::npos) {
        subtitleText.replace(nl, 1, " \u2022 ");
      }
      auto subtitle = renderer.truncatedText(UI_10_FONT_ID, subtitleText.c_str(), textWidth);
      renderer.drawText(UI_10_FONT_ID, rect.x + BaseMetrics::values.contentSidePadding, itemY + 30, subtitle.c_str(),
                        i != selectedIndex);
    }

    if (rowValue != nullptr) {
      // Draw value
      std::string valueText = rowValue(i);
      const auto valueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - BaseMetrics::values.contentSidePadding - valueTextWidth,
                        itemY, valueText.c_str(), i != selectedIndex);
    }
  }
}

void BaseTheme::drawListSeparator(const GfxRenderer& renderer, Rect rowRect, int textX, int textWidth,
                                  const std::string& title) const {
  const std::string item = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), textWidth);
  const int lineY = rowRect.y + rowRect.height - 2;
  renderer.drawLine(rowRect.x, lineY, rowRect.x + rowRect.width - 1, lineY, true);
  renderer.drawText(SMALL_FONT_ID, textX, rowRect.y + 7, item.c_str(), true, EpdFontFamily::BOLD);
}

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  // Hide last battery draw
  constexpr int maxBatteryWidth = 80;
  renderer.fillRect(rect.x + rect.width - maxBatteryWidth, rect.y + 5, maxBatteryWidth,
                    BaseMetrics::values.batteryHeight + 10, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  const int batteryX = rect.x + rect.width - 12 - BaseMetrics::values.batteryWidth;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 5, BaseMetrics::values.batteryWidth, BaseMetrics::values.batteryHeight},
                   showBatteryPercentage);

  // Draw clock in header
  if (SETTINGS.useClock) {
    char clockStr[16];
    HalClock::formatTime(clockStr, sizeof(clockStr), !SETTINGS.clockFormat12h);
    renderer.drawText(SMALL_FONT_ID, rect.x + BaseMetrics::values.contentSidePadding, rect.y + 5, clockStr);
  }

  if (title) {
    int padding = rect.width - batteryX + BaseMetrics::values.batteryWidth;
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title,
                                                 rect.width - padding * 2 - BaseMetrics::values.contentSidePadding * 2,
                                                 EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, rect.y + 5, truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(
        SMALL_FONT_ID, subtitle, rect.width - BaseMetrics::values.contentSidePadding * 2, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    renderer.drawText(SMALL_FONT_ID,
                      rect.x + rect.width - BaseMetrics::values.contentSidePadding - truncatedSubtitleWidth, subtitleY,
                      truncatedSubtitle.c_str(), true);
  }
}

void BaseTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  constexpr int underlineHeight = 2;  // Height of selection underline
  constexpr int underlineGap = 4;     // Gap between text and underline
  constexpr int maxListValueWidth = 200;

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;
  int rightSpace = BaseMetrics::values.contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - BaseMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + 10;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_12_FONT_ID, label, rect.width - BaseMetrics::values.contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_12_FONT_ID, currentX, rect.y, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);
}

void BaseTheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  constexpr int underlineHeight = 2;  // Height of selection underline
  constexpr int underlineGap = 4;     // Gap between text and underline

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;

  for (const auto& tab : tabs) {
    const int textWidth =
        renderer.getTextWidth(UI_12_FONT_ID, tab.label, tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

    // Draw underline for selected tab
    if (tab.selected) {
      if (selected) {
        renderer.fillRect(currentX - 3, rect.y, textWidth + 6, lineHeight + underlineGap);
      } else {
        renderer.fillRect(currentX, rect.y + lineHeight + underlineGap, textWidth, underlineHeight);
      }
    }

    // Draw tab label
    renderer.drawText(UI_12_FONT_ID, currentX, rect.y, tab.label, !(tab.selected && selected),
                      tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

    currentX += textWidth + BaseMetrics::values.tabSpacing;
  }
}

// Draw the "Recent Book" cover card on the home screen
// TODO: Refactor method to make it cleaner, split into smaller methods
void BaseTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const bool hasContinueReading = !recentBooks.empty();
  const bool bookSelected = hasContinueReading && selectorIndex == 0;

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  // When there's no cover image, use fixed size (half screen)
  // When there's cover image, adapt width to image aspect ratio, keep height fixed at 400px
  const int baseHeight = rect.height;  // Fixed height (400px)

  int bookWidth, bookX;
  bool hasCoverImage = false;

  if (hasContinueReading && !recentBooks[0].coverBmpPath.empty()) {
    // Try to get actual image dimensions from BMP header
    const std::string coverBmpPath =
        UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

    FsFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      // Never draw the no-cover marker: scaling a 1x1 BMP into the slot paints a solid block.
      // Falling through to the existing no-cover branch shows the theme's own tile instead.
      if (bitmap.parseHeaders() == BmpReaderError::Ok &&
          !UITheme::isCoverPlaceholderBmp(bitmap.getWidth(), bitmap.getHeight())) {
        hasCoverImage = true;
        const int imgWidth = bitmap.getWidth();
        const int imgHeight = bitmap.getHeight();

        // Calculate width based on aspect ratio, maintaining baseHeight
        if (imgWidth > 0 && imgHeight > 0) {
          const float aspectRatio = static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
          bookWidth = static_cast<int>(baseHeight * aspectRatio);

          // Ensure width doesn't exceed reasonable limits (max 90% of screen width)
          const int maxWidth = static_cast<int>(rect.width * 0.9f);
          if (bookWidth > maxWidth) {
            bookWidth = maxWidth;
          }
        } else {
          bookWidth = rect.width / 2;  // Fallback
        }
      }
      file.close();
    }
  }

  if (!hasCoverImage) {
    // No cover: use half screen size
    bookWidth = rect.width / 2;
  }

  bookX = rect.x + (rect.width - bookWidth) / 2;
  const int bookY = rect.y;
  const int bookHeight = baseHeight;

  // Bookmark dimensions (used in multiple places)
  const int bookmarkWidth = bookWidth / 8;
  const int bookmarkHeight = bookHeight / 5;
  const int bookmarkX = bookX + bookWidth - bookmarkWidth - 10;
  const int bookmarkY = bookY + 5;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  {
    // Draw cover image as background if available (inside the box)
    // Only load from SD on first render, then use stored buffer

    if (hasContinueReading && !recentBooks[0].coverBmpPath.empty() && !coverRendered) {
      const std::string coverBmpPath =
          UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

      // First time: load cover from SD and render
      FsFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        // Never draw the no-cover marker: scaling a 1x1 BMP into the slot paints a solid block.
        // Falling through to the existing no-cover branch shows the theme's own tile instead.
        if (bitmap.parseHeaders() == BmpReaderError::Ok &&
            !UITheme::isCoverPlaceholderBmp(bitmap.getWidth(), bitmap.getHeight())) {
          LOG_DBG("THEME", "Rendering bmp");

          renderer.fillRect(bookX, bookY, bookWidth, bookHeight, false);
          renderer.drawBitmap(bitmap, bookX, bookY, bookWidth, bookHeight);

          // Draw border around the card
          renderer.drawRect(bookX, bookY, bookWidth, bookHeight);

          // No bookmark ribbon when cover is shown - it would just cover the art

          // Store the buffer with cover image for fast navigation
          coverBufferStored = storeCoverBuffer();
          coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer

          // First render: if selected, draw selection indicators now
          if (bookSelected) {
            LOG_DBG("THEME", "Drawing selection");
            renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
            renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
          }
        }
        file.close();
      }
    }

    if (!bufferRestored && !coverRendered) {
      // No cover image: draw border or fill, plus bookmark as visual flair
      if (bookSelected) {
        renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
      } else {
        renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
      }

      // Draw bookmark ribbon when no cover image (visual decoration)
      if (hasContinueReading) {
        const int notchDepth = bookmarkHeight / 3;
        const int centerX = bookmarkX + bookmarkWidth / 2;

        const int xPoints[5] = {
            bookmarkX,                  // top-left
            bookmarkX + bookmarkWidth,  // top-right
            bookmarkX + bookmarkWidth,  // bottom-right
            centerX,                    // center notch point
            bookmarkX                   // bottom-left
        };
        const int yPoints[5] = {
            bookmarkY,                                // top-left
            bookmarkY,                                // top-right
            bookmarkY + bookmarkHeight,               // bottom-right
            bookmarkY + bookmarkHeight - notchDepth,  // center notch point
            bookmarkY + bookmarkHeight                // bottom-left
        };

        // Draw bookmark ribbon (inverted if selected)
        renderer.fillPolygon(xPoints, yPoints, 5, !bookSelected);
      }
    }

    // If buffer was restored, draw selection indicators if needed
    if (bufferRestored && bookSelected && coverRendered) {
      // Draw selection border (no bookmark inversion needed since cover has no bookmark)
      renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
      renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
    } else if (!coverRendered && !bufferRestored) {
      // Selection border already handled above in the no-cover case
    }
  }

  if (hasContinueReading) {
    const std::string& lastBookTitle = recentBooks[0].title;
    const std::string& lastBookAuthor = recentBooks[0].author;
    const std::string& lastBookSeries = recentBooks[0].series;

    // Invert text colors based on selection state:
    // - With cover: selected = white text on black box, unselected = black text on white box
    // - Without cover: selected = white text on black card, unselected = black text on white card

    auto lines = renderer.wrappedText(UI_12_FONT_ID, lastBookTitle.c_str(), bookWidth - 40, 3);

    // Book title text
    int totalTextHeight = renderer.getLineHeight(UI_12_FONT_ID) * static_cast<int>(lines.size());
    if (!lastBookAuthor.empty()) {
      totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    }
    if (!lastBookSeries.empty()) {
      totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID);
    }

    // Vertically center the title block within the card
    int titleYStart = bookY + (bookHeight - totalTextHeight) / 2;

    const auto truncatedAuthor = lastBookAuthor.empty()
                                     ? std::string{}
                                     : renderer.truncatedText(UI_10_FONT_ID, lastBookAuthor.c_str(), bookWidth - 40);
    const auto truncatedSeries = lastBookSeries.empty()
                                     ? std::string{}
                                     : renderer.truncatedText(UI_10_FONT_ID, lastBookSeries.c_str(), bookWidth - 40);

    // If cover image was rendered, draw box behind title, author, and series
    if (coverRendered) {
      constexpr int boxPadding = 8;
      // Calculate the max text width for the box
      int maxTextWidth = 0;
      for (const auto& line : lines) {
        const int lineWidth = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
        if (lineWidth > maxTextWidth) {
          maxTextWidth = lineWidth;
        }
      }
      if (!truncatedAuthor.empty()) {
        const int authorWidth = renderer.getTextWidth(UI_10_FONT_ID, truncatedAuthor.c_str());
        if (authorWidth > maxTextWidth) {
          maxTextWidth = authorWidth;
        }
      }
      if (!truncatedSeries.empty()) {
        const int seriesWidth = renderer.getTextWidth(UI_10_FONT_ID, truncatedSeries.c_str());
        if (seriesWidth > maxTextWidth) {
          maxTextWidth = seriesWidth;
        }
      }

      const int boxWidth = maxTextWidth + boxPadding * 2;
      const int boxHeight = totalTextHeight + boxPadding * 2;
      const int boxX = rect.x + (rect.width - boxWidth) / 2;
      const int boxY = titleYStart - boxPadding;

      // Draw box (inverted when selected: black box instead of white)
      renderer.fillRect(boxX, boxY, boxWidth, boxHeight, bookSelected);
      // Draw border around the box (inverted when selected: white border instead of black)
      renderer.drawRect(boxX, boxY, boxWidth, boxHeight, !bookSelected);
    }

    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, titleYStart, line.c_str(), !bookSelected);
      titleYStart += renderer.getLineHeight(UI_12_FONT_ID);
    }

    if (!truncatedAuthor.empty()) {
      titleYStart += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, titleYStart, truncatedAuthor.c_str(), !bookSelected);
      titleYStart += renderer.getLineHeight(UI_10_FONT_ID);
    }

    if (!truncatedSeries.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, titleYStart, truncatedSeries.c_str(), !bookSelected);
    }

    // "Continue Reading" label at the bottom
    const int continueY = bookY + bookHeight - renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    if (coverRendered) {
      // Draw box behind "Continue Reading" text (inverted when selected: black box instead of white)
      const char* continueText = tr(STR_CONTINUE_READING);
      const int continueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, continueText);
      constexpr int continuePadding = 6;
      const int continueBoxWidth = continueTextWidth + continuePadding * 2;
      const int continueBoxHeight = renderer.getLineHeight(UI_10_FONT_ID) + continuePadding;
      const int continueBoxX = rect.x + (rect.width - continueBoxWidth) / 2;
      const int continueBoxY = continueY - continuePadding / 2;
      renderer.fillRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, bookSelected);
      renderer.drawRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, !bookSelected);
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, continueText, !bookSelected);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, tr(STR_CONTINUE_READING), !bookSelected);
    }
  } else {
    // No book to continue reading
    const int y =
        bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, "No open book");
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), "Start reading below");
  }
}

void BaseTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  int rowHeight = BaseMetrics::values.menuRowHeight;
  int rowSpacing = BaseMetrics::values.menuSpacing;
  if (buttonCount > 0 && rect.height > 0) {
    const int defaultHeight = buttonCount * rowHeight + std::max(0, buttonCount - 1) * rowSpacing;
    if (defaultHeight > rect.height) {
      rowSpacing = std::max(0, (rect.height - buttonCount * rowHeight) / std::max(1, buttonCount - 1));
      if (buttonCount * rowHeight + std::max(0, buttonCount - 1) * rowSpacing > rect.height) {
        rowHeight = std::max(30, (rect.height - std::max(0, buttonCount - 1) * rowSpacing) / buttonCount);
      }
      if (buttonCount * rowHeight + std::max(0, buttonCount - 1) * rowSpacing > rect.height) {
        rowHeight = std::max(1, rect.height / buttonCount);
        rowSpacing = 0;
      }
    }
  }

  for (int i = 0; i < buttonCount; ++i) {
    const int tileY = rect.y + static_cast<int>(i) * (rowHeight + rowSpacing);

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRect(rect.x + BaseMetrics::values.contentSidePadding, tileY,
                        rect.width - BaseMetrics::values.contentSidePadding * 2, rowHeight);
    } else {
      renderer.drawRect(rect.x + BaseMetrics::values.contentSidePadding, tileY,
                        rect.width - BaseMetrics::values.contentSidePadding * 2, rowHeight);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = rect.x + (rect.width - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY = tileY + (rowHeight - lineHeight) / 2;  // vertically centered assuming y is top of text
    // Invert text when the tile is selected, to contrast with the filled background
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, selectedIndex != i);
  }
}

Rect BaseTheme::drawPopup(const GfxRenderer& renderer, const char* message, const bool overlayDisplayedFrame) const {
  // Re-seed the write buffer from the frame on screen so the box overlays current content, not the
  // stale two-refreshes-ago frame left by the last buffer swap. Compose-then-popup callers skip this.
  if (overlayDisplayedFrame) renderer.syncWriteBufferFromDisplayed();
  constexpr int margin = 15;
  constexpr int y = 60;
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::BOLD);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + margin * 2;
  const int h = textHeight + margin * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  renderer.fillRect(x - 2, y - 2, w + 4, h + 4, true);  // frame thickness 2
  renderer.fillRect(x, y, w, h, false);

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + margin - 2;
  renderer.drawText(UI_12_FONT_ID, textX, textY, message, true, EpdFontFamily::BOLD);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  constexpr int barHeight = 4;
  const int barWidth = layout.width - 30;  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - 10;

  int fillWidth = barWidth * progress / 100;

  renderer.fillRect(barX, barY, fillWidth, barHeight, true);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BaseTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                              const int pageCount, std::string title, const int paddingBottom, const bool isStarred,
                              const std::string& printedPageLabel, const bool fillMargin,
                              const bool pageCountApproximate) const {
  // While a section is still being laid out the total page count is a byte-based estimate, shown
  // with a leading "~" so the reader knows it will firm up as the chapter finishes building.
  const char* pageCountPrefix = pageCountApproximate ? "~" : "";
  auto metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  const auto screenHeight = renderer.getScreenHeight();
  const auto screenWidth = renderer.getScreenWidth();
  // fillMargin: extend bar edge-to-edge under the bezel, adapted from upstream PR #2138
  const int barMarginLeft = fillMargin ? 0 : orientedMarginLeft;
  const int barMarginRight = fillMargin ? 0 : orientedMarginRight;
  const int progressBarMaxWidth = screenWidth - barMarginLeft - barMarginRight;

  auto drawEdgeProgressBar = [&](const uint8_t progressBar, const uint8_t thickness, const bool topEdge) {
    const int barHeight = progressBarPixelHeight(progressBar, thickness, metrics);
    if (barHeight <= 0) {
      return;
    }
    const int progress = statusBarProgressPercent(progressBar, bookProgress, currentPage, pageCount);
    const int barWidth = progressBarMaxWidth * progress / 100;
    const int extraBottom = (!topEdge && fillMargin) ? orientedMarginBottom - 1 : 0;
    const int y =
        topEdge ? orientedMarginTop + paddingBottom : screenHeight - orientedMarginBottom - paddingBottom - barHeight;
    renderer.fillRect(barMarginLeft, y, barWidth, barHeight + extraBottom, true);
  };

  // Draw progress bars first; status items are then placed inside the reserved band for their selected edge.
  drawEdgeProgressBar(SETTINGS.statusBarUpperProgressBar, SETTINGS.statusBarUpperProgressBarThickness, true);
  drawEdgeProgressBar(SETTINGS.statusBarLowerProgressBar, SETTINGS.statusBarLowerProgressBarThickness, false);

  const bool hasProgressText = SETTINGS.statusBarBookProgressPercentage || SETTINGS.statusBarChapterPageCount;
  const bool hasStatusItems = hasProgressText || SETTINGS.statusBarBattery || !title.empty() ||
                              SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE ||
                              (SETTINGS.useClock && SETTINGS.statusBarClock) || !printedPageLabel.empty();
  if (!hasStatusItems) {
    return;
  }

  const bool statusItemsAtTop =
      SETTINGS.statusBarItemsPosition == CrossPointSettings::STATUS_BAR_ITEMS_POSITION::STATUS_BAR_ITEMS_TOP;
  const int adjacentProgressHeight = statusItemsAtTop
                                         ? UITheme::getProgressBarHeight(SETTINGS.statusBarUpperProgressBar,
                                                                         SETTINGS.statusBarUpperProgressBarThickness)
                                         : UITheme::getProgressBarHeight(SETTINGS.statusBarLowerProgressBar,
                                                                         SETTINGS.statusBarLowerProgressBarThickness);
  const int statusItemsHeight = UITheme::getStatusBarItemsHeight();

  const int textY = statusItemsAtTop ? orientedMarginTop + paddingBottom + adjacentProgressHeight + 4
                                     : screenHeight - orientedMarginBottom - paddingBottom - adjacentProgressHeight -
                                           statusItemsHeight + 4;

  constexpr int statusItemGap = 8;  // gap between adjacent items within one cluster
  constexpr int starGap = 6;        // the star sits tighter against the progress text

  // Resolve the clock before anything is placed: whichever end it sits on has to reserve its width
  // before the title is centred in what is left over.
  char clockStr[16] = "";
  int clockTextWidth = 0;
  if (SETTINGS.useClock && SETTINGS.statusBarClock) {
    HalClock::formatTime(clockStr, sizeof(clockStr), !SETTINGS.clockFormat12h);
    clockTextWidth = renderer.getTextWidth(SMALL_FONT_ID, clockStr);
  }
  const bool clockOnRight =
      SETTINGS.statusBarClockPosition == CrossPointSettings::STATUS_BAR_CLOCK_POSITION::STATUS_BAR_CLOCK_RIGHT;

  int progressTextWidth = 0;
  const int printedLabelWidth =
      printedPageLabel.empty() ? 0 : renderer.getTextWidth(SMALL_FONT_ID, printedPageLabel.c_str());
  const int printedLabelGap = printedLabelWidth > 0 && hasProgressText ? 8 : 0;

  if (hasProgressText) {
    // Right-aligned device page counter / progress percentage. The printed-page label, if any,
    // is drawn to the LEFT of this counter as a parenthesised hint — the device counter on the
    // right always reflects spine pagination.
    char progressStr[32];

    if (SETTINGS.statusBarBookProgressPercentage && SETTINGS.statusBarChapterPageCount) {
      snprintf(progressStr, sizeof(progressStr), "%d/%s%d  %.0f%%", currentPage, pageCountPrefix, pageCount,
               bookProgress);
    } else if (SETTINGS.statusBarBookProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%s%d", currentPage, pageCountPrefix, pageCount);
    }

    const int progressStrWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    progressTextWidth = progressStrWidth + printedLabelGap + printedLabelWidth;

    const int textX = screenWidth - metrics.statusBarHorizontalMargin - orientedMarginRight - progressStrWidth;
    renderer.drawText(SMALL_FONT_ID, textX, textY, progressStr);
    if (printedLabelWidth > 0) {
      renderer.drawText(SMALL_FONT_ID, textX - printedLabelGap - printedLabelWidth, textY, printedPageLabel.c_str());
    }
  } else if (printedLabelWidth > 0) {
    progressTextWidth = printedLabelWidth;
    const int textX = screenWidth - metrics.statusBarHorizontalMargin - orientedMarginRight - printedLabelWidth;
    renderer.drawText(SMALL_FONT_ID, textX, textY, printedPageLabel.c_str());
  }

  // Draw Battery
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;
  int batterySize = 0;
  if (SETTINGS.statusBarBattery) {
    GUI.drawBatteryLeft(renderer,
                        Rect{metrics.statusBarHorizontalMargin + orientedMarginLeft + 1, textY, metrics.batteryWidth,
                             metrics.batteryHeight},
                        showBatteryPercentage);
    // Measure the drawn footprint instead of estimating it: a three digit percentage ("100%")
    // is wider than a fixed guess, and the clock/title placed to the right would overlap it.
    // The leading 1 is the icon's own inset from the horizontal margin, above.
    batterySize = 1 + statusBarBatteryWidth(renderer, metrics, showBatteryPercentage);
  }

  // Right cluster, laid out from the right edge inwards: progress text (already drawn), then the
  // star, then the clock when it is right-positioned.
  const int rightEdge = screenWidth - metrics.statusBarHorizontalMargin - orientedMarginRight;
  const int starWidth = isStarred ? renderer.getTextWidth(SMALL_FONT_ID, "*") : 0;
  const int starReserve = isStarred ? starWidth + (progressTextWidth > 0 ? starGap : 0) : 0;
  int rightClusterWidth = progressTextWidth + starReserve;

  // Draw Clock at whichever end it was assigned. Left: just past the battery. Right: just past the
  // star / progress text, so it can never land on top of either.
  int leftClusterWidth = batterySize;
  if (clockTextWidth > 0) {
    int clockX;
    if (clockOnRight) {
      rightClusterWidth += (rightClusterWidth > 0 ? statusItemGap : 0) + clockTextWidth;
      clockX = rightEdge - rightClusterWidth;
    } else {
      clockX = metrics.statusBarHorizontalMargin + orientedMarginLeft + leftClusterWidth + statusItemGap;
      leftClusterWidth += statusItemGap + clockTextWidth;
    }
    renderer.drawText(SMALL_FONT_ID, clockX, textY, clockStr);
  }

  // Draw Title
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE && !title.empty()) {
    const int rendererableScreenWidth =
        screenWidth - (metrics.statusBarHorizontalMargin * 2) - orientedMarginLeft - orientedMarginRight;

    const int titleMarginLeft = leftClusterWidth + 30;
    const int titleMarginRight = rightClusterWidth + 30;

    // Attempt to center title on the screen, but if title is too wide then later we will center it within the
    // available space.
    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;

    int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    if (titleWidth > availableTitleSpace) {
      // Not enough space to center on the screen, center it within the remaining space instead
      availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
      titleMarginLeftAdjusted = titleMarginLeft;
    }
    if (titleWidth > availableTitleSpace) {
      title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }

    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + metrics.statusBarHorizontalMargin + orientedMarginLeft +
                          (availableTitleSpace - titleWidth) / 2,
                      textY, title.c_str());
  }

  // Draw star indicator between title and progress text
  if (isStarred) {
    renderer.drawText(SMALL_FONT_ID, rightEdge - progressTextWidth - starReserve, textY, "*");
  }
}

void BaseTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto truncatedLabel =
      renderer.truncatedText(SMALL_FONT_ID, label, rect.width - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y, truncatedLabel.c_str());
}

void BaseTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                              int contentStartX, int contentWidth) const {
  (void)textWidth;
  (void)contentStartX;
  (void)contentWidth;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int bracketHeight = lineHeight;
  const int fieldLeft = rect.x + 15;
  const int fieldRight = rect.x + rect.width - 15;
  const int topY = rect.y - 7;
  const int bottomY = rect.y + rect.height + lineHeight + 7;
  const int tickLen = bracketHeight / 2;
  const int thickness = cursorMode ? 3 : 1;

  renderer.fillRect(fieldLeft, topY, thickness, bottomY - topY + 1, true);
  renderer.drawLine(fieldLeft, topY, fieldLeft + tickLen, topY, thickness, true);
  renderer.drawLine(fieldLeft, bottomY, fieldLeft + tickLen, bottomY, thickness, true);

  renderer.fillRect(fieldRight - thickness + 1, topY, thickness, bottomY - topY + 1, true);
  renderer.drawLine(fieldRight, topY, fieldRight - tickLen, topY, thickness, true);
  renderer.drawLine(fieldRight, bottomY, fieldRight - tickLen, bottomY, thickness, true);
}

void BaseTheme::drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected,
                                const char* secondaryLabel, const KeyboardKeyType keyType,
                                const bool inactiveSelection) const {
  if (isSelected) {
    if (inactiveSelection) {
      renderer.drawRect(rect.x, rect.y, rect.width, rect.height, 2, true);
    } else if (keyType == KeyboardKeyType::Disabled) {
      renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
    } else {
      renderer.fillRect(rect.x, rect.y, rect.width, rect.height, true);
    }
  } else if (keyType == KeyboardKeyType::Shift || keyType == KeyboardKeyType::Mode || keyType == KeyboardKeyType::Del ||
             keyType == KeyboardKeyType::Space || keyType == KeyboardKeyType::Ok ||
             keyType == KeyboardKeyType::Disabled) {
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  }

  const bool invert = isSelected && !inactiveSelection && keyType != KeyboardKeyType::Disabled;

  if (keyType == KeyboardKeyType::Space) {
    const int lineHalfWidth = rect.width * 3 / 10;
    const int centerX = rect.x + rect.width / 2;
    const int lineY = rect.y + rect.height / 2 + 3;
    renderer.drawLine(centerX - lineHalfWidth, lineY, centerX + lineHalfWidth, lineY, 3, !invert);
    return;
  }

  if (keyType == KeyboardKeyType::Del) {
    const int centerX = rect.x + rect.width / 2;
    const int centerY = rect.y + rect.height / 2;
    const int arrowLen = rect.width / 4;
    const int arrowHead = arrowLen / 2;
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX + arrowLen / 2, centerY, 3, !invert);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX - arrowLen / 2 + arrowHead, centerY - arrowHead, 3,
                      !invert);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX - arrowLen / 2 + arrowHead, centerY + arrowHead, 3,
                      !invert);
    return;
  }

  const bool hasSecondary = secondaryLabel != nullptr && secondaryLabel[0] != '\0';
  const int primaryOffset = 0;
  const int itemWidth = renderer.getTextWidth(UI_12_FONT_ID, label);
  const int textX = rect.x + (rect.width - itemWidth) / 2;
  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2 + primaryOffset;

  if (hasSecondary) {
    const int secWidth = renderer.getTextWidth(SMALL_FONT_ID, secondaryLabel);
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - secWidth - 1, rect.y, secondaryLabel, !invert);
  }

  renderer.drawText(UI_12_FONT_ID, textX, textY, label, !invert);
}
