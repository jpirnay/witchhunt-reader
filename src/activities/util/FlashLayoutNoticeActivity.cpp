#include "FlashLayoutNoticeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "FlashLayoutNotice.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/themes/PanelMetrics.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
// The flasher's code: as large as the web server's, and not drawn below the size a phone still reads from a page.
constexpr int kMaxCode = 198;
constexpr int kMinCode = 80;
}  // namespace

void FlashLayoutNoticeActivity::onEnter() {
  Activity::onEnter();
  whatIsWrong_ = FlashLayoutNotice::whatIsWrong();
  requestUpdate();
}

void FlashLayoutNoticeActivity::loop() {
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.type != ButtonEventManager::PressType::Short) continue;
    if (ev.button == MappedInputManager::Button::Back || ev.button == MappedInputManager::Button::Confirm) {
      FlashLayoutNotice::markShown();
      activityManager.goHome();
      return;
    }
  }
}

void FlashLayoutNoticeActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, UITheme::getHeaderRect(renderer), tr(STR_FLASH_EMERGENCY_TITLE), nullptr);

  const Rect body = UITheme::getBodyRect(renderer, true, false);
  const int font = PanelMetrics::COMPACT ? SMALL_FONT_ID : UI_10_FONT_ID;
  const int lineHeight = renderer.getLineHeight(font);
  const int urlHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int gap = metrics.verticalSpacing;
  const int left = body.x + metrics.contentSidePadding;
  const int width = std::max(0, body.width - metrics.contentSidePadding * 2);
  const int bottom = body.y + body.height;

  // The code and the address: beside the text on a screen wider than tall, under it otherwise, where the text leaves
  // room for at least the smallest code.
  const bool beside = body.width > body.height;
  int code = beside ? std::min({kMaxCode, width * 2 / 5, body.height - urlHeight - gap}) : 0;
  const int textWidth = beside ? width - code - gap : width;
  const int reserve = beside ? 0 : kMinCode + urlHeight + gap * 2;

  // The first paragraph says it is an emergency mode and why, in bold; the second what fixes it and what it costs.
  const struct {
    const char* text;
    EpdFontFamily::Style style;
  } paragraphs[] = {{whatIsWrong_.c_str(), EpdFontFamily::BOLD}, {tr(STR_FLASH_EMERGENCY_FIX), EpdFontFamily::REGULAR}};
  int y = body.y;
  for (const auto& paragraph : paragraphs) {
    const int linesLeft = (bottom - reserve - y) / lineHeight;
    if (linesLeft <= 0) break;
    for (const auto& line : renderer.wrappedText(font, paragraph.text, textWidth, linesLeft, paragraph.style)) {
      renderer.drawText(font, left, y, line.c_str(), true, paragraph.style);
      y += lineHeight;
    }
    y += gap;
  }

  int codeX = left + textWidth + gap;
  int codeY = body.y;
  if (!beside) {
    code = std::min({kMaxCode, width, bottom - y - urlHeight - gap});
    codeX = left + (width - code) / 2;
    codeY = y;
  }
  const int column = beside ? code : width;
  const int columnX = beside ? codeX : left;
  int urlY = y;
  if (code >= kMinCode) {
    QrUtils::drawQrCode(renderer, Rect{codeX, codeY, code, code}, FlashLayoutNotice::kFlasherUrl);
    urlY = codeY + code + gap;
  }
  if (urlY + urlHeight <= bottom) {
    const std::string url = renderer.truncatedText(SMALL_FONT_ID, FlashLayoutNotice::kFlasherAddress, column);
    const int urlWidth = renderer.getTextWidth(SMALL_FONT_ID, url.c_str());
    renderer.drawText(SMALL_FONT_ID, columnX + (column - urlWidth) / 2, urlY, url.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
