#include "SettingActionDispatch.h"

#include "ButtonActionsOverviewActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "ClockSettingsActivity.h"
#include "DetectTimezoneActivity.h"
#include "DictionarySelectionActivity.h"
#include "EnumSelectionActivity.h"
#include "FontDownloadActivity.h"
#include "FontSelectionActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "ReadingStatsActivity.h"
#include "SdCardFontGlobals.h"
#include "SdFirmwareUpdateActivity.h"
#include "StatusBarSettingsActivity.h"
#include "SwitchToUsbDriveActivity.h"
#include "SyncTimeActivity.h"
#include "SystemInformationActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/weather/WeatherSettingsActivity.h"

std::unique_ptr<Activity> createActivityForAction(SettingAction action, GfxRenderer& renderer,
                                                  MappedInputManager& mappedInput) {
  switch (action) {
    case SettingAction::RemapFrontButtons:
      return std::make_unique<ButtonRemapActivity>(renderer, mappedInput);
    case SettingAction::ButtonActionsOverview:
      return std::make_unique<ButtonActionsOverviewActivity>(renderer, mappedInput);
    case SettingAction::CustomiseStatusBar:
      return std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput);
    case SettingAction::DownloadFonts:
      return std::make_unique<FontDownloadActivity>(renderer, mappedInput);
    case SettingAction::ClockSettings:
      return std::make_unique<ClockSettingsActivity>(renderer, mappedInput);
    case SettingAction::KOReaderSync:
      return std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput);
    case SettingAction::OPDSBrowser:
      return std::make_unique<OpdsServerListActivity>(renderer, mappedInput);
    case SettingAction::Network:
      return std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false);
    case SettingAction::ClearCache:
      return std::make_unique<ClearCacheActivity>(renderer, mappedInput);
    case SettingAction::CheckForUpdates:
      return std::make_unique<OtaUpdateActivity>(renderer, mappedInput);
    case SettingAction::SdFirmwareUpdate:
      return std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput);
    case SettingAction::SwitchToUsbDrive:
      return std::make_unique<SwitchToUsbDriveActivity>(renderer, mappedInput);
    case SettingAction::Language:
      return std::make_unique<LanguageSelectActivity>(renderer, mappedInput);
    case SettingAction::Weather:
      return std::make_unique<WeatherSettingsActivity>(renderer, mappedInput);
    case SettingAction::SystemInfo:
      return std::make_unique<SystemInformationActivity>(renderer, mappedInput);
    case SettingAction::SyncTime:
      return std::make_unique<SyncTimeActivity>(renderer, mappedInput);
    case SettingAction::DetectTimezone:
      return std::make_unique<DetectTimezoneActivity>(renderer, mappedInput);
    case SettingAction::ReadingStats:
      return std::make_unique<ReadingStatsActivity>(renderer, mappedInput);
    case SettingAction::DictionarySelect:
      return std::make_unique<DictionarySelectionActivity>(renderer, mappedInput);
    case SettingAction::Submenu:
    case SettingAction::None:
      return nullptr;
  }
  return nullptr;
}

std::unique_ptr<Activity> createSelectorActivity(const SettingInfo& setting, GfxRenderer& renderer,
                                                 MappedInputManager& mappedInput) {
  // Font-family settings keep their dedicated selector: it enumerates SD-card font
  // families (which aren't part of the setting's static enum list) on top of the
  // built-in families. The EPUB/TXT variant is distinguished by the getter, matching
  // the sniff previously duplicated at each call site.
  if (setting.valueGetter == fontFamilyDynamicGetter || setting.valueGetter == txtFontFamilyDynamicGetter) {
    const auto target = (setting.valueGetter == txtFontFamilyDynamicGetter) ? FontSelectionActivity::Target::TXT
                                                                            : FontSelectionActivity::Target::EPUB;
    return std::make_unique<FontSelectionActivity>(renderer, mappedInput, target);
  }

  if (setting.type != SettingType::ENUM) return nullptr;
  return std::make_unique<EnumSelectionActivity>(renderer, mappedInput, setting);
}
