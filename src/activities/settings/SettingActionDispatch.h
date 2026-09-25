#pragma once
#include <memory>

#include "SettingInfo.h"

class Activity;
class GfxRenderer;
class MappedInputManager;

// Creates the sub-activity corresponding to the given SettingAction.
// Returns nullptr for None, Submenu, or unknown actions (caller handles those).
std::unique_ptr<Activity> createActivityForAction(SettingAction action, GfxRenderer& renderer,
                                                  MappedInputManager& mappedInput);

// Creates the full-screen selector activity for a setting flagged with
// withSelectorActivity(). Returns the SD-card-aware FontSelectionActivity for the
// reader/TXT font-family settings, and the generic EnumSelectionActivity for any
// other ENUM setting. Callers own persistence in their result handler. Returns
// nullptr if the setting is not an ENUM (defensive; the flag is only set on enums).
std::unique_ptr<Activity> createSelectorActivity(const SettingInfo& setting, GfxRenderer& renderer,
                                                 MappedInputManager& mappedInput);

// The reboot-after-WiFi policy for settings actions that bring the radio up but own
// no onExit() reboot of their own: the WiFi Networks picker (which leaves the radio
// to its parent) and Weather's city search. Call from the parent's result handler,
// after settings are saved, and only when the returning activity usesWifi() -- the
// KOReader sync worker may hold the radio in the background, and an unrelated
// action must not reboot the device under it. Returns when the radio is off;
// otherwise does not return.
void restartToSettingsIfRadioLeftOn();
