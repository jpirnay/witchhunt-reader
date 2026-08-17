#include "HalClock.h"

#include <Arduino.h>
#include <HalCapabilities.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <Preferences.h>
#include <Rtc.h>  // SDK driver, used for the PCF8563 family (DS3231 is handled inline below)
#include <WiFi.h>
#include <Wire.h>  // Needed for I2C communication with the RTC
#include <esp_private/esp_clk.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include <cmath>
#include <cstdlib>

#include "HalI2cBus.h"

// ---- RTC / I2C configuration ----------------------------------------------
// Pins for ESP32-C3 (according to https://gist.github.com/CrazyCoder/1c5f846adee18e21f91e264601a6ddce)
// The DS3231's own I2C address. Read from the board profile rather than
// hardcoded, but note the guard in initExternalRTC() is rtcType() == Ds3231, so
// this is only ever used on a board that actually has one -- it is not a generic
// "the RTC" address. A BM8563 answers at 0x51 with different registers and needs
// its own driver, not this one with a different constant.
static uint8_t ds3231Address() { return BoardConfig::ACTIVE.sensors.rtcAddr; }
// static constexpr int I2C_SDA = 8;
// static constexpr int I2C_SCL = 9;
static uint8_t bin2bcd(uint8_t val) { return val + 6 * (val / 10); }
static uint8_t bcd2bin(uint8_t val) { return val - 6 * (val >> 4); }

/**
 * Convert struct tm (interpreted as UTC) to Unix epoch seconds.
 * Replaces mktime(), as mktime considers the local timezone (TZ).
 */
static time_t timegm_compat(const struct tm* tm) {
  int32_t year = tm->tm_year + 1900;
  int32_t month = tm->tm_mon;  // 0-11

  // Helper calculation: days since the beginning of the year
  static const uint16_t days_before_month[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

  // Days since 1970 (considering leap years)
  time_t days = (year - 1970) * 365 + (year - 1969) / 4;
  days += days_before_month[month];

  // Leap year correction for the current year (no extra day before March)
  if (month > 1 && (year % 4 == 0)) {
    days++;
  }
  days += tm->tm_mday - 1;

  return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

// ---- RTC-memory state (survives deep sleep, not cold boot) ----------------

static constexpr uint32_t CLOCK_RTC_MAGIC = 0xC10C4B1D;
static constexpr uint32_t CLOCK_RTC_FLAG_LP_VALID = 0x00000001u;

// Temperature drift model for ESP32 RTC-based timekeeping.
//
// The chip's low-power (slow) clock frequency depends on temperature.
// ESP32 variants can drift by about 2 minutes per day per °C from the
// initial captured operating temperature.
//
// - dt_drift ≈ 120 seconds/day/°C
// - relative frequency error per second per °C = 120 / 86400
//
// At restore() we apply a first-order correction over the sleep interval:
// corrected_interval = raw_interval × (1 + ΔT × drift_factor), where
// drift_factor = 120 / 86400.
//
// Experimental source: https://www.reddit.com/r/esp32/comments/11cikkp/the_clock_on_the_esp_is_wrong/
static constexpr float CLOCK_TEMP_DRIFT_SECONDS_PER_SECOND_PER_DEG = 120.0f / 86400.0f;

RTC_NOINIT_ATTR static uint32_t rtcClockMagic;
RTC_NOINIT_ATTR static uint32_t rtcClockFlags;
RTC_NOINIT_ATTR static time_t rtcEpoch;        // last-known unix epoch
RTC_NOINIT_ATTR static uint64_t rtcLpTimeUs;   // esp_clk_rtc_time() at capture
RTC_NOINIT_ATTR static uint32_t rtcSlowCal;    // esp_clk_slowclk_cal_get() at capture
RTC_NOINIT_ATTR static float rtcTemperatureC;  // captured chip temperature at save
RTC_NOINIT_ATTR static uint32_t rtcStateChecksum;

static bool clockApproximate = true;

// How long the deep sleep that this boot woke from lasted, in seconds; 0 when it
// cannot be established. Set once by restore(), reported on the System Information
// screen. Only the two authoritative restore paths can produce it: the DS3231 and
// the LP-timer correction both give a wall clock derived independently of the
// sleep-entry epoch, so the difference between them is real elapsed time. The
// cold-boot NVS path sets the clock FROM that same epoch, so its difference is
// zero by construction and tells us nothing.
static uint32_t lastSleepSec = 0;
// Sanity bound on the above, shared by both paths: a year. Anything larger means
// a stale NVS epoch or a corrupt snapshot, not a real sleep.
static constexpr double MAX_PLAUSIBLE_SLEEP_S = 365.0 * 24 * 3600;

// Drift correction scale factor (learned from NTP sync results).
//
// Raw temp drift model uses 2 min/day/°C -> factor = 120/86400. This is a
// generic base model. The actual board may behave a bit differently. On each
// NTP sync we estimate how the local clock error compares to the model and
// update this scale factor slightly to converge toward real world behavior.
//
// rtcDriftScale = 1.0 means we trust 2 min/day/°C exactly. If the device is
// slower/faster than that, NTP drift calibration adjusts this factor.
static float rtcDriftScale = 1.0f;

static unsigned long lastPeriodicUpdateMs = 0;
static constexpr unsigned long PERIODIC_UPDATE_INTERVAL_MS = 10UL * 60UL * 1000UL;

struct TimeZoneEntry {
  const char* tz;
};

static constexpr TimeZoneEntry TIMEZONES[] = {
    {"GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"MSK-3"},
    {"UTC-4"},
    {"UTC-5:30"},
    {"UTC-7"},
    {"UTC-8"},
    {"UTC-9"},
    {"AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
    {"NZST-12NZDT,M9.5.0/2,M4.1.0/3"},
    {"UTC+3"},
    {"EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"AST4ADT,M3.2.0/2,M11.1.0/2"},
    {"ACST-9:30ACDT,M10.1.0/2,M4.1.0/3"},
    {"AKST9AKDT,M3.2.0/2,M11.1.0/2"},
};

// ---- NVS helpers ----------------------------------------------------------

// If the last NTP sync is older than this, treat a cold-boot restore as
// unsynced rather than showing a potentially very wrong time.
static constexpr int64_t STALE_THRESHOLD_S = 72 * 3600;  // 72 hours

static constexpr char NVS_NAMESPACE[] = "halclock";
static constexpr char NVS_KEY[] = "epoch";
static constexpr char NVS_SYNC_KEY[] = "lastsync";
static constexpr char NVS_DRIFT_KEY[] = "driftcoef";
static constexpr char NVS_TEMP_KEY[] = "lasttemp";
// Sleep-entry epoch, written by capture() and CONSUMED (zeroed) by restore().
// Deliberately not NVS_KEY: that one is the clock's cold-boot fallback and must
// survive, whereas this must not. Consuming it is what makes the reported
// duration a sleep length rather than "time since the last sleep began" — the
// device can reboot many times between sleeps (every reflash does), and each of
// those boots would otherwise report the same ever-staler figure.
static constexpr char NVS_SLEEP_KEY[] = "sleepmark";

static void nvsWrite(time_t epoch) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putLong64(NVS_KEY, (int64_t)epoch);
    prefs.end();
  }
}

static void nvsWriteDriftScale(float driftScale) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putFloat(NVS_DRIFT_KEY, driftScale);
    prefs.end();
  }
}

static float nvsReadDriftScale() {
  Preferences prefs;
  float result = 1.0f;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    result = prefs.getFloat(NVS_DRIFT_KEY, 1.0f);
    prefs.end();
  }
  // Guard against NaN, Inf, or out-of-range values from corrupted NVS.
  if (!std::isfinite(result) || result < 0.1f || result > 5.0f) {
    result = 1.0f;
  }
  return result;
}

static void nvsWriteLastSyncTemp(float tempC) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putFloat(NVS_TEMP_KEY, tempC);
    prefs.end();
  }
}

static float nvsReadLastSyncTemp() {
  Preferences prefs;
  float result = 0.0f;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    result = prefs.getFloat(NVS_TEMP_KEY, 0.0f);
    prefs.end();
  }
  return result;
}

static void nvsWriteSyncTime(time_t syncEpoch) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putLong64(NVS_SYNC_KEY, (int64_t)syncEpoch);
    prefs.end();
  }
}

static void nvsWriteSleepMark(time_t epoch) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putLong64(NVS_SLEEP_KEY, (int64_t)epoch);
    prefs.end();
  }
}

// Read the sleep marker and clear it in the same breath, so exactly one boot can
// ever claim a given sleep. Returns 0 when no sleep is pending.
static time_t nvsTakeSleepMark() {
  Preferences prefs;
  time_t epoch = 0;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    epoch = (time_t)prefs.getLong64(NVS_SLEEP_KEY, 0);
    if (epoch != 0) {
      prefs.putLong64(NVS_SLEEP_KEY, 0);  // one write per sleep cycle, not per boot
    }
    prefs.end();
  }
  return epoch;
}

static time_t nvsRead() {
  Preferences prefs;
  time_t epoch = 0;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    epoch = (time_t)prefs.getLong64(NVS_KEY, 0);
    prefs.end();
  }
  return epoch;
}

static time_t nvsReadSyncTime() {
  Preferences prefs;
  time_t syncEpoch = 0;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    syncEpoch = (time_t)prefs.getLong64(NVS_SYNC_KEY, 0);
    prefs.end();
  }
  return syncEpoch;
}

// ---- internal helpers -----------------------------------------------------

static bool initExternalRTC();
static float readExternalTemp();

// The SDK RTC driver, for the PCF8563 family only. Function-local so it is
// constructed on first use rather than during static init, where the I2C bus is
// not up yet. Only reached when rtcType() == Pcf8563.
static freeink::Rtc& externalRtc() {
  static freeink::Rtc rtc;
  return rtc;
}

static float readChipTemperatureC() {
  // ESP32 and ESP32-C3 use the internal ADC temperature sensor.
  //
  // Specifically Ds3231, not merely "an external RTC is present": the DS3231
  // carries a temperature register at 0x11 and the PCF8563 family carries no
  // temperature sensor at all, so widening this with the RTC gate would read a
  // nonexistent register and report noise as the device temperature.
  if (HalCapabilities::rtcType() == BoardConfig::RtcType::Ds3231 && initExternalRTC()) {
    return readExternalTemp();
  }
  return (float)temperatureRead();
}

// ---- New internal helpers for DS3231 ---------------------------------------

static bool initExternalRTC() {
  static bool initialized = false;
  static bool exists = false;
  if (initialized) return exists;
  initialized = true;

  // Two families, two drivers. The DS3231 is probed and driven inline below.
  // The PCF8563 family (T5S3's PCF8563TS, X4 Pro's BM8563 -- both at 0x51) is
  // register-incompatible with it and would return garbage if driven with DS3231
  // offsets, so it goes through the SDK's Rtc lib, which already implements it.
  // Dispatching on rtcType rather than hasHardwareRtc() is what keeps those apart.
  if (HalCapabilities::rtcType() == BoardConfig::RtcType::Pcf8563) {
    HalI2cBus::Lock i2cLock;
    exists = externalRtc().begin();
    LOG_INF("CLK", exists ? "PCF8563 RTC found via SDK driver." : "No PCF8563 RTC found.");
    return exists;
  }
  if (HalCapabilities::rtcType() != BoardConfig::RtcType::Ds3231) {
    LOG_DBG("CLK", "Skipping external RTC init: board profile declares none");
    return false;
  }

  // Wire has already been initialized; no need to call Wire.begin(I2C_SDA, I2C_SCL);
  // Shares the bus with the touch controller on touch boards, which is serviced
  // from the sampler task — see HalI2cBus. No-op on non-touch boards.
  HalI2cBus::Lock i2cLock;
  Wire.beginTransmission(ds3231Address());
  if (Wire.endTransmission() == 0) {
    exists = true;
    LOG_INF("CLK", "DS3231 Hardware via I2C found.");
  } else {
    LOG_INF("CLK", "No DS3231 found.");
  }
  return exists;
}

// Write full date+time to the external RTC (DS3231: all 7 registers inline;
// PCF8563 family: delegated to the SDK driver). Both are operated in UTC.
static void writeExternalRTC(time_t t) {
  struct tm timeinfo;
  gmtime_r(&t, &timeinfo);  // DS3231 is usually operated in UTC

  if (HalCapabilities::rtcType() == BoardConfig::RtcType::Pcf8563) {
    freeink::Rtc::DateTime dt;
    dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
    dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
    dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
    dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
    dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
    dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
    dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);  // both are 0 = Sunday
    HalI2cBus::Lock i2cLock;
    externalRtc().set(dt);
    return;
  }

  HalI2cBus::Lock i2cLock;
  Wire.beginTransmission(ds3231Address());
  Wire.write(0x00);                             // Start at register 0x00 (seconds)
  Wire.write(bin2bcd(timeinfo.tm_sec));         // 0x00: Seconds
  Wire.write(bin2bcd(timeinfo.tm_min));         // 0x01: Minutes
  Wire.write(bin2bcd(timeinfo.tm_hour));        // 0x02: Hours (24h mode)
  Wire.write(bin2bcd(timeinfo.tm_wday + 1));    // 0x03: Day of week (1-7)
  Wire.write(bin2bcd(timeinfo.tm_mday));        // 0x04: Date (01-31)
  Wire.write(bin2bcd(timeinfo.tm_mon + 1));     // 0x05: Month (01-12, century bit 0 = 20xx)
  Wire.write(bin2bcd(timeinfo.tm_year - 100));  // 0x06: Year (00-99 = 2000-2099)
  Wire.endTransmission();
}

// Read time from the external RTC. Returns 0 when the time cannot be trusted --
// the callers treat 0 as "no usable RTC time", so a stopped oscillator must not
// surface as a valid timestamp. The SDK driver already reports that case by
// returning false on the PCF8563's VL flag.
static time_t readExternalRTC() {
  if (HalCapabilities::rtcType() == BoardConfig::RtcType::Pcf8563) {
    freeink::Rtc::DateTime dt;
    HalI2cBus::Lock i2cLock;
    if (!externalRtc().now(dt)) return 0;

    struct tm timeinfo = {};
    timeinfo.tm_year = static_cast<int>(dt.year) - 1900;
    timeinfo.tm_mon = static_cast<int>(dt.month) - 1;
    timeinfo.tm_mday = dt.day;
    timeinfo.tm_hour = dt.hour;
    timeinfo.tm_min = dt.minute;
    timeinfo.tm_sec = dt.second;
    timeinfo.tm_isdst = 0;
    return timegm_compat(&timeinfo);
  }

  HalI2cBus::Lock i2cLock;
  Wire.beginTransmission(ds3231Address());
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return 0;

  Wire.requestFrom(ds3231Address(), (uint8_t)7);
  if (Wire.available() < 7) return 0;

  struct tm timeinfo = {};
  timeinfo.tm_sec = bcd2bin(Wire.read() & 0x7F);
  timeinfo.tm_min = bcd2bin(Wire.read());
  timeinfo.tm_hour = bcd2bin(Wire.read() & 0x3F);
  Wire.read();  // Wochentag überspringen
  timeinfo.tm_mday = bcd2bin(Wire.read());
  timeinfo.tm_mon = bcd2bin(Wire.read()) - 1;
  timeinfo.tm_year = bcd2bin(Wire.read()) + 100;
  timeinfo.tm_isdst = 0;

  return timegm_compat(&timeinfo);
}

// Read temperature (Register 0x11)
static float readExternalTemp() {
  Wire.beginTransmission(ds3231Address());
  Wire.write(0x11);
  if (Wire.endTransmission() != 0) {
    return 0.0f;
  }

  int count = Wire.requestFrom(ds3231Address(), (uint8_t)2);
  if (count < 2) {
    return 0.0f;
  }

  int8_t msb = Wire.read();
  uint8_t lsb = Wire.read();
  return (float)msb + (lsb >> 6) * 0.25f;
}

// ----

static void setSystemClock(time_t epoch) {
  struct timeval tv = {};
  tv.tv_sec = epoch;
  settimeofday(&tv, nullptr);
}

static uint32_t fnv1a32Append(uint32_t hash, const void* data, size_t len) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

static uint32_t computeRtcStateChecksum() {
  uint32_t hash = 2166136261u;
  hash = fnv1a32Append(hash, &rtcClockFlags, sizeof(rtcClockFlags));
  hash = fnv1a32Append(hash, &rtcEpoch, sizeof(rtcEpoch));
  hash = fnv1a32Append(hash, &rtcLpTimeUs, sizeof(rtcLpTimeUs));
  hash = fnv1a32Append(hash, &rtcSlowCal, sizeof(rtcSlowCal));
  hash = fnv1a32Append(hash, &rtcTemperatureC, sizeof(rtcTemperatureC));
  return hash;
}

static bool rtcStateLooksSane() {
  // Broad sanity window: reject obviously invalid RTC values only.
  static constexpr time_t MIN_EPOCH = 1577836800;  // 2020-01-01 UTC
  static constexpr time_t MAX_EPOCH = 7258118400;  // 2200-01-01 UTC

  if (rtcEpoch < MIN_EPOCH || rtcEpoch > MAX_EPOCH) {
    return false;
  }
  if (rtcLpTimeUs == 0 || rtcSlowCal == 0) {
    return false;
  }
  if (!std::isfinite(rtcTemperatureC) || rtcTemperatureC < -80.0f || rtcTemperatureC > 150.0f) {
    return false;
  }
  return true;
}

static bool rtcValid() {
  if (rtcClockMagic != CLOCK_RTC_MAGIC) {
    return false;
  }

  // Backward compatibility: older firmware snapshots had no checksum.
  if (rtcStateChecksum == 0) {
    return rtcStateLooksSane();
  }

  if (rtcStateChecksum != computeRtcStateChecksum()) {
    return false;
  }

  return rtcStateLooksSane();
}

/// Compute temperature-corrected elapsed seconds from LP timer delta.
/// Uses the trapezoidal rule (average of start + end temperature) as a
/// first-order approximation of the temperature integral over the interval.
/// Returns the corrected elapsed seconds and updates lpNowOut/calNowOut
/// for the caller to re-baseline.
static double computeCorrectedElapsedSec(uint64_t lpNow, float tempNow) {
  uint32_t calNow = esp_clk_slowclk_cal_get();
  uint64_t elapsedUs;
  if (rtcSlowCal != 0 && calNow != 0) {
    // rtcLpTimeUs was computed with rtcSlowCal; convert it to the
    // current calibration basis so the subtraction is consistent.
    uint64_t lpThenCorrected = (uint64_t)((double)rtcLpTimeUs * calNow / rtcSlowCal);
    elapsedUs = lpNow - lpThenCorrected;
  } else {
    elapsedUs = lpNow - rtcLpTimeUs;
  }

  // Use the full temperature delta between the average over the interval
  // and the calibration reference (which is the capture-time temperature).
  // avgTemp approximates the mean temperature during the interval.
  // The drift model says the RTC runs (1 + deltaT * driftRate) times
  // faster/slower than nominal, so the true elapsed wall-clock time
  // differs from the raw LP-derived time by that factor.
  float avgTemp = (rtcTemperatureC + tempNow) * 0.5f;
  // Positive when COOLED DOWN relative to capture temperature.
  // ESP32 RC oscillator has a positive temperature coefficient: it runs faster
  // when hotter, causing the LP timer to over-count. To recover true elapsed
  // time we must REDUCE the raw LP-derived seconds when the device is warmer
  // than at capture (and INCREASE them when cooler). Hence the sign inversion.
  float tempDelta = rtcTemperatureC - avgTemp;  // = (rtcTemperatureC - tempNow) / 2
  float tempFactor = 1.0f + tempDelta * CLOCK_TEMP_DRIFT_SECONDS_PER_SECOND_PER_DEG * rtcDriftScale;
  if (tempFactor < 0.5f) {
    tempFactor = 0.5f;
  } else if (tempFactor > 1.5f) {
    tempFactor = 1.5f;
  }

  double elapsedSec = (double)elapsedUs / 1000000.0;
  double correctedSec = elapsedSec * (double)tempFactor;

  LOG_DBG("CLK", "Drift calc: startT=%.1fC nowT=%.1fC dT=%.3f factor=%.6f raw=%.3fs corr=%.3fs", rtcTemperatureC,
          tempNow, tempDelta, tempFactor, elapsedSec, correctedSec);

  return correctedSec;
}

/// Capture current time + LP timer into RTC memory, and epoch into NVS.
static void capture(bool lpValid) {
  rtcEpoch = time(nullptr);
  // Update DS3231 only when the current time is authoritative.
  if (initExternalRTC() && !clockApproximate) {
    writeExternalRTC(rtcEpoch);
  }
  rtcLpTimeUs = esp_clk_rtc_time();
  rtcSlowCal = esp_clk_slowclk_cal_get();
  rtcTemperatureC = readChipTemperatureC();
  rtcClockMagic = CLOCK_RTC_MAGIC;
  rtcClockFlags = lpValid ? CLOCK_RTC_FLAG_LP_VALID : 0;
  rtcStateChecksum = computeRtcStateChecksum();
  nvsWrite(rtcEpoch);
  nvsWriteSleepMark(rtcEpoch);
}

// ---- public API -----------------------------------------------------------

namespace HalClock {

void applyTimezone(uint8_t timeZoneSetting) {
  const size_t index = timeZoneSetting < (sizeof(TIMEZONES) / sizeof(TIMEZONES[0])) ? timeZoneSetting : 0;
  setenv("TZ", TIMEZONES[index].tz, 1);
  tzset();
  LOG_DBG("CLK", "Timezone applied: %s", TIMEZONES[index].tz);
}

static const char* sntpStatusName(sntp_sync_status_t status) {
  switch (status) {
    case SNTP_SYNC_STATUS_RESET:
      return "reset";
    case SNTP_SYNC_STATUS_IN_PROGRESS:
      return "in progress";
    case SNTP_SYNC_STATUS_COMPLETED:
      return "completed";
    default:
      return "unknown";
  }
}

bool syncNtp(char* errorBuf, size_t errorBufSize, const char* preferredServer) {
  if (errorBuf && errorBufSize > 0) {
    errorBuf[0] = '\0';
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (errorBuf && errorBufSize > 0) {
      snprintf(errorBuf, errorBufSize, "WiFi disconnected");
    }
    LOG_ERR("CLK", "NTP sync failed: WiFi disconnected");
    return false;
  }

  const unsigned long syncStartMs = millis();
  LOG_DBG("CLK", "NTP sync start: dns=%s gw=%s rssi=%ld ip=%s", WiFi.dnsIP().toString().c_str(),
          WiFi.gatewayIP().toString().c_str(), static_cast<long>(WiFi.RSSI()), WiFi.localIP().toString().c_str());

  time_t preSyncTime = time(nullptr);
  time_t prevSyncTime = nvsReadSyncTime();
  float prevSyncTemp = nvsReadLastSyncTemp();

  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  // Register servers in priority order. A user-configured server (host or IP),
  // when set, is polled first. The hardcoded anycast IP (Cloudflare
  // time.cloudflare.com) needs no DNS lookup, so it always sits ahead of the
  // pool hostname -- this avoids a full timeout when the local resolver is slow
  // to answer right after DHCP. SNTP polls all registered servers, so a working
  // answer from any of them completes the sync.
  uint8_t idx = 0;
  const bool havePreferred = (preferredServer != nullptr) && (preferredServer[0] != '\0');
  if (havePreferred) {
    esp_sntp_setservername(idx++, preferredServer);
    LOG_DBG("CLK", "NTP preferred server: %s", preferredServer);
  }
  esp_sntp_setservername(idx++, "162.159.200.1");
  esp_sntp_setservername(idx++, "pool.ntp.org");
  esp_sntp_init();

  int retry = 0;
  constexpr int maxRetries = 50;  // 5 seconds
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < maxRetries) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    retry++;
  }

  if (retry >= maxRetries) {
    const char* statusName = sntpStatusName(sntp_get_sync_status());
    if (errorBuf && errorBufSize > 0) {
      snprintf(errorBuf, errorBufSize, "NTP timeout (%s)", statusName);
    }
    LOG_ERR("CLK", "NTP sync timeout after %lu ms (%s, dns=%s)", millis() - syncStartMs, statusName,
            WiFi.dnsIP().toString().c_str());
    // Stop SNTP on the failure path so a later applyClientTime() (which
    // refuses while SNTP is enabled) isn't blocked by a dangling instance.
    esp_sntp_stop();
    return false;
  }

  // NTP sync yields authoritative time; allow DS3231 to be updated.
  clockApproximate = false;
  capture(false);
  nvsWriteSyncTime(rtcEpoch);

  float currentTemp = rtcTemperatureC;
  if (currentTemp != 0.0f) {
    nvsWriteLastSyncTemp(currentTemp);
  }

  if (prevSyncTime > 0 && preSyncTime > 0 && rtcEpoch > prevSyncTime) {
    float interval = (float)(rtcEpoch - prevSyncTime);
    // error = how far the local clock was off before NTP corrected it.
    // Negative means local clock was behind (NTP jumped us forward).
    // Positive means local clock was ahead (NTP pulled us back).
    float error = (float)(preSyncTime - rtcEpoch);
    if (interval >= 60.0f) {
      // Convert to seconds-of-drift per day.
      float observedDriftPerDay = error * 86400.0f / interval;

      // Adaptive model calibration:
      // - Observed drift is derived from the difference between local clock
      //   reading just before NTP and the true time reported by NTP, scaled
      //   to a per-day rate over the interval since the previous sync.
      // - The baseline model expects 120 sec/day per °C.
      // - Measure temp delta since last sync (from stored NVS temp).
      // - If large enough, compute an empirical scale to apply to the model
      //   so future drift corrections are better aligned with actual hardware.
      // - The scale is persisted to NVS via saveBeforeSleep().
      float effectiveScale = rtcDriftScale;
      float tempDelta = currentTemp - prevSyncTemp;
      if (std::fabs(tempDelta) > 0.1f) {
        float modelDriftPerDay = 120.0f * tempDelta;
        if (std::fabs(modelDriftPerDay) > 0.01f) {
          float measuredScale = observedDriftPerDay / modelDriftPerDay;
          effectiveScale = 0.9f * rtcDriftScale + 0.1f * measuredScale;
          effectiveScale = std::max(0.1f, std::min(5.0f, effectiveScale));
          rtcDriftScale = effectiveScale;
        }
      }

      LOG_DBG("CLK", "NTP drift: interval=%.0fs error=%.3fs perDay=%.3f scale=%.3f deltaT=%.2f", interval, error,
              observedDriftPerDay, rtcDriftScale, tempDelta);
    }
  }

  clockApproximate = false;
  LOG_INF("CLK", "NTP synced, epoch %lld", (long long)rtcEpoch);
  return true;
}

bool syncNtp(const char* preferredServer) { return syncNtp(nullptr, 0, preferredServer); }

void saveBeforeSleep(bool keepLpAlive) {
  if (!isSynced()) {
    return;
  }
  capture(keepLpAlive);
  // Persist learned drift scale and last temperature to NVS so they survive
  // cold boot. We only write here (not periodically) to minimise flash wear.
  nvsWriteDriftScale(rtcDriftScale);
  nvsWriteLastSyncTemp(rtcTemperatureC);
  LOG_DBG("CLK", "Saved epoch %lld before sleep (driftScale=%.3f)", (long long)rtcEpoch, rtcDriftScale);
}

void restore() {
  // Claim any pending sleep marker up front, exactly once per boot. Only the
  // branches below whose wall clock came from a source that ran THROUGH the
  // sleep can turn it into a duration; the rest just consume it so a later boot
  // cannot report a stale one.
  const time_t sleepEntryEpoch = nvsTakeSleepMark();

  // PRIORITY 1: DS3231 (Hardware-RTC)
  if (initExternalRTC()) {
    time_t rtcTime = readExternalRTC();
    if (rtcTime > 1577836800) {  // Check if time is after 2020 (plausible timestamp)
      // The DS3231 ran through the sleep, so the difference against the marker
      // capture() persisted on the way in is the sleep duration. The marker is
      // zero on any boot that did not follow a sleep (a reflash, a reset), which
      // is what keeps this from reporting time-since-last-sleep instead.
      const double slept = static_cast<double>(rtcTime - sleepEntryEpoch);
      if (sleepEntryEpoch > 0 && slept > 0.0 && slept < MAX_PLAUSIBLE_SLEEP_S) {
        lastSleepSec = static_cast<uint32_t>(slept);
      }
      setSystemClock(rtcTime);
      rtcEpoch = rtcTime;
      clockApproximate = false;
      LOG_INF("CLK", "Got time from hardware RTC. Last deep sleep %us", lastSleepSec);
      return;
    }
  }

  rtcDriftScale = nvsReadDriftScale();
  const bool lpValid = (rtcClockFlags & CLOCK_RTC_FLAG_LP_VALID) != 0;
  if (rtcValid() && lpValid) {
    // RTC memory survived — we woke from deep sleep.
    //
    // We restore the wall clock by computing elapsed real time from the
    // LP timer delta and applying both frequency calibration and temperature
    // drift correction.
    //
    // Steps:
    // 1) Read current LP timer and slow-clock calibration.
    // 2) Compute raw elapsed LP ticks, on the same calibration basis used
    //    when capture() was called.
    // 3) Convert elapsed ticks to seconds.
    // 4) Apply temperature drift correction based on measured RTC memory
    //    capture temperature and current chip temp.
    // 5) Set system time to rtcEpoch + corrected elapsed seconds.
    //
    // This is an approximation: we use the average of start/end measured
    // temperature as a simple integral proxy. More advanced models could
    // sample temperature continuously, but this is a good tradeoff for low
    // cost and better accuracy vs no temperature compensation.
    uint64_t lpNow = esp_clk_rtc_time();
    time_t estimated = rtcEpoch;
    if (lpNow > rtcLpTimeUs) {
      float tempNow = readChipTemperatureC();
      double correctedSec = computeCorrectedElapsedSec(lpNow, tempNow);
      // Reject obviously bad values from a corrupted RTC snapshot.
      // 157680000 s = 5 years.
      if (std::isfinite(correctedSec) && correctedSec >= 0.0 && correctedSec <= 157680000.0) {
        estimated += static_cast<time_t>(correctedSec);
        // The LP timer ran through the sleep, so this correction IS its duration.
        if (correctedSec < MAX_PLAUSIBLE_SLEEP_S) {
          lastSleepSec = static_cast<uint32_t>(correctedSec);
        }
      } else {
        LOG_ERR("CLK", "Discarding implausible LP elapsed time: %.3fs", correctedSec);
      }
    } else if (lpNow < rtcLpTimeUs) {
      LOG_ERR("CLK", "LP timer regressed (now=%llu < saved=%llu), using baseline epoch",
              static_cast<unsigned long long>(lpNow), static_cast<unsigned long long>(rtcLpTimeUs));
    }

    setSystemClock(estimated);
    // Re-baseline LP timer and temperature for next interval.
    rtcEpoch = estimated;
    rtcLpTimeUs = esp_clk_rtc_time();
    rtcSlowCal = esp_clk_slowclk_cal_get();
    rtcTemperatureC = readChipTemperatureC();
    rtcStateChecksum = computeRtcStateChecksum();
    clockApproximate = true;
    LOG_INF("CLK", "Restored from RTC + LP timer, epoch %lld", (long long)estimated);
    return;
  }

  // Cold boot — try NVS.  No elapsed correction possible.
  time_t epoch = nvsRead();
  if (epoch > 0) {
    time_t lastSync = nvsReadSyncTime();
    if (lastSync > 0 && (epoch - lastSync) > STALE_THRESHOLD_S) {
      LOG_ERR("CLK", "NVS epoch %lld is stale (last NTP sync %lld, %lld h ago), discarding", (long long)epoch,
              (long long)lastSync, (long long)((epoch - lastSync) / 3600));
      return;
    }
    setSystemClock(epoch);
    rtcEpoch = epoch;
    rtcLpTimeUs = esp_clk_rtc_time();
    rtcSlowCal = esp_clk_slowclk_cal_get();
    rtcTemperatureC = nvsReadLastSyncTemp();
    if (rtcTemperatureC == 0.0f) {
      rtcTemperatureC = readChipTemperatureC();
    }
    rtcClockMagic = CLOCK_RTC_MAGIC;
    rtcClockFlags = 0;
    rtcStateChecksum = computeRtcStateChecksum();
    clockApproximate = true;
    LOG_INF("CLK", "Restored from NVS, epoch %lld (no elapsed correction)", (long long)epoch);
  }
}

time_t now() {
  if (!isSynced()) {
    return 0;
  }
  return time(nullptr);
}

void updatePeriodic() {
  // DS3231 (if present) has priority, synchronize the system time
  // every 10 minutes directly against the RTC, instead of calculating.
  if (initExternalRTC()) {
    unsigned long nowMs = millis();
    if (nowMs - lastPeriodicUpdateMs >= PERIODIC_UPDATE_INTERVAL_MS) {
      time_t rtcTime = readExternalRTC();
      if (rtcTime > 1577836800) {  // Check if time is after 2020 (plausible timestamp)
        lastPeriodicUpdateMs = nowMs;
        setSystemClock(rtcTime);
        LOG_DBG("CLK", "Systemtime has been taken from the hardware RTC");
      }
    }
    return;
  }

  if (!isSynced()) {
    return;
  }
  unsigned long nowMs = millis();
  if (nowMs - lastPeriodicUpdateMs < PERIODIC_UPDATE_INTERVAL_MS) {
    return;
  }
  lastPeriodicUpdateMs = nowMs;

  // Compute temperature-corrected elapsed time since last baseline and apply
  // only the drift delta (correction - raw) to the system clock. The kernel
  // clock already advanced by the raw amount, so we must not re-add it.
  uint64_t lpNow = esp_clk_rtc_time();
  if (lpNow <= rtcLpTimeUs) {
    return;
  }

  float tempNow = readChipTemperatureC();
  double correctedSec = computeCorrectedElapsedSec(lpNow, tempNow);

  // Raw elapsed seconds (what the kernel clock already counted).
  uint64_t rawElapsedUs = lpNow - rtcLpTimeUs;
  double rawSec = (double)rawElapsedUs / 1000000.0;

  // The drift delta is the difference between what really elapsed
  // (temperature-corrected) and what the kernel counted (raw).
  double driftDeltaSec = correctedSec - rawSec;

  // Re-baseline LP timer and temperature for the next interval.
  rtcLpTimeUs = lpNow;
  rtcSlowCal = esp_clk_slowclk_cal_get();
  rtcTemperatureC = tempNow;

  // Only nudge the system clock if the drift delta is meaningful (>50 ms).
  // This avoids unnecessary settimeofday calls for negligible corrections.
  if (std::fabs(driftDeltaSec) > 0.05) {
    rtcEpoch = time(nullptr) + (time_t)driftDeltaSec;
    setSystemClock(rtcEpoch);
    LOG_DBG("CLK", "Periodic drift nudge: raw=%.3fs corr=%.3fs delta=%.3fs scale=%.3f", rawSec, correctedSec,
            driftDeltaSec, rtcDriftScale);
  }
}

bool isSynced() {
  return time(nullptr) > 1577836800;  // > 2020-01-01
}

bool isApproximate() { return clockApproximate; }

time_t lastSyncTime() { return nvsReadSyncTime(); }

uint32_t lastSleepSeconds() { return lastSleepSec; }

void formatTime(char* buf, size_t bufSize, bool use24h) {
  if (!isSynced()) {
    snprintf(buf, bufSize, "--:--");
    return;
  }

  time_t t = time(nullptr);
  struct tm timeinfo;
  localtime_r(&t, &timeinfo);

  const char* prefix = isApproximate() ? "~" : "";

  if (use24h) {
    snprintf(buf, bufSize, "%s%02d:%02d", prefix, timeinfo.tm_hour, timeinfo.tm_min);
  } else {
    int hour = timeinfo.tm_hour % 12;
    if (hour == 0) hour = 12;
    const char* ampm = timeinfo.tm_hour < 12 ? "AM" : "PM";
    snprintf(buf, bufSize, "%s%d:%02d%s", prefix, hour, timeinfo.tm_min, ampm);
  }
}

void formatLogTime(char* buf, size_t bufSize) {
  if (!isSynced()) {
    buf[0] = '\0';
    return;
  }

  time_t t = time(nullptr);
  struct tm timeinfo;
  localtime_r(&t, &timeinfo);
  snprintf(buf, bufSize, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

void wifiOff(bool skipNtpSync) {
  if (!skipNtpSync && isApproximate() && WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED) {
    syncNtp();
  }
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

bool applyClientTime(time_t timestamp) {
  // Validate bounds: [2020-01-01, 2100-01-01]
  static constexpr time_t MIN_TIMESTAMP = 1577836800;  // 2020-01-01 UTC
  static constexpr time_t MAX_TIMESTAMP = 4102444800;  // 2100-01-01 UTC

  if (timestamp < MIN_TIMESTAMP || timestamp > MAX_TIMESTAMP) {
    LOG_ERR("CLK", "Client time %lld out of bounds [%lld, %lld]", (long long)timestamp, (long long)MIN_TIMESTAMP,
            (long long)MAX_TIMESTAMP);
    return false;
  }

  // Only apply when SNTP is not active (clock not synced from network).
  // NTP-synced time has priority and should never be overwritten by a client.
  if (esp_sntp_enabled()) {
    LOG_INF("CLK", "SNTP is active; not applying client time");
    return false;
  }

  // If we're in STA mode (connected to WiFi as a regular station), don't apply
  // client time — NTP sync is preferred and should happen soon.
  if (WiFi.getMode() == WIFI_STA) {
    LOG_INF("CLK", "In STA mode; not applying client time");
    return false;
  }

  setSystemClock(timestamp);
  clockApproximate = false;
  rtcEpoch = timestamp;
  LOG_INF("CLK", "Applied client time: %lld", (long long)timestamp);

  // Persist to RTC if available (X3 only)
  if (initExternalRTC()) {
    writeExternalRTC(timestamp);
    LOG_DBG("CLK", "Persisted client time to the hardware RTC");
  }

  // Also persist to NVS
  nvsWrite(timestamp);
  nvsWriteSyncTime(timestamp);

  return true;
}

}  // namespace HalClock
