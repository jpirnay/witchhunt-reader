#include "QrDisplayActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

void QrDisplayActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void QrDisplayActivity::onExit() { Activity::onExit(); }

void QrDisplayActivity::loop() {
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if ((ev.button == MappedInputManager::Button::Back || ev.button == MappedInputManager::Button::Confirm) &&
        ev.type == ButtonEventManager::PressType::Short) {
      finish();
      return;
    }
  }
}

void QrDisplayActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Opened over the reader, so in the book's orientation: header and code are measured from the
  // content area's top, which the hint strip holds in PortraitInverted.
  const Rect body = UITheme::getBodyRect(renderer, true, false);

  GUI.drawHeader(renderer, UITheme::getHeaderRect(renderer), tr(STR_DISPLAY_QR), nullptr);

  const int startY = body.y;
  constexpr int truncNoticeHeight = 16;
  const int availableHeight = body.height - metrics.verticalSpacing - truncNoticeHeight;

  const Rect qrBounds(body.x + metrics.contentSidePadding, startY, body.width - metrics.contentSidePadding * 2,
                      availableHeight);
  const bool truncated = QrUtils::drawQrCode(renderer, qrBounds, textPayload);

  if (truncated) {
    renderer.drawCenteredText(SMALL_FONT_ID, startY + availableHeight + 2, "...", true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
