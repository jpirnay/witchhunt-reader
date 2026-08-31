#pragma once

#include <FrontlightManager.h>

// Thin firmware HAL over the SDK frontlight manager. It is inert on boards
// without a frontlight, so callers do not need board-specific conditionals.
//
// Ported verbatim from upstream/develop (crosspoint-reader#2983, Justin
// Mitchell) apart from the setBrightnessDelta() helper, which this fork adds so
// a button or gesture bound to "Light brighter/dimmer" has one place to clamp.
// Upstream reaches the same behaviour from inside FrontlightPanelActivity,
// which is FUI-based and not ported here.
class HalFrontlight {
 public:
  static HalFrontlight& getInstance() { return instance; }

  void begin(uint8_t brightness, uint8_t warmth, bool on);

  bool present() const { return manager.present(); }
  bool hasColorTemperature() const { return manager.hasColorTemperature(); }

  void setBrightness(uint8_t percent);
  void setWarmth(uint8_t warmPercent);
  void setOn(bool on);

  // Step the brightness by `delta` percent, clamped to [MIN_BRIGHTNESS, 100].
  // Lighting the panel is what setOn() is for, so a step never drops to 0 —
  // otherwise "dimmer" would have two different meanings at the bottom of the
  // range. Returns the resulting level.
  uint8_t setBrightnessDelta(int delta);

  uint8_t brightness() const { return lastBrightness; }
  uint8_t warmth() const { return manager.colorTemperature(); }
  bool isOn() const { return lit; }

  // The dimmest level the UI will select. 0 is reachable only through setOn(false):
  // a 0% "on" light is a second, worse way of saying off.
  static constexpr uint8_t MIN_BRIGHTNESS = 1;

 private:
  HalFrontlight() = default;

  FrontlightManager manager;
  // The SDK represents off as brightness 0. Keep the selected brightness so
  // toggling back on restores it.
  uint8_t lastBrightness = 60;
  bool lit = false;

  static HalFrontlight instance;
};

#define Frontlight HalFrontlight::getInstance()
