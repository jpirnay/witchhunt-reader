#include "WeatherActivity.h"

#include <FileReplace.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WeatherClient.h>
#include <WeatherIcons.h>
#include <WeatherSettingsStore.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "UiFontScale.h"
#include "WeatherSettingsActivity.h"
#include "activities/NetworkMemoryTrim.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/DateNames.h"
#include "components/UITheme.h"
#include "components/WeatherDraw.h"
#include "components/themes/PanelMetrics.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
// The faces. The current temperature and the detail values are the reader's NotoSans 18 bold, the temperature
// upscaled; the rest are the UI faces bound for fixed layouts (UiFontScale.h), since every box here is sized in
// constants and would overflow with the UI font size setting.
constexpr int VALUE_FONT_ID = NOTOSANS_18_FONT_ID;
constexpr int LABEL_FONT_ID = FIT_BODY_FONT_ID;
constexpr int HEADING_FONT_ID = FIT_TITLE_FONT_ID;
constexpr int FOOTNOTE_FONT_ID = FIT_SMALL_FONT_ID;

constexpr const char* DEGREE = WeatherDraw::kDegree;

constexpr int EDGE = 12;         // to the panel's edges, and to the button hints
constexpr int GRAPH_HOURS = 24;  // the days after are the forecast strip's

// Whole multiples, so GfxRenderer's area-weighted resample turns every source pixel into a whole block.
int heroTextScale() { return PanelMetrics::COMPACT ? 2 : 3; }
int heroIconScale() { return PanelMetrics::COMPACT ? 1 : 2; }

constexpr const StrId* DAY_NAME_IDS = DateNames::kShortDays;
constexpr const StrId* MONTH_NAME_IDS = DateNames::kShortMonths;

void drawWeatherIcon(const GfxRenderer& renderer, const uint8_t* icon, const int x, const int y, const int scale) {
  WeatherDraw::drawIcon(renderer, icon, x, y, scale);
}

// The forecast's times are the location's, as the API reports them (timezone=auto), not the device's.
struct tm localTime(const time_t epoch, const int utcOffsetSeconds) {
  const time_t local = epoch + utcOffsetSeconds;
  struct tm info;
  gmtime_r(&local, &info);
  return info;
}

void formatClockTime(char* buf, const size_t bufSize, const time_t epoch, const int utcOffsetSeconds) {
  const struct tm t = localTime(epoch, utcOffsetSeconds);
  if (!SETTINGS.clockFormat12h) {
    snprintf(buf, bufSize, "%02d:%02d", t.tm_hour, t.tm_min);
    return;
  }
  const int hour = t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12;
  snprintf(buf, bufSize, "%d:%02d%s", hour, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
}

const char* tempUnitLabel() { return WeatherDraw::tempUnitLabel(); }

// The API's unit parameters ("kmh", "ms") are not what a reader expects to see.
const char* windUnitLabel() {
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
  return "";
}

const char* precipUnitLabel() { return WEATHER_SETTINGS.getPrecipUnit() == WeatherPrecipUnit::INCH ? "in" : "mm"; }

// How far above its baseline `text` puts ink, at the font's own size; the font's ascender when it has no glyphs.
int inkAbove(const GfxRenderer& renderer, const int fontId, const char* text, const EpdFontFamily::Style style) {
  int above = 0;
  int below = 0;
  if (!renderer.getTextInkMetrics(fontId, text, style, &above, &below)) return renderer.getFontAscenderSize(fontId);
  return above;
}
}  // namespace

void WeatherActivity::onEnter() {
  Activity::onEnter();

  // Force landscape orientation for weather display
  {
    RenderLock lock(*this);
    renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
  }

  state = State::LOADING_CACHE;
  errorMessage.clear();
  forceRefresh = false;
  showRefreshPopup = false;
  requestUpdate();

  loadAndDisplay();
}

void WeatherActivity::onExit() {
  Activity::onExit();

  // Weather screen is always landscape; restore app UI to portrait on exit.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  // The temperature is scaled text, which takes the reader's scaled-glyph cache on first use;
  // given back here as a reader gives it back, so it does not stay under the next screen.
  renderer.releaseScaledGlyphCache();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void WeatherActivity::loadAndDisplay() {
  if (!WEATHER_SETTINGS.hasLocation()) {
    // Not an error: nothing has gone wrong yet, and a refresh cannot help. Offer the settings instead.
    state = State::LOCATION_REQUIRED;
    requestUpdate();
    return;
  }

  // Try cache first
  WeatherData cached = WeatherClient::getWeather(WEATHER_SETTINGS, forceRefresh);
  if (cached.valid) {
    // Skip the WiFi round only when the clock can vouch for the age (issue #212): on a board without an RTC the
    // clock may have come back from NVS hours behind, and an old forecast would read as minutes old.
    const time_t now = time(nullptr);
    const auto clockSource = HalClock::source();
    const bool cacheIsFresh =
        clock_authority::knownFresh(clockSource, now, cached.fetchedAt, WeatherClient::CACHE_TTL_SECONDS);
    const bool shouldBackgroundRefresh = forceRefresh || !cacheIsFresh;

    LOG_DBG("WEA", "Displayed %s cache (age=%ld s, clock %s); %s", cacheIsFresh ? "fresh" : "stale",
            static_cast<long>(now - cached.fetchedAt), clock_authority::describe(clockSource),
            shouldBackgroundRefresh ? "scheduling refresh" : "skip auto-refresh");

    weatherData = std::move(cached);
    state = State::WEATHER_DISPLAY;
    requestUpdate();

    // Refresh only when forced or when cache is stale.
    if (shouldBackgroundRefresh) {
      checkAndConnectWifi();
    }
    return;
  }

  // Need fresh data - check WiFi
  LOG_DBG("WEA", "No cache to display; proceeding to WiFi/network fetch path");
  checkAndConnectWifi();
}

void WeatherActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = State::FETCHING;
    requestUpdate(true);
    fetchWeather();
    return;
  }

  launchWifiSelection();
}

void WeatherActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  // WifiSelectionActivity draws in whatever orientation it is handed, and this screen holds the panel in landscape:
  // hand it portrait, as openSettingsActivity() does for the settings (from PR #855, trungtypo). No frame of this
  // screen in between: the picker draws over it at once.
  {
    RenderLock lock(*this);
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void WeatherActivity::onWifiSelectionComplete(bool connected) {
  // Back to landscape after the picker, which launchWifiSelection() gave portrait.
  {
    RenderLock lock(*this);
    renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
  }
  LOG_DBG("WEA", "onWifiSelectionComplete connected=%d wifiStatus=%d", connected ? 1 : 0, (int)WiFi.status());

  if (connected) {
    state = State::CHECK_WIFI;
    wifiWaitStartedAtMs = millis();
    LOG_DBG("WEA", "state -> CHECK_WIFI (waitStart=%lu)", (unsigned long)wifiWaitStartedAtMs);
    requestUpdate(true);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without further fragmenting.
    state = State::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}

void WeatherActivity::fetchWeather() {
  LOG_DBG("WEA", "fetchWeather[1] enter status=%d ip=%s", (int)WiFi.status(), WiFi.localIP().toString().c_str());
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    LOG_ERR("WEA", "fetchWeather[2] WiFi not ready status=%d ip=%s", (int)WiFi.status(),
            WiFi.localIP().toString().c_str());
    state = State::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
    return;
  }

  trimMemoryForNetworkSession(renderer, "WEA");
  LOG_DBG("WEA", "fetchWeather[3] WeatherClient::getWeather before");
  weatherData = WeatherClient::getWeather(WEATHER_SETTINGS, true);
  LOG_DBG("WEA", "fetchWeather[4] WeatherClient::getWeather after valid=%d", weatherData.valid ? 1 : 0);
  if (weatherData.valid) {
    state = State::WEATHER_DISPLAY;
    saveSleepImage();
  } else {
    state = State::ERROR;
    errorMessage = weatherData.errorMessage.empty() ? tr(STR_WEATHER_FETCH_FAILED) : weatherData.errorMessage.c_str();
  }
  showRefreshPopup = false;
  requestUpdate();
}

// Issue #212: the forecast as a picture in the sleep screen's image folder, rewritten on every fetch. Drawn into the
// write buffer before the screen's own render, which clears it again, so the frame on the panel is not touched. The
// BMP is the panel's pixels in portrait (ScreenshotUtil), so the sleep screen shows it the way the panel did.
void WeatherActivity::saveSleepImage() {
  static constexpr char kTmpPath[] = "/sleep/weather.bmp.tmp";  // not a .bmp: the sleep screen never picks it

  // Held across the card write, as ScreenshotUtil::service() holds it: the render task would draw over the frame.
  RenderLock lock(*this);
  renderer.clearScreen();
  drawForecast(EDGE);  // without the button hints: there are no buttons to press on a sleep screen
  if (!ScreenshotUtil::saveFramebufferAsBmp(kTmpPath, renderer, GfxRenderer::Portrait)) {
    LOG_ERR("WEA", "Could not write %s", kTmpPath);
    return;
  }
  if (!FileReplace::replace(kTmpPath, SLEEP_IMAGE_PATH)) {
    LOG_ERR("WEA", "Could not replace %s", SLEEP_IMAGE_PATH);
    Storage.remove(kTmpPath);
    return;
  }
  LOG_DBG("WEA", "Forecast saved to %s", SLEEP_IMAGE_PATH);
}

void WeatherActivity::openSettingsActivity() {
  {
    RenderLock lock(*this);
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  }
  startActivityForResult(std::make_unique<WeatherSettingsActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) {
                           {
                             RenderLock lock(*this);
                             renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
                           }
                           forceRefresh = true;
                           loadAndDisplay();
                         });
}

void WeatherActivity::triggerRefresh(const bool showPopup) {
  showRefreshPopup = showPopup;
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = State::FETCHING;
    requestUpdate(true);
    fetchWeather();
  } else {
    launchWifiSelection();
  }
}

void WeatherActivity::loop() {
  if (state == State::WIFI_SELECTION) {
    return;  // Handled by WifiSelectionActivity
  }

  if (state == State::LOCATION_REQUIRED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openSettingsActivity();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openSettingsActivity();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      triggerRefresh(false);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::LOADING_CACHE || state == State::CHECK_WIFI || state == State::FETCHING) {
    if (state == State::CHECK_WIFI) {
      LOG_DBG("WEA", "loop CHECK_WIFI status=%d ip=%s elapsed=%lu", (int)WiFi.status(),
              WiFi.localIP().toString().c_str(), (unsigned long)(millis() - wifiWaitStartedAtMs));
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        // Give lwIP/sockets a short window to settle after activity handoff.
        if (wifiWaitStartedAtMs > 0 && millis() - wifiWaitStartedAtMs < 1200) {
          LOG_DBG("WEA", "loop CHECK_WIFI waiting for settle window");
          return;
        }
        LOG_DBG("WEA", "state CHECK_WIFI -> FETCHING");
        state = State::FETCHING;
        requestUpdate(true);
        fetchWeather();
        return;
      }

      if (wifiWaitStartedAtMs > 0 && millis() - wifiWaitStartedAtMs > 15000) {
        LOG_ERR("WEA", "CHECK_WIFI timeout after %lu ms", (unsigned long)(millis() - wifiWaitStartedAtMs));
        state = State::ERROR;
        errorMessage = tr(STR_WIFI_CONN_FAILED);
        requestUpdate();
        return;
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  // WEATHER_DISPLAY state
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSettingsActivity();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
             mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    triggerRefresh(true);
  }
}

void WeatherActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageHeight = renderer.getScreenHeight();  // 480 in landscape

  if (state == State::LOCATION_REQUIRED) {
    renderer.drawCenteredText(FIT_TITLE_FONT_ID, pageHeight / 2 - 20, tr(STR_WEATHER), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(FIT_BODY_FONT_ID, pageHeight / 2 + 10, tr(STR_WEATHER_NO_LOCATION));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_WEATHER_SETTINGS_SHORT), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Waiting on the network with no cache: the forecast below would draw an empty one (from PR #855, trungtypo).
  const bool waitingWithoutData = !weatherData.valid && (state == State::CHECK_WIFI || state == State::WIFI_SELECTION);
  if (state == State::LOADING_CACHE || waitingWithoutData ||
      (state == State::FETCHING && (!showRefreshPopup || !weatherData.valid))) {
    renderer.drawCenteredText(FIT_TITLE_FONT_ID, pageHeight / 2, tr(STR_LOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::ERROR) {
    renderer.drawCenteredText(FIT_TITLE_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(FIT_BODY_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), tr(STR_WEATHER_SETTINGS_SHORT), tr(STR_WEATHER_REFRESH), "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // In LandscapeClockwise the physical bottom buttons, and so their hints, are at the logical left edge.
  drawForecast(UITheme::getInstance().getMetrics().buttonHintsHeight + EDGE);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_WEATHER_SETTINGS_SHORT), tr(STR_WEATHER_REFRESH), "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (state == State::FETCHING && showRefreshPopup) {
    // Full weather frame already composed into the write buffer above; overlay the popup on it.
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP), /*overlayDisplayedFrame=*/false);
  } else {
    renderer.displayBuffer();
  }
}

// Landscape, `left` onwards:
//
//   [icon] 12 °C                    Lisbon
//          Feels like 9°       Sun, Nov 15
//                        Sun  Mon  Tue  Wed  Thu
//                        [ic] [ic] [ic] [ic] [ic]
//                        14|8 11|6 ...
//   Sunrise    Sunset    | 24 h temperature line over
//   Wind       Humidity  | the chance of rain, 0-100 %
//   UV index   Precip.   |
//                                   Updated 10:27
//
// A compact panel held sideways (Murphy) keeps the top and the days, across the whole width.
void WeatherActivity::drawForecast(const int left) {
  const int right = renderer.getScreenWidth() - EDGE;
  const int screenHeight = renderer.getScreenHeight();
  const int footerTop = screenHeight - renderer.getLineHeight(FOOTNOTE_FONT_ID) - 2;
  const bool compact = PanelMetrics::isShort(screenHeight);
  const int splitX = left + (right - left) * (compact ? 50 : 46) / 100;

  const int currentBottom = drawCurrentConditions(left, splitX);
  const int placeBottom = drawPlaceAndDate(splitX, right, compact ? HEADING_FONT_ID : VALUE_FONT_ID);
  if (compact) {
    drawDays(left, right, std::max(currentBottom, placeBottom) + 4);
  } else {
    const int bodyTop = std::max(currentBottom, drawDays(splitX, right, placeBottom + 8)) + 12;
    drawDetails(left, bodyTop, splitX - left, footerTop - bodyTop);
    drawHourlyGraph(splitX, bodyTop, right - splitX, footerTop - bodyTop);
  }

  if (weatherData.fetchedAt > 0) {
    char timeBuf[16];
    formatClockTime(timeBuf, sizeof(timeBuf), weatherData.fetchedAt, weatherData.utcOffsetSeconds);
    char footer[48];
    snprintf(footer, sizeof(footer), "%s %s", tr(STR_WEATHER_UPDATED), timeBuf);
    renderer.drawText(FOOTNOTE_FONT_ID, right - renderer.getTextWidth(FOOTNOTE_FONT_ID, footer), footerTop, footer);
  }
}

// The icon, the temperature as large as the panel takes it, and what it feels like. Returns the bottom.
int WeatherActivity::drawCurrentConditions(const int left, const int right) {
  const auto& cur = weatherData.current;
  const int top = EDGE;
  const int iconScale = heroIconScale();
  const int iconSize = WEATHER_ICON_SIZE * iconScale;
  drawWeatherIcon(renderer, getWeatherIconLarge(getWeatherIconType(cur.weatherCode, cur.isDay)), left, top, iconScale);

  char temp[16];
  snprintf(temp, sizeof(temp), "%d", weather::whole(cur.temperature, weather::TENTHS_DECIMALS));
  const int scale = heroTextScale();
  const TextScale textScale = TextScale::ratio(scale, 1);
  const int digitsHeight = inkAbove(renderer, VALUE_FONT_ID, temp, EpdFontFamily::BOLD) * scale;
  const int digitsTop = top + std::max(0, (iconSize - digitsHeight) / 2);  // centred on the icon
  const int baseline = digitsTop + digitsHeight;
  const int tempX = left + iconSize + 12;
  renderer.drawTextScaled(VALUE_FONT_ID, tempX, baseline - renderer.getFontAscenderSizeScaled(VALUE_FONT_ID, textScale),
                          temp, true, EpdFontFamily::BOLD, textScale);

  // The unit's ink top level with the digits'.
  const char* unit = tempUnitLabel();
  const int unitX = tempX + renderer.getTextWidthScaled(VALUE_FONT_ID, temp, EpdFontFamily::BOLD, textScale) + 6;
  renderer.drawText(HEADING_FONT_ID, unitX,
                    digitsTop - renderer.getFontAscenderSize(HEADING_FONT_ID) +
                        inkAbove(renderer, HEADING_FONT_ID, unit, EpdFontFamily::BOLD),
                    unit, true, EpdFontFamily::BOLD);

  char feels[48];
  snprintf(feels, sizeof(feels), "%s %d%s", tr(STR_WEATHER_FEELS_LIKE),
           weather::whole(cur.apparentTemperature, weather::TENTHS_DECIMALS), DEGREE);
  const int feelsTop = baseline + 8;
  renderer.drawText(HEADING_FONT_ID, tempX, feelsTop,
                    renderer.truncatedText(HEADING_FONT_ID, feels, right - tempX - 8).c_str());
  return std::max(top + iconSize, feelsTop + renderer.getLineHeight(HEADING_FONT_ID));
}

// The place and the day the forecast was fetched, right-aligned. Returns the bottom.
int WeatherActivity::drawPlaceAndDate(const int left, const int right, const int placeFontId) {
  int y = EDGE / 2;
  const std::string place = WEATHER_SETTINGS.getLocationName();
  if (!place.empty()) {
    const auto shown = renderer.truncatedText(placeFontId, place.c_str(), right - left, EpdFontFamily::BOLD);
    renderer.drawText(placeFontId, right - renderer.getTextWidth(placeFontId, shown.c_str(), EpdFontFamily::BOLD), y,
                      shown.c_str(), true, EpdFontFamily::BOLD);
    // The line box of a reader face is generous; the date sits under the place's ink, not under its leading.
    y += renderer.getFontAscenderSize(placeFontId) + 6;
  }

  const time_t day =
      weatherData.fetchedAt > 0 ? weatherData.fetchedAt : (weatherData.daily.empty() ? 0 : weatherData.daily[0].date);
  if (day > 0) {
    const struct tm t = localTime(day, weatherData.utcOffsetSeconds);
    char date[48];
    snprintf(date, sizeof(date), "%s, %s %d", I18N.get(DAY_NAME_IDS[t.tm_wday]), I18N.get(MONTH_NAME_IDS[t.tm_mon]),
             t.tm_mday);
    renderer.drawText(HEADING_FONT_ID, right - renderer.getTextWidth(HEADING_FONT_ID, date), y, date);
    y += renderer.getLineHeight(HEADING_FONT_ID);
  }
  return y;
}

// One column per forecast day: name, icon, high|low. As many as fit their icon. Returns the bottom.
int WeatherActivity::drawDays(const int left, const int right, const int top) {
  if (weatherData.daily.empty()) return top;
  const int count =
      std::min(static_cast<int>(weatherData.daily.size()), std::max(1, (right - left) / (WEATHER_ICON_SIZE + 8)));

  // Weekdays counted on from the first day rather than each converted: a timezone edge once repeated a name.
  const int firstWeekday = localTime(weatherData.daily[0].date, weatherData.utcOffsetSeconds).tm_wday;
  const int nameHeight = renderer.getLineHeight(HEADING_FONT_ID);
  const int iconTop = top + nameHeight;
  const int tempsTop = iconTop + WEATHER_ICON_SIZE + 2;
  const int columnWidth = (right - left) / count;

  for (int i = 0; i < count; i++) {
    const auto& day = weatherData.daily[i];
    const int centre = left + i * columnWidth + columnWidth / 2;

    const char* name = I18N.get(DAY_NAME_IDS[(firstWeekday + i) % 7]);
    renderer.drawText(HEADING_FONT_ID, centre - renderer.getTextWidth(HEADING_FONT_ID, name, EpdFontFamily::BOLD) / 2,
                      top, name, true, EpdFontFamily::BOLD);

    drawWeatherIcon(renderer, getWeatherIconLarge(getWeatherIconType(day.weatherCode, true)),
                    centre - WEATHER_ICON_SIZE / 2, iconTop, 1);

    char temps[24];
    snprintf(temps, sizeof(temps), "%d%s|%d%s", weather::whole(day.tempMax, weather::TENTHS_DECIMALS), DEGREE,
             weather::whole(day.tempMin, weather::TENTHS_DECIMALS), DEGREE);
    renderer.drawText(LABEL_FONT_ID, centre - renderer.getTextWidth(LABEL_FONT_ID, temps) / 2, tempsTop, temps);
  }
  return tempsTop + renderer.getLineHeight(LABEL_FONT_ID);
}

// Today's details, a label over each value, in two columns of three.
void WeatherActivity::drawDetails(const int x, const int y, const int w, const int h) {
  struct Detail {
    StrId label;
    char value[16];
    char unit[24];
  };
  Detail details[6];
  int count = 0;
  const auto add = [&](const StrId label, const char* value, const char* unit) {
    Detail& d = details[count++];
    d.label = label;
    snprintf(d.value, sizeof(d.value), "%s", value);
    snprintf(d.unit, sizeof(d.unit), "%s", unit);
  };

  const auto& cur = weatherData.current;
  char value[16];
  char unit[24];
  if (!weatherData.daily.empty()) {
    formatClockTime(value, sizeof(value), weatherData.daily[0].sunrise, weatherData.utcOffsetSeconds);
    add(StrId::STR_WEATHER_SUNRISE, value, "");
    formatClockTime(value, sizeof(value), weatherData.daily[0].sunset, weatherData.utcOffsetSeconds);
    add(StrId::STR_WEATHER_SUNSET, value, "");
  }
  snprintf(value, sizeof(value), "%d", weather::whole(cur.windSpeed, weather::TENTHS_DECIMALS));
  snprintf(unit, sizeof(unit), "%s %s", windUnitLabel(), getWindDirectionText(cur.windDirection));
  add(StrId::STR_WEATHER_WIND, value, unit);
  snprintf(value, sizeof(value), "%d", cur.humidity);
  add(StrId::STR_WEATHER_HUMIDITY, value, "%");
  snprintf(value, sizeof(value), "%d", weather::whole(cur.uvIndex, weather::HUNDREDTHS_DECIMALS));
  add(StrId::STR_WEATHER_UV_INDEX, value, "");
  if (!weatherData.daily.empty()) {
    weather::formatPrecipitation(value, sizeof(value), weatherData.daily[0].precipSum);
    add(StrId::STR_WEATHER_PRECIP, value, precipUnitLabel());
  }

  const int columnWidth = w / 2;
  const int rowHeight = h / 3;
  const int labelHeight = renderer.getLineHeight(LABEL_FONT_ID);
  const int labelAscender = renderer.getFontAscenderSize(LABEL_FONT_ID);
  const int valueInk = inkAbove(renderer, VALUE_FONT_ID, "0", EpdFontFamily::BOLD);
  const int valueAscender = renderer.getFontAscenderSize(VALUE_FONT_ID);

  for (int i = 0; i < count; i++) {
    const Detail& d = details[i];
    const int cx = x + (i % 2) * columnWidth;
    const int rowTop = y + (i / 2) * rowHeight;
    renderer.drawText(LABEL_FONT_ID, cx, rowTop,
                      renderer.truncatedText(LABEL_FONT_ID, I18N.get(d.label), columnWidth - 8).c_str());

    const int baseline = rowTop + labelHeight + valueInk;
    renderer.drawText(VALUE_FONT_ID, cx, baseline - valueAscender, d.value, true, EpdFontFamily::BOLD);
    if (d.unit[0] != '\0') {
      const int unitX = cx + renderer.getTextWidth(VALUE_FONT_ID, d.value, EpdFontFamily::BOLD) + 5;
      renderer.drawText(LABEL_FONT_ID, unitX, baseline - labelAscender,
                        renderer.truncatedText(LABEL_FONT_ID, d.unit, cx + columnWidth - 8 - unitX).c_str());
    }
  }
}

// The next 24 hours: temperature on the left axis, the chance of rain as bars on the right one. A fixed 0-100 %
// scale, because bars scaled to the day's wettest hour made a 0.2 mm drizzle look like a downpour.
void WeatherActivity::drawHourlyGraph(const int x, const int y, const int w, const int h) {
  const auto& hours = weatherData.hourly;
  const int count = std::min(static_cast<int>(hours.size()), GRAPH_HOURS);
  if (count < 2) return;

  weather::Tenths lowest = hours[0].temperature;
  weather::Tenths highest = lowest;
  for (int i = 1; i < count; i++) {
    lowest = std::min(lowest, hours[i].temperature);
    highest = std::max(highest, hours[i].temperature);
  }
  // Five grid lines on whole degrees, with a degree of air above and below the curve.
  const int tempMin = weather::floorWhole(lowest) - 1;
  const int tempStep = std::max(1, (weather::ceilWhole(highest) + 1 - tempMin + 3) / 4);
  const int tempMax = tempMin + 4 * tempStep;

  char label[16];
  snprintf(label, sizeof(label), "%d%s", tempMin, DEGREE);
  int leftLabelWidth = renderer.getTextWidth(LABEL_FONT_ID, label);
  snprintf(label, sizeof(label), "%d%s", tempMax, DEGREE);
  leftLabelWidth = std::max(leftLabelWidth, renderer.getTextWidth(LABEL_FONT_ID, label));

  const int labelHeight = renderer.getLineHeight(LABEL_FONT_ID);
  const int labelAscender = renderer.getFontAscenderSize(LABEL_FONT_ID);
  const int labelDigitInk = inkAbove(renderer, LABEL_FONT_ID, "0", EpdFontFamily::REGULAR);
  const int plotLeft = x + leftLabelWidth + 8;
  const int plotRight = x + w - renderer.getTextWidth(LABEL_FONT_ID, "100%") - 8;
  const int plotTop = y + labelHeight;             // room for a day name over midnight
  const int plotBottom = y + h - labelHeight - 4;  // room for the hours under the axis
  const int plotWidth = plotRight - plotLeft;
  const int plotHeight = plotBottom - plotTop;
  if (plotWidth < count || plotHeight < 40) return;

  const auto xAt = [&](const int i) { return plotLeft + i * plotWidth / (count - 1); };
  // Rounded to the nearest pixel, a half up: t is above tempMin, so the quotient is positive.
  const auto yAt = [&](const weather::Tenths t) {
    const int64_t above = static_cast<int64_t>(t - tempMin * 10) * plotHeight;
    const int64_t span = static_cast<int64_t>(tempMax - tempMin) * 10;
    return plotBottom - static_cast<int>((2 * above + span) / (2 * span));
  };

  for (int k = 0; k <= 4; k++) {
    const int gy = plotBottom - k * plotHeight / 4;
    for (int gx = plotLeft; gx < plotRight; gx += 4) renderer.drawPixel(gx, gy);
    const int labelTop = gy - labelAscender + labelDigitInk / 2;  // digits centred on the line
    snprintf(label, sizeof(label), "%d%s", tempMin + k * tempStep, DEGREE);
    renderer.drawText(LABEL_FONT_ID, plotLeft - 8 - renderer.getTextWidth(LABEL_FONT_ID, label), labelTop, label);
    snprintf(label, sizeof(label), "%d%%", k * 25);
    renderer.drawText(LABEL_FONT_ID, plotRight + 8, labelTop, label);
  }

  const int barWidth = std::max(1, plotWidth / (count - 1) - 1);
  for (int i = 0; i < count; i++) {
    const int chance = std::clamp(hours[i].precipitationProbability, 0, 100);
    if (chance == 0) continue;
    const int barHeight = std::max(1, chance * plotHeight / 100);
    const int barLeft = std::max(plotLeft + 1, xAt(i) - barWidth / 2);
    const int barRight = std::min(plotRight, xAt(i) - barWidth / 2 + barWidth);
    if (barRight > barLeft) {
      renderer.fillRectDither(barLeft, plotBottom - barHeight, barRight - barLeft, barHeight, Color::DarkGray);
    }
  }

  for (int i = 1; i < count; i++) {
    renderer.drawLine(xAt(i - 1), yAt(hours[i - 1].temperature), xAt(i), yAt(hours[i].temperature), 2, true);
  }
  renderer.drawLine(plotLeft, plotTop, plotLeft, plotBottom);
  renderer.fillRect(plotLeft, plotBottom, plotWidth + 1, 2);

  // Every sixth hour on the axis; a line and the day's name at midnight.
  for (int i = 0; i < count; i++) {
    const struct tm t = localTime(hours[i].time, weatherData.utcOffsetSeconds);
    if (t.tm_hour % 6 != 0) continue;
    const int px = xAt(i);
    for (int gy = plotTop; gy < plotBottom; gy += 4) renderer.drawPixel(px, gy);
    if (SETTINGS.clockFormat12h) {
      snprintf(label, sizeof(label), "%d%s", t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12, t.tm_hour < 12 ? "AM" : "PM");
    } else {
      snprintf(label, sizeof(label), "%02d", t.tm_hour);
    }
    renderer.drawText(LABEL_FONT_ID, px - renderer.getTextWidth(LABEL_FONT_ID, label) / 2, plotBottom + 4, label);
    if (t.tm_hour == 0 && i > 0) {
      renderer.drawLine(px, plotTop - 4, px, plotBottom);
      const char* name = I18N.get(DAY_NAME_IDS[t.tm_wday]);
      const int nameWidth = renderer.getTextWidth(LABEL_FONT_ID, name, EpdFontFamily::BOLD);
      renderer.drawText(LABEL_FONT_ID, std::min(px + 4, plotRight - nameWidth), y, name, true, EpdFontFamily::BOLD);
    }
  }
}
