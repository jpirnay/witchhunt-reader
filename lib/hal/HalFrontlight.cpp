#include "HalFrontlight.h"

#include <BoardConfig.h>
#include <Logging.h>

#include <algorithm>
#include <optional>

#include "HalI2cBus.h"

HalFrontlight HalFrontlight::instance;

namespace {
// An LED driver on the I2C bus (BOOX Picco) shares it with the touch controller, which the input sampler polls from
// its own task, and two tasks' transfers on one bus corrupt each other (HalI2cBus.h). A PWM light takes no lock.
class LightBusLock {
 public:
  LightBusLock() {
    if (BoardConfig::hasI2cFrontlight()) lock_.emplace();
  }

 private:
  std::optional<HalI2cBus::Lock> lock_;
};
}  // namespace

void HalFrontlight::begin(const uint8_t brightness, const uint8_t warmth, const bool on) {
  const LightBusLock busLock;
  // begin() before present(): an I2C light is present only once begin() has found it on the bus. A board without a
  // light returns from begin() at once.
  manager.begin();
  if (!manager.present()) return;

  lastBrightness = brightness > 100 ? 100 : brightness;
  manager.setColorTemperature(warmth > 100 ? 100 : warmth);
  lit = on;
  manager.setBrightness(lit ? lastBrightness : 0);
  LOG_INF("LIGHT", "Frontlight up: %u%% warm=%u%% %s", lastBrightness, manager.colorTemperature(), lit ? "on" : "off");
}

void HalFrontlight::prepareForDeepSleep() {
  const LightBusLock busLock;
  // `lit` is left as it is: the next boot restores the light from the settings, not from here.
  if (BoardConfig::hasI2cFrontlight() && manager.present()) manager.setBrightness(0);
  manager.prepareForDeepSleep();
}

void HalFrontlight::setBrightness(const uint8_t percent) {
  lastBrightness = percent > 100 ? 100 : percent;
  const LightBusLock busLock;
  if (lit) manager.setBrightness(lastBrightness);
}

void HalFrontlight::setWarmth(const uint8_t warmPercent) {
  const LightBusLock busLock;
  manager.setColorTemperature(warmPercent > 100 ? 100 : warmPercent);
}

void HalFrontlight::setOn(const bool on) {
  if (on == lit) return;
  lit = on;
  const LightBusLock busLock;
  manager.setBrightness(lit ? lastBrightness : 0);
}

uint8_t HalFrontlight::setWarmthDelta(const int delta) {
  if (!hasColorTemperature()) return warmth();
  const int next = std::clamp(static_cast<int>(warmth()) + delta, 0, 100);
  setWarmth(static_cast<uint8_t>(next));
  return warmth();
}

uint8_t HalFrontlight::setBrightnessDelta(const int delta) {
  const int next = std::clamp(static_cast<int>(lastBrightness) + delta, static_cast<int>(MIN_BRIGHTNESS), 100);
  setBrightness(static_cast<uint8_t>(next));
  return lastBrightness;
}
