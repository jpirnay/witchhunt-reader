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

// Whether a sideways swipe currently turns pages. Only Swipe reading mode uses horizontal
// swipes for that; in the tap modes the centre third does nothing at all, and saying so is more
// useful than implying a page turn that will not happen.
bool swipeTurnsPagesNow() { return SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_SWIPE; }

// Up to three lines of text centred in a box, each clipped to it. Every region on both pages is
// labelled this way, so the clipping, the centring and the vertical block-centring are stated
// once.
//
// Three rather than two because an outer third carries three actions: a vertical swipe in its
// edge column, and a sideways swipe across the whole third. The FIRST line is bold, which is
// what each page's key line refers to.
void drawBoxedLines(const GfxRenderer& renderer, const int fontId, const Rect box, const char* const* lines,
                    const int count) {
  const int lineH = renderer.getLineHeight(fontId);
  const int inner = box.width - 6;
  if (inner <= 0 || count <= 0) return;

  int drawn = 0;
  for (int i = 0; i < count; ++i) {
    if (lines[i] != nullptr && lines[i][0] != '\0') drawn++;
  }
  if (drawn == 0) return;

  int y = box.y + (box.height - drawn * lineH) / 2;
  for (int i = 0; i < count; ++i) {
    const char* text = lines[i];
    if (text == nullptr || text[0] == '\0') continue;
    const auto style = i == 0 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string clipped = renderer.truncatedText(fontId, text, inner, style);
    const int w = renderer.getTextWidth(fontId, clipped.c_str(), style);
    renderer.drawText(fontId, box.x + (box.width - w) / 2, y, clipped.c_str(), true, style);
    y += lineH;
  }
}

// The two-line case, which the tap page uses for every cell.
void drawBoxedPair(const GfxRenderer& renderer, const int fontId, const Rect box, const char* top, const char* bottom) {
  const char* lines[2] = {top, bottom};
  drawBoxedLines(renderer, fontId, box, lines, 2);
}

}  // namespace

int GestureActionsOverviewActivity::pageCount() const {
  return mappedInput.hasTouch() && gpio.supportsMultiTouch() ? 3 : 2;
}

void GestureActionsOverviewActivity::onEnter() {
  Activity::onEnter();
  // Deliberately does NOT force landscape, unlike ButtonActionsOverviewActivity. That page is a
  // four-column TABLE and needs the width; this one is a MAP of the screen, and a map drawn in
  // an orientation the reader is not holding tells them where the zones are on a screen shape
  // they cannot see. Keeping the native orientation makes the frame the same shape as the panel
  // the zones are actually on, which is the whole point of drawing it.
  requestUpdate();
}

void GestureActionsOverviewActivity::onExit() { Activity::onExit(); }

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
  // "2 / 3" in the subtitle slot rather than the version string: a reader needs to know there
  // ARE more pages before they will look for a way to reach them, and the header is the one
  // piece of chrome on this screen that can say so.
  const std::string pageOfPages = std::to_string(page + 1) + " / " + std::to_string(pageCount());
  GUI.drawHeader(renderer,
                 Rect{contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight},
                 I18N.get(title), pageOfPages.c_str());

  int top = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Each region carries TWO actions stacked, and without a key there is nothing to say which
  // line is which -- the page showed the information and left the reader to guess. Drawn above
  // the frame rather than inside it, where every pixel belongs to a zone.
  if (page < 2) {
    const StrId keyId = page == 0 ? StrId::STR_GEST_OVERVIEW_TAP_KEY : StrId::STR_GEST_OVERVIEW_SWIPE_KEY;
    const char* key = I18N.get(keyId);
    const int keyW = renderer.getTextWidth(fontId, key);
    renderer.drawText(fontId, contentRect.x + (contentRect.width - keyW) / 2, top, key);
    top += renderer.getLineHeight(fontId) + metrics.verticalSpacing / 2;
  }
  const int available = contentRect.y + contentRect.height - top;
  // A SCALE MODEL of the panel: the frame carries the screen's own aspect ratio, so a zone
  // drawn in the top-left of the frame is the top-left of the real screen and the thirds and
  // corners are in the proportions a finger will meet. Fitted to whichever dimension runs out
  // first. This is why the page no longer forces landscape -- a map has to be the shape of the
  // thing it maps.
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int maxW = contentRect.width - metrics.verticalSpacing * 2;
  int frameH = available - metrics.verticalSpacing;
  int frameW = (screenH > 0) ? frameH * screenW / screenH : maxW;
  if (frameW > maxW) {
    frameW = maxW;
    frameH = (screenW > 0) ? frameW * screenH / screenW : frameH;
  }
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

      // Each outer THIRD carries three actions, not two: a vertical swipe in its edge column
      // adjusts the light, and a sideways swipe anywhere in the third jumps ten pages. The
      // sideways action used to be labelled in the CENTRE of the frame, which is the one place
      // it does not apply -- the centre third is the plain page turn. Labelled in the zone it
      // belongs to now, stacked up / down / sideways, with the key line naming the axes.
      //
      // The third and the column are drawn as separate divisions because they really are
      // different widths (33% vs 25%): the vertical action needs the column, the sideways one
      // takes the whole third.
      const int third = frame.width / 3;
      renderer.drawLine(frame.x + third, frame.y + band, frame.x + third, frame.y + frame.height - band, true);
      renderer.drawLine(frame.x + frame.width - third, frame.y + band, frame.x + frame.width - third,
                        frame.y + frame.height - band, true);

      const struct {
        Rect box;
        Gesture up;
        Gesture down;
        Gesture sideways;
      } zones[] = {
          {Rect(frame.x, frame.y + band, third, frame.height - band * 2), Gesture::SwipeUpLeft, Gesture::SwipeDownLeft,
           Gesture::SwipeInLeftZone},
          {Rect(frame.x + frame.width - third, frame.y + band, third, frame.height - band * 2), Gesture::SwipeUpRight,
           Gesture::SwipeDownRight, Gesture::SwipeInRightZone},
      };
      for (const auto& zone : zones) {
        const std::string up = actionText(settings, zone.up);
        const std::string down = actionText(settings, zone.down);
        const std::string sideways = actionText(settings, zone.sideways);
        const char* lines[3] = {up.c_str(), down.c_str(), sideways.c_str()};
        drawBoxedLines(renderer, fontId, zone.box, lines, 3);
      }

      // The centre third: a sideways swipe here is the ordinary page turn, and a vertical one
      // is deliberately nobody's. Named rather than looked up, since neither is a bindable row.
      const Rect middle{frame.x + third, frame.y + band, frame.width - third * 2, frame.height - band * 2};
      const char* centreLines[1] = {
          I18N.get(swipeTurnsPagesNow() ? StrId::STR_BTN_ACT_PAGE_FORWARD : StrId::STR_BTN_DEF_NOTHING)};
      drawBoxedLines(renderer, fontId, middle, centreLines, 1);

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

  // Prev/Next are LABELLED, not left empty, and that is what makes the pages reachable at all.
  // An empty hint label marks its box inactive, so it is neither drawn nor tappable -- which on
  // a board navigated by finger left the later pages with no route to them and nothing on screen
  // saying they existed. Labelling them both advertises the paging and makes the boxes touch
  // targets, because ActivityManager::dispatchHintStripTap() injects the raw button a box
  // depicts. Reported on a T5S3, whose only physical keys are Down and the capacitive home key.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_PREV), tr(STR_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#endif  // CP_TOUCH_UI
