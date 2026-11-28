#pragma once

#include <HalCapabilities.h>

#include "ButtonActionOptions.h"

// The board's answer to ButtonActionOptions::Caps, apart from that header so the option table builds on the host.
//
// From the board profile, never from a driver that may not have answered yet: the BOOX Picco's light (an LED driver on
// I2C) is found by Frontlight.begin(), after settings.json is read.
namespace ButtonActionOptions {

inline Caps boardCaps() {
  return Caps{HalCapabilities::hasFrontlight(), HalCapabilities::hasColorTemperature(), HalCapabilities::hasBleRadio()};
}

}  // namespace ButtonActionOptions
