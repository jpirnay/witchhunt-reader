#pragma once

#include <WeatherData.h>

#include <string>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Weather forecast display activity.
 * Renders in landscape mode (800x480) with:
 * - Current conditions (left panel)
 * - Daily forecast cards (center)
 * - 48-hour temperature + precipitation graph (bottom)
 *
 * On enter: loads cache -> if stale, connects WiFi -> fetches data -> displays.
 * Each successful fetch also saves the forecast to SLEEP_IMAGE_PATH for the sleep screen.
 * Keys: Back = return, Confirm = open settings, Left = refresh.
 */
class WeatherActivity final : public Activity {
 public:
  enum class State { LOADING_CACHE, CHECK_WIFI, WIFI_SELECTION, FETCHING, WEATHER_DISPLAY, ERROR };

  // In the folder SleepActivity picks custom sleep images from.
  static constexpr char SLEEP_IMAGE_PATH[] = "/sleep/weather.bmp";

  explicit WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Weather", renderer, mappedInput) {}

  void onEnter() override;
  bool usesWifi() const override { return true; }
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  State state = State::LOADING_CACHE;
  WeatherData weatherData;
  std::string errorMessage;
  bool forceRefresh = false;
  bool showRefreshPopup = false;
  uint32_t wifiWaitStartedAtMs = 0;

  void loadAndDisplay();
  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchWeather();
  void openSettingsActivity();
  void triggerRefresh(bool showPopup);
  void saveSleepImage();

  // Render sub-sections (landscape 800x480)
  void drawForecast();
  void renderCurrentConditions(int x, int y, int w, int h);
  void renderDailyForecast(int x, int y, int w, int h);
  void renderHourlyGraph(int x, int y, int w, int h);

  bool preventAutoSleep() override { return true; }
};
