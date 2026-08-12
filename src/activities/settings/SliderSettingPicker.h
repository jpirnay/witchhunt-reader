#pragma once

#include <cstdint>

#include "SettingInfo.h"
#include "activities/SliderPickerActivity.h"

// Numeric settings edited through SliderPickerActivity rather than an inline +/- stepper.
//
// Both SettingsActivity and SettingsSubmenuActivity can host these, and each used to carry its
// own verbatim copy of the config block plus the write-back branch — so every new slider setting
// meant editing the same ladder in two files and keeping them in step. These two functions are
// that ladder, stated once.
namespace SliderSetting {

// Fills cfg for a slider-backed action and returns true. Returns false for any other action, so
// callers can use it as the "is this one of mine?" test.
bool configFor(SettingAction action, SliderPickerActivity::Config& cfg);

// Writes a value the picker returned back to the setting the action denotes. Callers still own
// persisting SETTINGS afterwards.
void apply(SettingAction action, uint8_t value);

}  // namespace SliderSetting
