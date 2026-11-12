#include "FrontlightPanelActivity.h"

#if CP_TOUCH_UI

#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "DarkModeNow.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/themes/PanelMetrics.h"

namespace fui = freeink::ui;

namespace {
using namespace LightDrawerLayout;

constexpr int BUTTON_BRIGHTNESS_STEP = 5;
constexpr int FINE_STEP = 1;

// Upstream's, unchanged, and deliberately clamping to the full 0..100: this is shared by BOTH
// sliders and warmth 0 (fully cool) is a level a reader may genuinely want. The brightness floor
// is applied at the brightness call sites instead.
uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint8_t>(value);
}

// See the header: a 0% level with the light still "on" would leave the sun icon lit over a dark
// panel, so brightness stops at MIN_BRIGHTNESS and the switch is what reaches off.
uint8_t clampBrightness(const int value) {
  if (value < static_cast<int>(HalFrontlight::MIN_BRIGHTNESS)) return HalFrontlight::MIN_BRIGHTNESS;
  if (value > 100) return 100;
  return static_cast<uint8_t>(value);
}
}  // namespace

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FrontlightPanel", renderer, mappedInput), UiAppHost(renderer) {}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  brightness = Frontlight.brightness();
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  lightOnChanged = false;

  resetUi();
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_LIGHT, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.on(ACTION_DISMISS, &FrontlightPanelActivity::onDismissEvent, this);
  app.on(ACTION_DARK_MODE, &FrontlightPanelActivity::onDarkModeEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  requestUpdate();
}

void FrontlightPanelActivity::onExit() {
  // brightness/warmth are always restored unconditionally on boot (see main.cpp), so they never
  // diverge from SETTINGS at onEnter() -- comparing against SETTINGS here only fires on a genuine
  // user change. lightOn has no such guarantee (see lightOnChanged's declaration), so it's gated
  // on the user actually having touched it this session instead.
  //
  // One write for the whole visit either way: a drag names a new level on every routed frame, and
  // SPIFFS sectors have a finite erase budget.
  const bool changed = SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                       (lightOnChanged && SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (changed) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    if (lightOnChanged) SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
  closeRouting();
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = clampBrightness(percentFromPermille(event.dragPermille));
  Frontlight.setBrightness(self->brightness);
  if (!self->lightOn) {
    self->lightOn = true;
    self->lightOnChanged = true;
    Frontlight.setOn(true);
  }
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->warmth = percentFromPermille(event.dragPermille);
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onToggleEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleLight();
}

void FrontlightPanelActivity::onBrightnessStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value * FINE_STEP);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value * FINE_STEP);
}

void FrontlightPanelActivity::onDismissEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->close();
}

// One tap, and the drawer goes: the screen under it is what has to come back in the other polarity, and it does on its
// next frame (ActivityManager::applyOutputPolarity), drawn afresh with the full refresh every polarity change takes.
// Left open, the drawer would composite over a copy of that screen, whose pictures were drawn for the old polarity.
void FrontlightPanelActivity::onDarkModeEvent(const fui::ActionEvent&, void* user) {
  DarkModeNow::flip();
  static_cast<FrontlightPanelActivity*>(user)->close();
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  const uint8_t next = clampBrightness(static_cast<int>(brightness) + delta);
  if (next == brightness) return;
  brightness = next;
  Frontlight.setBrightness(brightness);
  if (!lightOn) {
    lightOn = true;
    lightOnChanged = true;
    Frontlight.setOn(true);
  }
  requestUpdate();
}

void FrontlightPanelActivity::adjustWarmth(const int delta) {
  int next = static_cast<int>(warmth) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == warmth) return;
  warmth = static_cast<uint8_t>(next);
  Frontlight.setWarmth(warmth);
  requestUpdate();
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  lightOnChanged = true;
  Frontlight.setOn(lightOn);
  requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

void FrontlightPanelActivity::loop() {
  // routeHeld so the capsules track a finger: without it a contact is routed on its press and
  // release edges only, and a slider jumps to where the finger landed and then to where it
  // lifted with nothing in between.
  //
  // No draggingSlider bookkeeping here, unlike upstream: FUI's router grabs a drag to the element
  // it started on and commits a dragged contact as a drag rather than as a tap, so a drag that
  // ends over the page cannot reach the sheet's dismiss zone.
  const auto touch = routeTouch(mappedInput, /*routeHeld=*/true);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    // A handled event may have been the dismiss, which has already called finish().
    if (touch) return;
  }

  // Upstream reaches this through an Activity::handleHomeGesture() override; this fork has no
  // such hook, so the gesture is polled here instead. Same meaning either way: the global Back
  // swipe closes the drawer.
  if (mappedInput.wasHomeGesture()) {
    close();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleLight();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this] { adjustBrightness(-BUTTON_BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { adjustBrightness(BUTTON_BRIGHTNESS_STEP); });
}

LightDrawerLayout::Model FrontlightPanelActivity::layoutModel() const {
  LightDrawerLayout::Model model;
  model.brightness = brightness;
  model.warmth = warmth;
  model.lightOn = lightOn;
  model.dark = DarkModeNow::isDark();
  model.hasWarmth = Frontlight.hasColorTemperature();
  model.brightnessLabel = tr(STR_LIGHT_BRIGHTNESS);
  model.warmthLabel = tr(STR_LIGHT_WARMTH);
  return model;
}

void FrontlightPanelActivity::panelScreen(UiScreen& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::buildPanelScreen(UiScreen& screen) {
  LightDrawerLayout::build(screen, layoutModel(), layoutMetrics, panelHeight);
}

void FrontlightPanelActivity::render(RenderLock&&) {
  // The sheet covers only its own band and leaves the page around it standing, so the rest of the
  // frame must hold what is on the panel.
  //
  // NOT on the opening pass. dispatchLightPanelGesture() has already put the displayed frame
  // there, and it had to do more than a sync to manage it: in the reader the secondary buffer
  // holds Background-A's pre-rendered NEXT page rather than the displayed one, so syncing here
  // would copy that wrong page straight back over the prepared frame. (Device log: the drawer
  // opened over the previous screen with hasSecondary=1, i.e. the memcpy ran and was wrong.)
  //
  // Every LATER pass does need it: our own displayBuffer() below ends in a swap, so by the next
  // slider step the write buffer is two frames behind and the secondary holds the sheet frame we
  // just shipped -- which is exactly what a sync recovers. No pre-render can interfere, because
  // the reader is suspended while this is the current activity.
  if (!firstRender) renderer.syncWriteBufferFromDisplayed();

  // Before renderUi(): the sheet is sized at the top of the screen build. The header band is left
  // for GUI.drawHeader below: FUI has its own header component, but the firmware's header language
  // is the themes', and a drawer that titled itself differently from every other screen would read
  // as a different app.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto tokens = uiThemeTokens(uiTarget);
  layoutMetrics = LightDrawerLayout::metricsFor(tokens, static_cast<int16_t>(metrics.topPadding + metrics.headerHeight),
                                                PanelMetrics::COMPACT);
  panelHeight = LightDrawerLayout::sheetHeight(uiTarget, tokens, layoutModel(), layoutMetrics);
  renderUi();

  // After renderUi(), not before: the sheet paints its own body, which would otherwise erase the
  // header drawn onto it.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_MENU_DISP_LIGHT));

  // Plain FAST throughout. The opening pass was briefly forced to HALF while the stale-frame bug
  // was still open; the device log showed HALF did not fix it (the content was wrong, not the
  // waveform) and cost ~2950 ms against ~1550 ms, so it bought a visible flash and nothing else.
  renderer.displayBuffer();
  firstRender = false;
}

#endif  // CP_TOUCH_UI
