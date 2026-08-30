#pragma once

#include <HalGPIO.h>

#include <cstdint>

/// Boot and sleep forensics.
///
/// Two things this firmware could not previously answer from the device itself:
///
///   1. Where did the last sleep stop?  A device that hangs before
///      esp_deep_sleep_start() and a device that slept fine but did not wake look
///      identical from the outside — the panel holds the sleep image either way and
///      no button does anything.  Issue #155 sat open for weeks on exactly that
///      ambiguity.
///   2. How did this boot start, and where did its time go?  The phase trace below
///      used to live as file-static state in main.cpp, reachable only over a serial
///      monitor (`CMD:BOOTLOG`) — which most reporters do not have, and which a
///      battery-powered device does not even open.
///
/// The answers are kept in three places, cheapest first:
///   * `.bss`      — this boot's phase stamps and wake verdict (26 bytes).
///   * RTC_NOINIT  — the in-flight sleep breadcrumb (12 bytes).  Survives a soft
///                   reset, a deep-sleep wake and a hang-plus-reset; lost when the
///                   battery latch is actually cut, which is itself the signal that
///                   the sleep completed.
///   * SD          — a fixed-size ring of the last 16 sleep/boot events, so history
///                   survives power loss and can be read back on screen.
///
/// Surfaced by BootDiagnosticsActivity (Settings > System > Boot Diagnostics).
namespace BootDiag {

// ---------------------------------------------------------------------------
// Boot phase trace
// ---------------------------------------------------------------------------

/// millis() stamp per boot phase.  The wake gesture is only a ~300 ms gate
/// (getPowerWakeHoldDuration), but the splash lands seconds later because the SD
/// mount, the config loads, the panel bring-up and the first (non-differential)
/// waveform all sit between the two.  Without per-phase stamps that gap is invisible
/// in the log — every phase before Serial.begin() has no output at all — and "the
/// power button feels unresponsive" reports cannot be attributed to a phase.
///
/// The stamps stay live for the whole session precisely so they can be re-emitted: a
/// wake from deep sleep re-enumerates USB, so a host monitor reconnecting mid-boot
/// misses the first lines, and the RTC ring buffer (16 entries) has usually rolled
/// past them by the time anyone looks.
///
/// Cost is 2 bytes per phase of .bss plus one log line; no heap, no allocation.
/// Stamps are clamped to 16 bits (a boot that reaches 65 s is pathological and reads
/// as 65535).
enum class BootPhase : uint8_t {
  SetupEntry,      // first statement of setup()
  NvsSettings,     // startup settings read from NVS (precedes the wake gate)
  WakeGate,        // power-button wake gesture decided
  HwInit,          // system / SPI bus / GPIO / power / tilt brought up
  SerialUp,        // USB-CDC opened (only when USB is connected; earlier phases log nothing)
  RecoverySettle,  // UP+POWER recovery-combo sample window
  SdMount,         // Storage.begin()
  ConfigLoad,      // settings, app state, i18n, KOReader, OPDS, weather, theme
  DisplayFonts,    // panel init + framebuffer alloc + font registration + SD font scan
  FirstPaint,      // splash / quick-resume frame handed to the panel (logo now visible)
  StoreLoad,       // clock, recent books, bookmarks, reading stats
  ActivityRoute,   // target activity entered (home / reader / recovery)
  PowerRelease,    // wake press released (stable), input sampler about to start
  Count,
};

void markPhase(BootPhase phase);
bool phaseReached(BootPhase phase);
uint16_t phaseMs(BootPhase phase);
/// Short label ("entry", "gate", "sd", ...).  Points at a string literal.
const char* phaseName(BootPhase phase);

/// Record what the boot-time wake gate decided.  Kept for the whole session so it can
/// be re-emitted alongside the phase trace and shown on the diagnostics page.
void setWakeCheck(const HalGPIO::WakeCheck& check);
const HalGPIO::WakeCheck& wakeCheck();

/// The whole boot story in two log lines: what the power button was judged to be, and
/// where the time went.  Re-emitted on every serial (re)connect.
void logSummary();

// ---------------------------------------------------------------------------
// Sleep breadcrumb
// ---------------------------------------------------------------------------

/// How far the last deep-sleep attempt got.  Advanced through RTC_NOINIT as the sleep
/// path proceeds, so the two indistinguishable failures — "never actually slept" and
/// "slept but did not wake" — separate cleanly on the next boot.
///
/// Ordered: a larger value means strictly more progress, so the page can render the
/// stage as a position along the sequence.
enum class SleepStage : uint8_t {
  None = 0,         // no sleep attempted since this power cycle began
  Requested,        // enterDeepSleep() entered and committed
  StatePersisted,   // app state + clock written
  ScreenPainted,    // sleep screen handed to the panel
  PanelAsleep,      // display.deepSleep() returned
  RailsConfigured,  // GPIO13 / rail teardown done, wake pin pulled up
  AwaitingRelease,  // blocking on the power-button release — where a stuck pin hangs
  ReleaseTimedOut,  // the release wait gave up and slept anyway
  WakeArmed,        // wake source armed; esp_deep_sleep_start() is the next statement
  Count,
};

/// Why the firmware decided to sleep.  Distinguishes a deliberate gesture from the two
/// early-boot paths that sleep before any UI exists, which is worth knowing: those are
/// invisible to the user and used to change the device's power state behind their back.
enum class SleepTrigger : uint8_t {
  Unknown = 0,
  PowerHold,         // power button held past the sleep threshold
  Timeout,           // inactivity auto-sleep
  ButtonAction,      // a button mapped to the Sleep action
  WakeGateRejected,  // woke, gate refused the press, going straight back down
  UsbPowerBoot,      // USB power caused a cold boot; nothing else was initialised
  Count,
};

const char* stageName(SleepStage stage);
const char* triggerName(SleepTrigger trigger);

/// Open a sleep attempt.  Resets the breadcrumb and records the policy this sleep runs
/// under, so a later boot can say whether the battery latch was meant to be cut.
void beginSleep(SleepTrigger trigger, bool keepClockAlive, bool fromReader);

/// Advance the breadcrumb.  Never moves backwards, so an out-of-order call cannot
/// understate the progress actually made.
void markSleepStage(SleepStage stage);

/// Record the outcome of the power-button release wait: how long it blocked, and
/// whether it gave up rather than seeing a clean release.
void noteReleaseWait(unsigned long waitedMs, bool timedOut);

// ---------------------------------------------------------------------------
// Persisted history
// ---------------------------------------------------------------------------

/// One line of the sleep/wake history.  16 bytes, POD, and read back with memcpy
/// rather than a pointer cast — the C3 faults on unaligned multi-byte loads.
struct Record {
  uint32_t seq;    // monotonic across boots and power cycles; establishes ordering
  uint8_t kind;    // Kind
  uint8_t code;    // Sleep: SleepStage reached | Boot: HalGPIO::WakeVerdict
  uint8_t reason;  // Sleep: SleepTrigger       | Boot: esp_reset_reason()
  uint8_t flags;   // Sleep: kFlag*             | Boot: esp_sleep_get_wakeup_cause()
  uint16_t msA;    // Sleep: release-wait ms    | Boot: wake gate decidedAtMs
  uint16_t msB;    // Sleep: uptime seconds     | Boot: wake gate heldMs
  uint16_t msC;    // Sleep: unused             | Boot: millis() at SD mount
  uint16_t pad;    // keeps the struct 16 bytes and 4-byte aligned
};
static_assert(sizeof(Record) == 16, "Record is written to SD verbatim; keep it 16 bytes");

enum Kind : uint8_t { KindSleep = 0, KindBoot = 1 };

/// Record::flags bits, sleep records only.
enum SleepFlags : uint8_t {
  kFlagKeepClock = 1 << 0,       // battery latch stays HIGH, MCU powered through sleep
  kFlagFromReader = 1 << 1,      // slept with a book open (wake repaints nothing until the page lands)
  kFlagReleaseTimeout = 1 << 2,  // the power-button release wait gave up
  kFlagStageFromRtc = 1 << 3,    // final stage was amended from the RTC breadcrumb on the next boot
};

/// How many events the ring keeps.  16 covers eight full sleep/wake cycles, which is
/// more than any reporter has been asked to reproduce, at 272 bytes on the card.
inline constexpr uint8_t kCapacity = 16;

/// Call once after Storage.begin().  Amends the previous sleep record from the RTC
/// breadcrumb (when it survived), appends this boot's record, and clears the
/// breadcrumb.  Cheap: one 272-byte read and one 272-byte write.
void persistBoot();

/// Call on the sleep path once everything that can fail has succeeded and only the
/// release wait and the wake arming remain.  Appends the sleep record so it survives a
/// power cut, a hang, or a battery pull.
void persistSleep();

/// Read the ring back, newest first.  `out` must have room for kCapacity records.
/// Returns how many were filled.  No heap: the caller owns the storage.
uint8_t loadRecords(Record* out, uint8_t maxRecords);

}  // namespace BootDiag
