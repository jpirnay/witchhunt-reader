#include "ScreenRepairActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ScreenRepairActivity::onEnter() {
  Activity::onEnter();
  state = WARNING;
  requestUpdate();
}

void ScreenRepairActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state = REPAIRING;
      }
      // Paint the "this will take a while" screen and WAIT for it, so the reader sees why the
      // panel is about to flash for twenty seconds. The cycle below never yields to a render.
      requestUpdateAndWait();
      runRepairCycle();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == DONE && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
  }
}

void ScreenRepairActivity::runRepairCycle() {
  LOG_INF("REPAIR", "Screen repair: %d alternations + %d trailing white", kAlternations, kTrailingWhite);

  // Held for the whole cycle: this drives the panel directly, and a render landing in the middle
  // would put a page between two conditioning pushes and undo the point of them.
  RenderLock lock(*this);

  // HALF rather than FAST throughout. FAST is the differential bank, which drives only what
  // changed -- and after the first black push nothing does. HALF is the clean bank, the one
  // waveform on this board that drives every pixel every time, which is the whole exercise.
  const auto drive = [this](const uint8_t colour) {
    renderer.clearScreen(colour);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  };

  constexpr uint8_t kBlack = 0x00;
  constexpr uint8_t kWhite = 0xFF;

  for (int i = 0; i < kAlternations; ++i) {
    drive(kBlack);
    drive(kWhite);
  }
  // Finish on white, and dwell there: white is the panel's rest state, so the last thing the ink
  // is asked to do should be to settle rather than to swing.
  for (int i = 0; i < kTrailingWhite; ++i) {
    drive(kWhite);
  }

  LOG_INF("REPAIR", "Screen repair complete");
  state = DONE;
  requestUpdate();
}

void ScreenRepairActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_SCREEN_REPAIR));

  const int midY = contentRect.y + contentRect.height / 2;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int textWidth = contentRect.width - 2 * metrics.contentSidePadding;

  if (state == WARNING) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_SCREEN_REPAIR_BODY), textWidth, 4);
    int y = midY - lineHeight * static_cast<int>(lines.size()) / 2;
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
      y += lineHeight;
    }
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_START), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == REPAIRING) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_SCREEN_REPAIR_RUNNING));
    renderer.displayBuffer();
    return;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_SCREEN_REPAIR_DONE), true, EpdFontFamily::BOLD);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // FULL, so the screen the reader is left looking at is itself a clean one rather than a
  // differential against the white the cycle ended on.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
