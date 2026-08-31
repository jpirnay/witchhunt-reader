#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

/// Lightweight wall-clock facade.
///
/// The ESP32-C3 has no battery-backed RTC, so wall-clock time is lost on every
/// deep-sleep / power cycle.  HalClock bridges this gap using three layers:
///
///  - **LP timer** (`esp_clk_rtc_time()`) — keeps running during deep sleep
///    when `keepClockAlive` is enabled (GPIO13 stays HIGH).  Used to compute
///    elapsed time and correct the stored epoch on wake.
///  - **RTC memory** (`RTC_NOINIT_ATTR`) — survives deep sleep, lost on cold
///    boot.  Stores the epoch + LP timer value captured before sleep.
///  - **NVS** (flash key-value store) — survives power cycles.  Fallback when
///    RTC memory is unavailable (cold boot).
///
/// Usage:
///  1. On boot, call `restore()` to seed the system clock from the best
///     available source (RTC memory + LP correction > NVS).
///  2. After a successful NTP sync, call `syncNtp()`.
///  3. Before entering deep sleep, call `saveBeforeSleep()`.
///
/// `now()` returns the best-effort epoch (0 if never synced).
namespace HalClock {

/// Perform an NTP sync (requires WiFi to be connected).  Starts SNTP,
/// waits up to 5 seconds for completion, then captures the result.
/// Returns true if the sync succeeded.  `preferredServer` behaves as in the
/// error-reporting overload below.
bool syncNtp(const char* preferredServer = nullptr);

/// Same as syncNtp(), but fills `errorBuf` with a short failure reason when
/// the sync fails.  `preferredServer`, when non-null and non-empty, is polled
/// as the primary NTP server (a user-configured host or IP); a hardcoded
/// anycast IP and pool.ntp.org are always registered as fallbacks. Passing
/// nullptr uses only the built-in servers. The HAL takes the address as an
/// argument rather than reading app settings, mirroring applyTimezone().
bool syncNtp(char* errorBuf, size_t errorBufSize, const char* preferredServer = nullptr);

/// Apply timezone/DST rules via the POSIX TZ string for the given setting.
void applyTimezone(uint8_t timeZoneSetting);

/// Call just before deep sleep.  Snapshots the current system time to RTC
/// memory and NVS so it can be restored on wake / cold boot.  Pass true when
/// the LP timer is kept alive during sleep.
void saveBeforeSleep(bool keepLpAlive);

/// Call on boot to seed the system clock from the best available stored
/// value.  When RTC memory is valid (deep-sleep wake) and the LP timer was
/// running, the restored time includes elapsed-time correction.  Falls back
/// to NVS for cold boot (stale, but better than nothing).
void restore();

/// Returns the current best-effort wall-clock epoch, or 0 if the clock was
/// never set.
time_t now();

/// True if the clock has been set at least once (NTP or restore).
bool isSynced();

/// Periodic callback (called from main loop) to compensate temperature-induced
/// RTC drift while the device is awake.  Runs at a 10-minute interval.
/// Computes the drift delta since the last baseline using the temperature
/// model and nudges the system clock by only that delta (the kernel clock
/// already advanced the raw amount).  Drift state is persisted to NVS only
/// in saveBeforeSleep() to minimise flash wear.
void updatePeriodic();

/// True if the last restore was from a backup (not NTP) — i.e. the clock
/// may have drifted.  Cleared on NTP sync.
bool isApproximate();

/// True when the clock was seeded from an NVS epoch older than the staleness threshold.
///
/// The wall clock IS set in that case, deliberately: a last-known-good time is a sound LOWER
/// BOUND, and the machine uses on these RTC-less boards need one. 1970 is not a neutral fallback
/// — it makes every curated TLS root's notBefore look unreached, so the trust store fails to load
/// and https stops working entirely (see HalClock::isPlausibleForTls). Cache TTLs and session
/// timestamps degrade the same way.
///
/// What a stale epoch is NOT is a time of day, so formatTime() shows "--:--" while this is set.
/// Cleared by a successful NTP sync.
bool isStaleRestore();

/// True when the system clock sits inside the window where TLS certificate
/// date validation can succeed.
///
/// wolfSSL rejects a CA whose notBefore lies in the *future* of the device
/// clock (ASN_BEFORE_DATE_E) and one that has expired (ASN_AFTER_DATE_E), and
/// it does so when the trust store is LOADED, not just during the handshake —
/// wolfSSL_CTX_load_verify_buffer() runs with WOLFSSL_LOAD_FLAG_NONE, so
/// WOLFSSL_LOAD_FLAG_DATE_ERR_OKAY is not in play. On these RTC-less C3 boards
/// a cold boot leaves the clock at 1970 (restore() deliberately discards a
/// stale NVS epoch rather than trusting it), which puts every curated root's
/// notBefore in the future and makes the whole trust store fail to load.
///
/// "Approximate" is fine here: an NVS-restored clock that is merely a few hours
/// off still lands inside every root's validity window, so it verifies fine.
/// Only genuinely unset or wildly-wrong clocks matter.
bool isPlausibleForTls();

/// Make the clock usable for TLS: a no-op returning true when it already is,
/// otherwise one SNTP attempt against `preferredServer`. Returns whether the
/// clock ended up plausible.
///
/// CALL THIS AFTER WIFI IS UP AND BEFORE THE FIRST TLS CONNECT of any network
/// flow. Skipping it does not produce a verification warning — it produces a
/// trust store that will not load at all, which surfaces as a bare "connect
/// failed" with no verification error to classify (and therefore no
/// verified->insecure fallback either).
///
/// Failed attempts are rate-limited rather than latched, so a call made before
/// the radio was ready does not poison the rest of the session; repeated calls
/// on a healthy clock cost one time() comparison.
bool ensureUsableForTls(const char* preferredServer = nullptr);

/// Returns the epoch of the last successful NTP sync (from NVS), or 0 if
/// no sync has ever been recorded.
time_t lastSyncTime();

/// How long the deep sleep this boot woke from lasted, in seconds, or 0 when it
/// cannot be established (cold boot, or a clock that was never synced so no
/// sleep-entry epoch was captured). Only restore() paths whose wall clock comes
/// from a source that ran through the sleep — the DS3231 or the LP timer — can
/// produce a real figure; see the definition in HalClock.cpp.
uint32_t lastSleepSeconds();

/// Format the current time for display.  Returns "--:--" if the clock was
/// never synced, prefixes with "~" if approximate.
/// When use24h is false, formats as "2:05pm" / "12:30am".
/// Output is written to `buf` (must be at least 16 bytes).
void formatTime(char* buf, size_t bufSize, bool use24h);

/// Format the current time for log timestamps.  Returns "HH:MM:SS" if
/// synced, or an empty string if not.
void formatLogTime(char* buf, size_t bufSize);

/// Tear down WiFi cleanly.  When skipNtpSync is false (default) and the
/// clock is approximate, performs an opportunistic NTP sync before
/// disconnecting — essentially free since we already have a connection.
void wifiOff(bool skipNtpSync = false);

/// Apply a Unix timestamp (e.g., from a hotspot client upload).
/// Only updates system clock if SNTP is not active (clock not synced from network).
/// Bounds check: [2020-01-01, 2100-01-01]. Returns true on success.
bool applyClientTime(time_t timestamp);

}  // namespace HalClock
