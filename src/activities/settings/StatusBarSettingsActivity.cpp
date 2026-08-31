#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
const StrId progressBarNames[] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};
const StrId progressBarThicknessNames[] = {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM,
                                           StrId::STR_PROGRESS_BAR_THICK};
const StrId titleNames[] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};
const StrId statusItemsPositionNames[] = {StrId::STR_TOP, StrId::STR_BOTTOM};
const StrId clockPositionNames[] = {StrId::STR_ALIGN_LEFT, StrId::STR_ALIGN_RIGHT};

// One menu row. Editing a status-bar option means: cycle `field` through `valueCount` values and
// display its current value. Rows with an enum-style set of choices provide `valueNames` (indexed by
// the field value); rows with no `valueNames` are on/off toggles rendered as Show/Hide.
//
// The whole menu is this single table. Adding, removing, or reordering a row is a one-line edit here —
// there is no parallel index bookkeeping to keep in sync. Rows with `requiresClock` are skipped when
// the clock feature is off, so the visible list compacts without any index remapping.
struct StatusBarItem {
  StrId label;
  uint8_t CrossPointSettings::* field;
  uint8_t valueCount;
  uint8_t defaultValue;     // value to reset to if the stored one is out of range
  const StrId* valueNames;  // nullptr → boolean Show/Hide toggle
  bool requiresClock;
};

template <size_t N>
constexpr StatusBarItem enumItem(StrId label, uint8_t CrossPointSettings::* field, const StrId (&names)[N],
                                 uint8_t defaultValue, bool requiresClock = false) {
  return {label, field, static_cast<uint8_t>(N), defaultValue, names, requiresClock};
}
constexpr StatusBarItem toggleItem(StrId label, uint8_t CrossPointSettings::* field, bool requiresClock = false) {
  return {label, field, 2, 1, nullptr, requiresClock};
}

const StatusBarItem statusBarItems[] = {
    enumItem(StrId::STR_STATUS_ITEMS_POSITION, &CrossPointSettings::statusBarItemsPosition, statusItemsPositionNames,
             CrossPointSettings::STATUS_BAR_ITEMS_POSITION::STATUS_BAR_ITEMS_BOTTOM),
    toggleItem(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount),
    toggleItem(StrId::STR_PRINTED_PAGE_NUMBER, &CrossPointSettings::statusBarPrintedPage),
    toggleItem(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage),
    enumItem(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle, titleNames,
             CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE),
    toggleItem(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery),
    toggleItem(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock, /*requiresClock=*/true),
    enumItem(StrId::STR_CLOCK_POSITION, &CrossPointSettings::statusBarClockPosition, clockPositionNames,
             CrossPointSettings::STATUS_BAR_CLOCK_POSITION::STATUS_BAR_CLOCK_LEFT, /*requiresClock=*/true),
    enumItem(StrId::STR_UPPER_PROGRESS_BAR, &CrossPointSettings::statusBarUpperProgressBar, progressBarNames,
             CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS),
    enumItem(StrId::STR_UPPER_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarUpperProgressBarThickness,
             progressBarThicknessNames, CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS::PROGRESS_BAR_NORMAL),
    enumItem(StrId::STR_LOWER_PROGRESS_BAR, &CrossPointSettings::statusBarLowerProgressBar, progressBarNames,
             CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS),
    enumItem(StrId::STR_LOWER_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarLowerProgressBarThickness,
             progressBarThicknessNames, CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS::PROGRESS_BAR_NORMAL),
};

// Map a visible row index (clock rows omitted when the clock is off) to its entry in statusBarItems.
const StatusBarItem& visibleItem(int visibleIndex) {
  int seen = 0;
  for (const auto& item : statusBarItems) {
    if (item.requiresClock && !SETTINGS.useClock) {
      continue;
    }
    if (seen == visibleIndex) {
      return item;
    }
    ++seen;
  }
  return statusBarItems[0];  // out-of-range guard; callers clamp the index first
}

int visibleItemCount() {
  return static_cast<int>(
      std::count_if(std::begin(statusBarItems), std::end(statusBarItems),
                    [](const StatusBarItem& item) { return !item.requiresClock || SETTINGS.useClock; }));
}

// Retained for the progress-bar preview drawing below, which references specific enum cardinalities.
constexpr int PROGRESS_BAR_ITEMS = 3;

constexpr int previewHorizontalInset = 10;
constexpr int previewHeight = 78;
constexpr int previewInnerMargin = 4;
constexpr int previewBatteryInset = 2;  // matches the battery's inset from the margin in the real bar
constexpr int statusItemGap = 8;        // gap between adjacent status items, as in BaseTheme::drawStatusBar

void drawPreviewProgressBar(const GfxRenderer& renderer, const Rect& rect, const uint8_t progressBar,
                            const uint8_t thickness, const bool topEdge) {
  if (progressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS) {
    return;
  }

  const int percent = progressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS ? 75 : 25;
  const int barHeight = UITheme::getProgressBarHeight(progressBar, thickness);
  const int y = topEdge ? rect.y + previewInnerMargin : rect.y + rect.height - previewInnerMargin - barHeight;
  const int barWidth = (rect.width - previewInnerMargin * 2) * percent / 100;
  renderer.fillRect(rect.x + previewInnerMargin, y, barWidth, barHeight);
}

void drawPreviewStatusItems(const GfxRenderer& renderer, const Rect& rect, const ThemeMetrics& metrics) {
  const bool hasProgressText = SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage;
  const bool hasTitle = SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE;
  const bool hasStatusItems = hasProgressText || hasTitle || SETTINGS.statusBarBattery ||
                              SETTINGS.statusBarPrintedPage || (SETTINGS.useClock && SETTINGS.statusBarClock);
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
  const int textY = statusItemsAtTop
                        ? rect.y + previewInnerMargin + adjacentProgressHeight + 4
                        : rect.y + rect.height - previewInnerMargin - adjacentProgressHeight - statusItemsHeight + 4;

  const bool showBatteryPercentage =
      SETTINGS.statusBarBattery &&
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;
  const bool showClock = SETTINGS.useClock && SETTINGS.statusBarClock;
  const int previewClockWidth = showClock ? renderer.getTextWidth(SMALL_FONT_ID, "00:00") : 0;
  const bool clockOnRight =
      SETTINGS.statusBarClockPosition == CrossPointSettings::STATUS_BAR_CLOCK_POSITION::STATUS_BAR_CLOCK_RIGHT;

  // Left cluster: battery, then the clock when it is left-positioned. Reserving the battery's
  // *measured* width (icon + percentage) is what keeps the clock off the percentage text —
  // estimating it is what made the preview overlap (issue #214).
  const int leftClusterX = rect.x + previewInnerMargin + previewBatteryInset;
  int leftClusterWidth = 0;
  if (SETTINGS.statusBarBattery) {
    GUI.drawBatteryLeft(renderer, Rect{leftClusterX, textY, metrics.batteryWidth, metrics.batteryHeight},
                        showBatteryPercentage);
    leftClusterWidth = BaseTheme::statusBarBatteryWidth(renderer, metrics, showBatteryPercentage);
  }

  // Right-aligned zone: the printed ("physical") page label sits to the LEFT of the device page
  // counter as a parenthesised hint, matching BaseTheme::drawStatusBar. Example label "(vii)".
  const char* printedLabel = SETTINGS.statusBarPrintedPage ? "(vii)" : "";
  const int printedLabelWidth = *printedLabel ? renderer.getTextWidth(SMALL_FONT_ID, printedLabel) : 0;
  const int printedLabelGap = printedLabelWidth > 0 && hasProgressText ? 8 : 0;

  int progressTextWidth = 0;
  const int rightEdge = rect.x + rect.width - previewInnerMargin - 2;
  if (hasProgressText) {
    char progressStr[32] = "";
    if (SETTINGS.statusBarChapterPageCount && SETTINGS.statusBarBookProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %d%%", 8, 32, 75);
    } else if (SETTINGS.statusBarBookProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d%%", 75);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", 8, 32);
    }

    const int progressStrWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    progressTextWidth = progressStrWidth + printedLabelGap + printedLabelWidth;
    renderer.drawText(SMALL_FONT_ID, rightEdge - progressStrWidth, textY, progressStr);
    if (printedLabelWidth > 0) {
      renderer.drawText(SMALL_FONT_ID, rightEdge - progressStrWidth - printedLabelGap - printedLabelWidth, textY,
                        printedLabel);
    }
  } else if (printedLabelWidth > 0) {
    progressTextWidth = printedLabelWidth;
    renderer.drawText(SMALL_FONT_ID, rightEdge - printedLabelWidth, textY, printedLabel);
  }

  // Clock goes at whichever end it is configured for, mirroring BaseTheme::drawStatusBar: just
  // past the battery on the left, or just past the progress text on the right.
  int rightClusterWidth = progressTextWidth;
  if (showClock) {
    int clockX;
    if (clockOnRight) {
      rightClusterWidth += (rightClusterWidth > 0 ? statusItemGap : 0) + previewClockWidth;
      clockX = rightEdge - rightClusterWidth;
    } else {
      clockX = leftClusterX + leftClusterWidth + statusItemGap;
      leftClusterWidth += statusItemGap + previewClockWidth;
    }
    renderer.drawText(SMALL_FONT_ID, clockX, textY, "00:00");
  }

  if (!hasTitle) {
    return;
  }

  const char* title = SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE
                          ? tr(STR_EXAMPLE_BOOK)
                          : tr(STR_EXAMPLE_CHAPTER);
  const int leftReserve = leftClusterWidth > 0 ? previewBatteryInset + leftClusterWidth + statusItemGap : 6;
  const int rightReserve = rightClusterWidth > 0 ? rightClusterWidth + 18 : 6;
  const int titleAreaWidth = rect.width - previewInnerMargin * 2 - leftReserve - rightReserve;
  if (titleAreaWidth <= 0) {
    return;
  }

  std::string previewTitle = renderer.truncatedText(SMALL_FONT_ID, title, titleAreaWidth);
  const int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, previewTitle.c_str());
  renderer.drawText(SMALL_FONT_ID, rect.x + previewInnerMargin + leftReserve + (titleAreaWidth - titleWidth) / 2, textY,
                    previewTitle.c_str());
}
}  // namespace

void StatusBarSettingsActivity::onEnter() {
  Activity::onEnter();

  if (selectedIndex >= visibleItemCount()) {
    selectedIndex = 0;
  }

  // Clamp status bar settings in case of corrupt/migrated data: every field must hold a valid value
  // index (0..valueCount-1). A stray value would index past its valueNames array when rendered.
  for (const auto& item : statusBarItems) {
    if (SETTINGS.*item.field >= item.valueCount) {
      SETTINGS.*item.field = item.defaultValue;
    }
  }

  requestUpdate();
}

void StatusBarSettingsActivity::onExit() { Activity::onExit(); }

void StatusBarSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }

  // Handle navigation
  const int menuCount = visibleItemCount();
  buttonNavigator.onNextList(selectedIndex, menuCount, [this] { requestUpdate(); });
  buttonNavigator.onPreviousList(selectedIndex, menuCount, [this] { requestUpdate(); });
}

void StatusBarSettingsActivity::handleSelection() {
  const StatusBarItem& item = visibleItem(selectedIndex);
  SETTINGS.*item.field = (SETTINGS.*item.field + 1) % item.valueCount;
  SETTINGS.saveToFile();
}

void StatusBarSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  const int pageWidth = (int)renderer.getScreenWidth();
  const int pageHeight = (int)renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_CUSTOMISE_STATUS_BAR));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int previewLabelHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int previewAreaHeight = previewLabelHeight + previewHeight + metrics.verticalSpacing * 2;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - previewAreaHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, visibleItemCount(), static_cast<int>(selectedIndex),
      [](int index) { return std::string(I18N.get(visibleItem(index).label)); }, nullptr, nullptr,
      [](int index) {
        const StatusBarItem& item = visibleItem(index);
        const uint8_t value = SETTINGS.*item.field;
        // Enum rows show their named value; toggle rows (no valueNames) show Show/Hide.
        if (item.valueNames) {
          return I18N.get(item.valueNames[value]);
        }
        return value ? tr(STR_SHOW) : tr(STR_HIDE);
      },
      true);

  // Draw button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const int previewLabelY = contentTop + contentHeight + metrics.verticalSpacing;
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, previewLabelY, tr(STR_PREVIEW));
  const Rect previewRect{previewHorizontalInset, previewLabelY + previewLabelHeight + metrics.verticalSpacing / 2,
                         pageWidth - previewHorizontalInset * 2, previewHeight};
  renderer.drawRect(previewRect.x, previewRect.y, previewRect.width, previewRect.height);
  drawPreviewProgressBar(renderer, previewRect, SETTINGS.statusBarUpperProgressBar,
                         SETTINGS.statusBarUpperProgressBarThickness, true);
  drawPreviewProgressBar(renderer, previewRect, SETTINGS.statusBarLowerProgressBar,
                         SETTINGS.statusBarLowerProgressBarThickness, false);
  drawPreviewStatusItems(renderer, previewRect, metrics);

  renderer.displayBuffer();
}
