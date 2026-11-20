#include "WeatherSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WeatherClient.h>
#include <WeatherSettingsStore.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "WeatherCityResultsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

void WeatherSettingsActivity::buildMenuItems() {
  menuItems.reserve(10);
  menuItems.push_back(SettingInfo::Separator(StrId::STR_SETTINGS_TITLE));
  menuItems.push_back(SettingInfo::Toggle(StrId::STR_USE_WEATHER, &CrossPointSettings::useWeather, "useWeather",
                                          StrId::STR_CAT_SYSTEM));

  menuItems.push_back(SettingInfo::Action(StrId::STR_WEATHER_SEARCH_CITY, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_WEATHER_LATITUDE, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_WEATHER_LONGITUDE, SettingAction::None));

  menuItems.push_back(SettingInfo::Separator(StrId::STR_WEATHER_UNITS));
  menuItems.push_back(SettingInfo::Action(StrId::STR_WEATHER_TEMP_UNIT, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_WEATHER_WIND_UNIT, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_WEATHER_PRECIP_UNIT, SettingAction::None));
}

std::string WeatherSettingsActivity::getItemValueString(int index) const {
  const auto& item = menuItems[index];
  switch (item.nameId) {
    case StrId::STR_WEATHER_SEARCH_CITY: {
      auto name = WEATHER_SETTINGS.getLocationName();
      return name.empty() ? std::string(tr(STR_NOT_SET)) : name;
    }
    case StrId::STR_WEATHER_LATITUDE: {
      char buf[16];
      snprintf(buf, sizeof(buf), "%.4f", WEATHER_SETTINGS.getLatitude());
      return std::string(buf);
    }
    case StrId::STR_WEATHER_LONGITUDE: {
      char buf[16];
      snprintf(buf, sizeof(buf), "%.4f", WEATHER_SETTINGS.getLongitude());
      return std::string(buf);
    }
    case StrId::STR_WEATHER_TEMP_UNIT:
      return WEATHER_SETTINGS.getTempUnit() == WeatherTempUnit::CELSIUS ? "C" : "F";
    case StrId::STR_WEATHER_WIND_UNIT:
      switch (WEATHER_SETTINGS.getWindUnit()) {
        case WeatherWindUnit::KMH:
          return "km/h";
        case WeatherWindUnit::MS:
          return "m/s";
        case WeatherWindUnit::MPH:
          return "mph";
        case WeatherWindUnit::KNOTS:
          return "kn";
      }
      return "km/h";
    case StrId::STR_WEATHER_PRECIP_UNIT:
      return WEATHER_SETTINGS.getPrecipUnit() == WeatherPrecipUnit::MM ? "mm" : "in";
    default:
      return MenuListActivity::getItemValueString(index);
  }
}

void WeatherSettingsActivity::onActionSelected(int index) {
  const auto& item = menuItems[index];
  if (item.nameId == StrId::STR_WEATHER_SEARCH_CITY) {
    launchCitySearch();
  } else if (item.nameId == StrId::STR_WEATHER_LATITUDE) {
    launchLatitudeEntry();
  } else if (item.nameId == StrId::STR_WEATHER_LONGITUDE) {
    launchLongitudeEntry();
  } else if (item.nameId == StrId::STR_WEATHER_TEMP_UNIT) {
    auto current = WEATHER_SETTINGS.getTempUnit();
    WEATHER_SETTINGS.setTempUnit(current == WeatherTempUnit::CELSIUS ? WeatherTempUnit::FAHRENHEIT
                                                                     : WeatherTempUnit::CELSIUS);
    WEATHER_SETTINGS.saveToFile();
    requestUpdate();
  } else if (item.nameId == StrId::STR_WEATHER_WIND_UNIT) {
    auto current = static_cast<uint8_t>(WEATHER_SETTINGS.getWindUnit());
    WEATHER_SETTINGS.setWindUnit(static_cast<WeatherWindUnit>((current + 1) % 4));
    WEATHER_SETTINGS.saveToFile();
    requestUpdate();
  } else if (item.nameId == StrId::STR_WEATHER_PRECIP_UNIT) {
    auto current = WEATHER_SETTINGS.getPrecipUnit();
    WEATHER_SETTINGS.setPrecipUnit(current == WeatherPrecipUnit::MM ? WeatherPrecipUnit::INCH : WeatherPrecipUnit::MM);
    WEATHER_SETTINGS.saveToFile();
    requestUpdate();
  }
}

void WeatherSettingsActivity::onSettingToggled(int index) {
  if (menuItems[index].nameId == StrId::STR_USE_WEATHER) {
    SETTINGS.saveToFile();
  }
}

void WeatherSettingsActivity::launchCitySearch() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEATHER_SEARCH_CITY), "",
                                                                 64, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;

                           const auto& kb = std::get<KeyboardResult>(result.data);
                           if (kb.text.empty()) return;

                           if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
                             startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                                                    [this, query = kb.text](const ActivityResult& wifiResult) {
                                                      if (!wifiResult.isCancelled) showCityResults(query);
                                                    });
                             return;
                           }
                           showCityResults(kb.text);
                         });
}

// The search is a blocking request on the loop task, as it always was; the matches then open as
// their own list, also when there are none, so an empty search says so. This runs inside a result
// handler (the keyboard's, or the Wi-Fi picker's). ActivityManager moves a handler out before
// running it, so starting another activity from one is supported, the same way the keyboard handler
// above starts the Wi-Fi picker.
void WeatherSettingsActivity::showCityResults(const std::string& query) {
  startActivityForResult(
      std::make_unique<WeatherCityResultsActivity>(renderer, mappedInput, WeatherClient::searchCity(query)),
      [this](const ActivityResult&) { requestUpdate(); });  // the Location row shows a pick
}

void WeatherSettingsActivity::launchLatitudeEntry() {
  std::string current = std::to_string(WEATHER_SETTINGS.getLatitude());
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEATHER_LATITUDE),
                                                                 current, 12, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const auto& kb = std::get<KeyboardResult>(result.data);
                             char* end = nullptr;
                             const float lat = strtof(kb.text.c_str(), &end);
                             if (end != kb.text.c_str() && *end == '\0' && lat >= -90.0f && lat <= 90.0f) {
                               if (lat != WEATHER_SETTINGS.getLatitude()) {
                                 WEATHER_SETTINGS.setLocation(lat, WEATHER_SETTINGS.getLongitude(), "");
                               }
                               WEATHER_SETTINGS.saveToFile();
                               requestUpdate();
                             }
                           }
                         });
}

void WeatherSettingsActivity::launchLongitudeEntry() {
  std::string current = std::to_string(WEATHER_SETTINGS.getLongitude());
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEATHER_LONGITUDE),
                                                                 current, 12, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const auto& kb = std::get<KeyboardResult>(result.data);
                             char* end = nullptr;
                             const float lon = strtof(kb.text.c_str(), &end);
                             if (end != kb.text.c_str() && *end == '\0' && lon >= -180.0f && lon <= 180.0f) {
                               if (lon != WEATHER_SETTINGS.getLongitude()) {
                                 WEATHER_SETTINGS.setLocation(WEATHER_SETTINGS.getLatitude(), lon, "");
                               }
                               WEATHER_SETTINGS.saveToFile();
                               requestUpdate();
                             }
                           }
                         });
}

void WeatherSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = listContentRect();

  GUI.drawHeader(renderer, listHeaderRect(), tr(STR_WEATHER_SETTINGS));

  const int contentTop = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      contentRect.height - (metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2);

  drawMenuList(Rect{contentRect.x, contentTop, contentRect.width, contentHeight});

  drawListHints();

  renderer.displayBuffer();
}
