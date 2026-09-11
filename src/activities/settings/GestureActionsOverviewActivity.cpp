#include "GestureActionsOverviewActivity.h"

#if CP_TOUCH_UI

#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "SettingInfo.h"
#include "SettingsList.h"
#include "TouchGestures.h"
#include "components/TapZones.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

using TouchGestures::Gesture;

// What the settings screen currently shows for one gesture. Looked up by the gesture's own
// label StrId, so BINDINGS stays the single source of truth for which row is which -- the
// same reason ButtonActionsOverviewActivity looks its cells up by (submenu, press kind)
// instead of keeping a parallel table.
//
// Empty when the row is absent, which is how a board without a warm light or without
// multi-touch drops those actions for free: getSettingsList() has already filtered them.
std::string actionText(const std::vector<SettingInfo>& settings, const Gesture gesture) {
  const StrId label = TouchGestures::BINDINGS[static_cast<size_t>(gesture)].label;
  const auto it = std::find_if(settings.begin(), settings.end(), [label](const SettingInfo& s) {
    return s.nameId == label && s.submenu == StrId::STR_MENU_GESTURE_ACTIONS;
  });
  if (it == settings.end()) return {};
  return it->getDisplayValue();
}

// Two lines of text centred in a box, each clipped to it. Both pages label regions this way
// -- a tap action above a hold action, or a direction above its action -- so the clipping and
// the centring are stated once.
void drawBoxedPair(const GfxRenderer& renderer, const int fontId, const Rect box, const char* top, const char* bottom) {
  const int lineH = renderer.getLineHeight(fontId);
  const int inner = box.width - 6;
  if (inner <= 0) return;

  const int pairHeight = (bottom != nullptr && bottom[0] != '\0') ? lineH * 2 : lineH;
  int y = box.y + (box.height - pairHeight) / 2;

  const auto centred = [&](const char* text, const EpdFontFamily::Style style) {
    if (text == nullptr || text[0] == '\0') return;
    const std::string clipped = renderer.truncatedText(fontId, text, inner, style);
    const int w = renderer.getTextWidth(fontId, clipped.c_str(), style);
    renderer.drawText(fontId, box.x + (box.width - w) / 2, y, clipped.c_str(), true, style);
    y += lineH;
  };
  centred(top, EpdFontFamily::BOLD);
  centred(bottom, EpdFontFamily::REGULAR);
}

}  // namespace

int GestureActionsOverviewActivity::pageCount() const {
  return mappedInput.hasTouch() && gpio.supportsMultiTouch() ? 3 : 2;
}

void GestureActionsOverviewActivity::onEnter() {
  Activity::onEnter();
  // Landscape for the same reason the button overview forces it: the regions have to be wide
  // enough for a real action label, and a portrait third is not.
  {
    RenderLock lock(*this);
    renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
  }
  requestUpdate();
}

void GestureActionsOverviewActivity::onExit() {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  Activity::onExit();
}

void GestureActionsOverviewActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  // Confirm pages forward and wraps, so the whole overview is reachable on a board whose only
  // nav keys are Down and the capacitive home key -- which is both of the touch boards.
  const bool next = mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
                    mappedInput.wasPressed(MappedInputManager::Button::Right) ||
                    mappedInput.wasPressed(MappedInputManager::Button::Down);
  const bool prev = mappedInput.wasPressed(MappedInputManager::Button::Left) ||
                    mappedInput.wasPressed(MappedInputManager::Button::Up);
  if (next) {
    page = (page + 1) % pageCount();
    requestUpdate();
    return;
  }
  if (prev) {
    page = (page + pageCount() - 1) % pageCount();
    requestUpdate();
  }
}

void GestureActionsOverviewActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/false);
  const int fontId = UI_10_FONT_ID;

  const StrId title = page == 0   ? StrId::STR_GEST_OVERVIEW_TAPS
                      : page == 1 ? StrId::STR_GEST_OVERVIEW_SWIPES
                                  : StrId::STR_GEST_MULTI_GROUP;
  GUI.drawHeader(renderer,
                 Rect{contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight},
                 I18N.get(title), CROSSPOINT_VERSION);

  const int top = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int available = contentRect.y + contentRect.height - top;
  // The frame is drawn to the panel's own proportions rather than a fixed size, so the same
  // arithmetic reads correctly on the X4 Pro's 800x480 and the T5S3's 960x540.
  const int frameH = available - metrics.verticalSpacing;
  const int frameW = std::min(contentRect.width - metrics.verticalSpacing * 2, frameH * 3 / 2);
  const Rect frame{contentRect.x + (contentRect.width - frameW) / 2, top, frameW, frameH};

  const auto settings = getSettingsList();

  if (page == 2) {
    // No zones to draw: pinch and rotate happen wherever the fingers are. A plain two-column
    // list is the honest picture, so the frame is not drawn at all on this page.
    const int lineH = renderer.getLineHeight(fontId);
    const Gesture rows[] = {Gesture::PinchIn, Gesture::PinchOut, Gesture::RotateClockwise,
                            Gesture::RotateCounterClockwise};
    int y = frame.y + metrics.verticalSpacing;
    const int labelW = frame.width * 45 / 100;
    for (const Gesture g : rows) {
      const StrId label = TouchGestures::BINDINGS[static_cast<size_t>(g)].label;
      renderer.drawText(fontId, frame.x, y, renderer.truncatedText(fontId, I18N.get(label), labelW).c_str(), true,
                        EpdFontFamily::BOLD);
      const std::string action = actionText(settings, g);
      renderer.drawText(fontId, frame.x + labelW, y,
                        renderer.truncatedText(fontId, action.c_str(), frame.width - labelW).c_str());
      y += lineH + 6;
    }
  } else {
    renderer.drawRect(frame.x, frame.y, frame.width, frame.height, true);

    const int colW = frame.width / 3;
    const int rowH = frame.height / 3;
    // The grid the reader actually meets: TapZones splits the horizontal thirds first, so the
    // outer columns run the full height and only the centre one is divided.
    renderer.drawLine(frame.x + colW, frame.y, frame.x + colW, frame.y + frame.height, true);
    renderer.drawLine(frame.x + colW * 2, frame.y, frame.x + colW * 2, frame.y + frame.height, true);
    renderer.drawLine(frame.x + colW, frame.y + rowH, frame.x + colW * 2, frame.y + rowH, true);
    renderer.drawLine(frame.x + colW, frame.y + rowH * 2, frame.x + colW * 2, frame.y + rowH * 2, true);

    if (page == 0) {
      // Corner squares, drawn to the same 1/8-of-the-shorter-edge rule cornerFor() uses, so
      // the picture is the geometry rather than a sketch of it.
      const int side = std::min(frame.width, frame.height) / TapZones::kCornerDivisor;
      renderer.drawRect(frame.x, frame.y, side, side, true);
      renderer.drawRect(frame.x + frame.width - side, frame.y, side, side, true);
      renderer.drawRect(frame.x, frame.y + frame.height - side, side, side, true);
      renderer.drawRect(frame.x + frame.width - side, frame.y + frame.height - side, side, side, true);

      // Tap above, hold below, in the region each belongs to.
      const struct {
        Rect box;
        Gesture tap;
        Gesture hold;
      } cells[] = {
          {Rect(frame.x, frame.y + rowH, colW, rowH), Gesture::TapLeft, Gesture::LongTapLeft},
          {Rect(frame.x + colW * 2, frame.y + rowH, colW, rowH), Gesture::TapRight, Gesture::LongTapRight},
          {Rect(frame.x + colW, frame.y, colW, rowH), Gesture::TapTop, Gesture::LongTapTop},
          {Rect(frame.x + colW, frame.y + rowH, colW, rowH), Gesture::TapCentre, Gesture::LongTapCentre},
          {Rect(frame.x + colW, frame.y + rowH * 2, colW, rowH), Gesture::TapBottom, Gesture::LongTapBottom},
      };
      for (const auto& cell : cells) {
        const std::string tapAction = actionText(settings, cell.tap);
        const std::string holdAction = actionText(settings, cell.hold);
        drawBoxedPair(renderer, fontId, cell.box, tapAction.c_str(), holdAction.c_str());
      }

      // The corners carry a hold only, and their squares are far too small for a label, so
      // each one is named just outside itself.
      const struct {
        int x;
        int y;
        Gesture hold;
      } corners[] = {
          {frame.x + side + 2, frame.y + 2, Gesture::LongTapTopLeft},
          {frame.x + frame.width - side - 2, frame.y + 2, Gesture::LongTapTopRight},
          {frame.x + side + 2, frame.y + frame.height - side, Gesture::LongTapBottomLeft},
          {frame.x + frame.width - side - 2, frame.y + frame.height - side, Gesture::LongTapBottomRight},
      };
      for (const auto& corner : corners) {
        const std::string action = actionText(settings, corner.hold);
        if (action.empty()) continue;
        const std::string clipped = renderer.truncatedText(fontId, action.c_str(), colW - side);
        const int w = renderer.getTextWidth(fontId, clipped.c_str());
        const bool rightSide = corner.x > frame.x + frame.width / 2;
        renderer.drawText(fontId, rightSide ? corner.x - w : corner.x, corner.y, clipped.c_str());
      }
    } else {
      // Swipes. The edge COLUMNS and the top/bottom BANDS are what the reader has to be able
      // to see, because those are the anchors the gestures are resolved against and nothing
      // else on the page reveals them.
      const int column = frame.width * TapZones::kEdgeColumnPercent / 100;
      const int band = frame.height * TapZones::kEdgeBandPercent / 100;
      renderer.drawLine(frame.x + column, frame.y, frame.x + column, frame.y + frame.height, true);
      renderer.drawLine(frame.x + frame.width - column, frame.y, frame.x + frame.width - column, frame.y + frame.height,
                        true);
      renderer.drawLine(frame.x, frame.y + band, frame.x + frame.width, frame.y + band, true);
      renderer.drawLine(frame.x, frame.y + frame.height - band, frame.x + frame.width, frame.y + frame.height - band,
                        true);

      const struct {
        Rect box;
        Gesture up;
        Gesture down;
      } columns[] = {
          {Rect(frame.x, frame.y + band, column, frame.height - band * 2), Gesture::SwipeUpLeft,
           Gesture::SwipeDownLeft},
          {Rect(frame.x + frame.width - column, frame.y + band, column, frame.height - band * 2), Gesture::SwipeUpRight,
           Gesture::SwipeDownRight},
      };
      for (const auto& col : columns) {
        const std::string upAction = actionText(settings, col.up);
        const std::string downAction = actionText(settings, col.down);
        drawBoxedPair(renderer, fontId, col.box, upAction.c_str(), downAction.c_str());
      }

      // The outward page-turn swipes live in the outer thirds, which overlap the edge columns
      // above -- so they are labelled in the middle band of the frame, clear of both.
      const Rect middle{frame.x + column, frame.y + band, frame.width - column * 2, frame.height - band * 2};
      drawBoxedPair(renderer, fontId, middle, actionText(settings, Gesture::SwipeLeftInLeft).c_str(),
                    actionText(settings, Gesture::SwipeRightInRight).c_str());

      // The bands are the built-in edge gestures, not bindable rows, so they are NAMED rather
      // than looked up -- and named per board, because which edge carries which swaps with
      // whether there is a light to reach (see MappedInputManager::wasMenuGesture).
      const bool lit = Frontlight.present();
      drawBoxedPair(renderer, fontId, Rect{frame.x, frame.y, frame.width, band},
                    I18N.get(lit ? StrId::STR_READING_LIGHT : StrId::STR_BTN_DEF_READER_MENU), nullptr);
      drawBoxedPair(renderer, fontId, Rect{frame.x, frame.y + frame.height - band, frame.width, band},
                    I18N.get(lit ? StrId::STR_BTN_DEF_READER_MENU : StrId::STR_BTN_DEF_GO_HOME), nullptr);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#endif  // CP_TOUCH_UI
