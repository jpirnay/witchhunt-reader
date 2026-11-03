#include "ButtonActionsOverviewActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <array>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "SettingInfo.h"
#include "SettingsList.h"
#include "components/UITheme.h"
#include "components/themes/PanelMetrics.h"
#include "fontIds.h"

namespace {

struct ButtonRow {
  StrId submenu;     // e.g. STR_BTN_BACK — identifies which logical button
  StrId labelStrId;  // e.g. STR_BTN_BACK — display label for the row
};

// Order matches the on-screen rendering order — power → confirm.
constexpr std::array<ButtonRow, 7> kButtonRows = {{
    {StrId::STR_BTN_POWER, StrId::STR_BTN_POWER},
    {StrId::STR_BTN_BACK, StrId::STR_BTN_BACK},
    {StrId::STR_BTN_LEFT, StrId::STR_BTN_LEFT},
    {StrId::STR_BTN_RIGHT, StrId::STR_BTN_RIGHT},
    {StrId::STR_BTN_UP, StrId::STR_BTN_UP},
    {StrId::STR_BTN_DOWN, StrId::STR_BTN_DOWN},
    {StrId::STR_BTN_CONFIRM, StrId::STR_BTN_CONFIRM},
}};

// The table's columns, left to right.
constexpr std::array<StrId, 3> kPressKinds = {StrId::STR_BTN_SHORT_PRESS, StrId::STR_BTN_DOUBLE_PRESS,
                                              StrId::STR_BTN_LONG_PRESS};

// On the heap: 21 strings are ~500 bytes, too much for a render's stack frame.
using CellValues = std::vector<std::array<std::string, kPressKinds.size()>>;

// Every (button, press kind) cell's display value, read in ONE walk of the settings: a cell is the
// first Controls row with that button's submenu and that press kind. Looked up cell by cell, the
// table needs the whole list at hand, which is one 11.5 KB block (see forEachSetting()).
CellValues cellValues() {
  CellValues values(kButtonRows.size());
  std::array<std::array<bool, kPressKinds.size()>, kButtonRows.size()> found{};
  forEachSetting([&](const SettingInfo& s) {
    if (s.category != StrId::STR_CAT_CONTROLS) return;
    for (size_t r = 0; r < kButtonRows.size(); r++) {
      if (kButtonRows[r].submenu != s.submenu) continue;
      for (size_t k = 0; k < kPressKinds.size(); k++) {
        if (s.nameId != kPressKinds[k] || found[r][k]) continue;
        values[r][k] = s.getDisplayValue();
        found[r][k] = true;
      }
    }
  });
  return values;
}

}  // namespace

void ButtonActionsOverviewActivity::onEnter() {
  Activity::onEnter();

  // Force landscape so the four-column table has room for full action labels.
  {
    RenderLock lock(*this);
    renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
  }

  requestUpdate();
}

void ButtonActionsOverviewActivity::onExit() {
  // Restore portrait — matches the rest of the settings UI.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  Activity::onExit();
}

void ButtonActionsOverviewActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    finish();
    return;
  }
}

void ButtonActionsOverviewActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/false);

  GUI.drawHeader(renderer,
                 Rect{contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_BTN_ACTIONS_OVERVIEW), CROSSPOINT_VERSION);

  const int fontId = UI_10_FONT_ID;
  const int lineH = renderer.getLineHeight(fontId);
  const int rowStep = lineH + PanelMetrics::capped(4, 1);  // seven rows have to fit a 240 px landscape panel
  const int padX = metrics.verticalSpacing * 2;

  // 4-column layout: button label, short, double, long.
  // Button column gets a fixed share, the three action columns share the remainder evenly.
  const int innerLeft = contentRect.x + padX;
  const int innerWidth = contentRect.width - padX * 2;
  const int colButtonWidth = innerWidth * 28 / 100;
  const int colActionWidth = (innerWidth - colButtonWidth) / 3;

  const int colX[4] = {
      innerLeft,
      innerLeft + colButtonWidth,
      innerLeft + colButtonWidth + colActionWidth,
      innerLeft + colButtonWidth + colActionWidth * 2,
  };
  const int colW[4] = {
      colButtonWidth - 2,
      colActionWidth - 2,
      colActionWidth - 2,
      innerLeft + innerWidth - colX[3] - 2,
  };

  int y = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Header row (bold)
  const char* headers[4] = {
      tr(STR_BTN_OVERVIEW_HEADER_BUTTON),
      tr(STR_BTN_OVERVIEW_HEADER_SHORT),
      tr(STR_BTN_OVERVIEW_HEADER_DOUBLE),
      tr(STR_BTN_OVERVIEW_HEADER_LONG),
  };
  for (int c = 0; c < 4; c++) {
    const std::string clipped = renderer.truncatedText(fontId, headers[c], colW[c], EpdFontFamily::BOLD);
    renderer.drawText(fontId, colX[c], y, clipped.c_str(), true, EpdFontFamily::BOLD);
  }
  y += lineH + 2;

  // Underline below header
  const int underlineX[4] = {innerLeft, innerLeft + innerWidth, innerLeft + innerWidth, innerLeft};
  const int underlineY[4] = {y, y, y + 1, y + 1};
  renderer.fillPolygon(underlineX, underlineY, 4, true);
  y += 4;

  const CellValues cells = cellValues();

  // Data rows
  for (size_t r = 0; r < kButtonRows.size(); r++) {
    const ButtonRow& row = kButtonRows[r];
    const std::string label = I18N.get(row.labelStrId);
    const std::string clippedLabel = renderer.truncatedText(fontId, label.c_str(), colW[0], EpdFontFamily::BOLD);
    renderer.drawText(fontId, colX[0], y, clippedLabel.c_str(), true, EpdFontFamily::BOLD);

    for (size_t k = 0; k < kPressKinds.size(); k++) {
      renderer.drawText(fontId, colX[k + 1], y,
                        renderer.truncatedText(fontId, cells[r][k].c_str(), colW[k + 1]).c_str());
    }

    y += rowStep;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
