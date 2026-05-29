#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "fontIds.h"
#include "images/Logo120.h"

namespace {
void drawPlusOutline(const GfxRenderer& renderer, int x, int y, int size, int armThickness) {
  const int armOffset = (size - armThickness) / 2;
  renderer.drawRect(x, y + armOffset, size, armThickness, true);
  renderer.drawRect(x + armOffset, y, armThickness, size, true);
  renderer.drawLine(x + armOffset + 1, y + armOffset, x + armOffset + armThickness - 2, y + armOffset, false);
  renderer.drawLine(x + armOffset + 1, y + armOffset + armThickness - 1, x + armOffset + armThickness - 2,
                    y + armOffset + armThickness - 1, false);
  renderer.drawLine(x + armOffset, y + armOffset + 1, x + armOffset, y + armOffset + armThickness - 2, false);
  renderer.drawLine(x + armOffset + armThickness - 1, y + armOffset + 1, x + armOffset + armThickness - 1,
                    y + armOffset + armThickness - 2, false);
}
}  // namespace

void BootActivity::onEnter() {
  Activity::onEnter();
  RenderLock lock(*this);

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  constexpr int LOGO_SIZE = 120;
  constexpr int PLUS_SIZE = LOGO_SIZE / 3;
  constexpr int PLUS_ARM_THICKNESS = PLUS_SIZE / 3;

  const int logoX = (pageWidth - LOGO_SIZE) / 2;
  const int logoY = (pageHeight - LOGO_SIZE) / 2;

  renderer.clearScreen();
  renderer.drawImage(Logo120, logoX, logoY, LOGO_SIZE, LOGO_SIZE);

  const int plusY = logoY - PLUS_SIZE / 2;
  const int plus1X = logoX + LOGO_SIZE;
  const int plus2X = plus1X + PLUS_SIZE + 2;
  drawPlusOutline(renderer, plus1X, plusY, PLUS_SIZE, PLUS_ARM_THICKNESS);
  drawPlusOutline(renderer, plus2X, plusY, PLUS_SIZE, PLUS_ARM_THICKNESS);

  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
