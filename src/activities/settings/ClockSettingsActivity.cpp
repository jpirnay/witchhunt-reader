#include "ClockSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalCapabilities.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "DetectTimezoneActivity.h"
#include "SyncTimeActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ClockSettingsActivity::buildMenuItems() {
  menuItems.reserve(6);
  menuItems.push_back(SettingInfo::Separator(StrId::STR_SETTINGS_TITLE));
  menuItems.push_back(
      SettingInfo::Toggle(StrId::STR_USE_CLOCK, &CrossPointSettings::useClock, "useClock", StrId::STR_CAT_SYSTEM));
  menuItems.push_back(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat12h,
                                        {StrId::STR_24H, StrId::STR_12H}, "clockFormat12h", StrId::STR_CAT_SYSTEM));
  {
    auto tzSetting = SettingInfo::Enum(
        StrId::STR_TIMEZONE, &CrossPointSettings::timeZone,
        {StrId::STR_TZ_UTC, StrId::STR_TZ_CET, StrId::STR_TZ_EET, StrId::STR_TZ_MSK, StrId::STR_TZ_UTC_PLUS4,
         StrId::STR_TZ_IST, StrId::STR_TZ_UTC_PLUS7, StrId::STR_TZ_UTC_PLUS8, StrId::STR_TZ_UTC_PLUS9,
         StrId::STR_TZ_AEST, StrId::STR_TZ_NZST, StrId::STR_TZ_UTC_MINUS3, StrId::STR_TZ_EST, StrId::STR_TZ_CST,
         StrId::STR_TZ_MST, StrId::STR_TZ_PST, StrId::STR_TZ_AST_ADT, StrId::STR_TZ_ACST_ACDT, StrId::STR_TZ_AKST_AKDT},
        "timeZone", StrId::STR_CAT_SYSTEM);
    menuItems.push_back(std::move(tzSetting));
  }

  // NTP server: ACTION item edited via the on-screen keyboard (STRING types have
  // no inline on-device editor). Empty value falls back to the built-in servers.
  menuItems.push_back(SettingInfo::Action(StrId::STR_NTP_SERVER, SettingAction::None));

  menuItems.push_back(SettingInfo::Action(StrId::STR_DETECT_TIMEZONE, SettingAction::DetectTimezone)
                          .withSubcategory(StrId::STR_READER_TOOLS));
  menuItems.push_back(SettingInfo::Action(StrId::STR_SYNC_TIME, SettingAction::SyncTime));
}

std::string ClockSettingsActivity::getItemValueString(int index) const {
  const auto& item = menuItems[index];
  if (item.nameId == StrId::STR_NTP_SERVER) {
    return SETTINGS.ntpServer[0] == '\0' ? std::string(tr(STR_DEFAULT_VALUE)) : std::string(SETTINGS.ntpServer);
  }
  return MenuListActivity::getItemValueString(index);
}

void ClockSettingsActivity::onActionSelected(int index) {
  const auto& item = menuItems[index];
  if (item.nameId == StrId::STR_DETECT_TIMEZONE) {
    auto resultHandler = [](const ActivityResult&) { SETTINGS.saveToFile(); };
    startActivityForResult(std::make_unique<DetectTimezoneActivity>(renderer, mappedInput), resultHandler);
  } else if (item.nameId == StrId::STR_SYNC_TIME) {
    auto resultHandler = [](const ActivityResult&) { SETTINGS.saveToFile(); };
    startActivityForResult(std::make_unique<SyncTimeActivity>(renderer, mappedInput), resultHandler);
  } else if (item.nameId == StrId::STR_NTP_SERVER) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_NTP_SERVER),
                                                                   std::string(SETTINGS.ntpServer),
                                                                   sizeof(SETTINGS.ntpServer) - 1, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               strncpy(SETTINGS.ntpServer, kb.text.c_str(), sizeof(SETTINGS.ntpServer) - 1);
                               SETTINGS.ntpServer[sizeof(SETTINGS.ntpServer) - 1] = '\0';
                               SETTINGS.saveToFile();
                             }
                           });
  }
}

void ClockSettingsActivity::onSettingToggled(int index) {
  const auto& item = menuItems[index];
  if (item.nameId == StrId::STR_USE_CLOCK) {
    if (!SETTINGS.useClock) {
      SETTINGS.statusBarClock = 0;
    }
    SETTINGS.saveToFile();
  } else if (item.nameId == StrId::STR_TIMEZONE) {
    HalClock::applyTimezone(SETTINGS.timeZone);
    SETTINGS.saveToFile();
  } else if (item.nameId == StrId::STR_CLOCK_FORMAT) {
    SETTINGS.saveToFile();
  }
}

void ClockSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer,
                 Rect(contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight),
                 tr(STR_CLOCK_SETTINGS));

  const char* batteryWarning = tr(STR_CLOCK_SETTINGS_WARNING_BATTERY);
  const char* driftWarning = tr(STR_CLOCK_SETTINGS_WARNING_DRIFT);

  std::string warning = driftWarning;
  // The battery warning is about keeping the MCU powered to hold the time, which
  // only applies where there is no battery-backed RTC to hold it instead. Was
  // `!gpio.deviceIsX3()`, i.e. "only the X3 has an RTC" -- which now shows the
  // warning on the T5S3, whose PCF8563 makes it untrue.
  if (!HalCapabilities::hasHardwareRtc()) {
    warning = std::string(batteryWarning) + "; " + driftWarning;
  }

  GUI.drawSubHeader(renderer,
                    Rect(contentRect.x, contentRect.y + metrics.topPadding + metrics.headerHeight, contentRect.width,
                         metrics.tabBarHeight),
                    warning.c_str());

  const int contentTop =
      contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight +
                                                  metrics.verticalSpacing * 2);

  drawMenuList(Rect{contentRect.x, contentTop, contentRect.width, contentHeight});

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
