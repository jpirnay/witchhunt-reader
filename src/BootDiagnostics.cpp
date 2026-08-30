#include "BootDiagnostics.h"

#include <Arduino.h>
#include <CrossPointSettings.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include <cstdio>
#include <cstring>

namespace BootDiag {
namespace {

// ---------------------------------------------------------------------------
// This boot's trace (.bss)
// ---------------------------------------------------------------------------

uint16_t s_phaseMs[static_cast<uint8_t>(BootPhase::Count)] = {};
// Which phases were reached — a stamp of 0 is a legal millis() value, so "reached"
// cannot be inferred from the value. 13 phases fit a uint16_t mask with room to spare.
uint16_t s_phaseReached = 0;
static_assert(static_cast<uint8_t>(BootPhase::Count) <= 16, "s_phaseReached mask is 16 bits wide");

HalGPIO::WakeCheck s_wakeCheck;

// Short labels so the whole trace fits one 256-byte ring-buffer entry (Logging.cpp).
constexpr const char* kPhaseNames[] = {"entry", "nvs",  "gate",  "hw",    "ser",   "recov", "sd",
                                       "cfg",   "disp", "paint", "store", "route", "rel"};
static_assert(sizeof(kPhaseNames) / sizeof(kPhaseNames[0]) == static_cast<size_t>(BootPhase::Count),
              "kPhaseNames must stay in sync with BootPhase");

// ---------------------------------------------------------------------------
// In-flight sleep breadcrumb (RTC_NOINIT)
// ---------------------------------------------------------------------------
//
// RTC_NOINIT rather than RTC_DATA: the point is to survive a panic, a watchdog reset
// and the user's own reset press — none of which is a clean restart — so the block must
// not be re-initialised by the startup code. It is uninitialised on a cold boot, which
// is what the magic guards against.
//
// 12 bytes of the 8 KB RTC-slow segment. The build already sits at ~68% of it, so this
// is deliberately three words and not a struct that will grow.
constexpr uint32_t kCrumbMagic = 0x57424331;  // 'WBC1'

RTC_NOINIT_ATTR uint32_t s_crumbMagic;
RTC_NOINIT_ATTR uint32_t s_crumbSleep;  // stage | trigger<<8 | flags<<16 | pending<<24
RTC_NOINIT_ATTR uint32_t s_crumbWait;   // release-wait milliseconds

// Set while a sleep has been recorded in the crumb but NOT yet written to SD — the two
// early-boot sleep paths run before Storage.begin(), so they can only leave a crumb and
// let the next boot persist it.
constexpr uint32_t kPendingShift = 24;

bool crumbValid() { return s_crumbMagic == kCrumbMagic; }

uint8_t crumbStage() { return static_cast<uint8_t>(s_crumbSleep & 0xFF); }
uint8_t crumbTrigger() { return static_cast<uint8_t>((s_crumbSleep >> 8) & 0xFF); }
uint8_t crumbFlags() { return static_cast<uint8_t>((s_crumbSleep >> 16) & 0xFF); }
bool crumbPending() { return ((s_crumbSleep >> kPendingShift) & 1u) != 0; }

void crumbStore(uint8_t stage, uint8_t trigger, uint8_t flags, bool pending) {
  s_crumbSleep = static_cast<uint32_t>(stage) | (static_cast<uint32_t>(trigger) << 8) |
                 (static_cast<uint32_t>(flags) << 16) | (static_cast<uint32_t>(pending ? 1u : 0u) << kPendingShift);
  s_crumbMagic = kCrumbMagic;
}

// ---------------------------------------------------------------------------
// Persisted ring
// ---------------------------------------------------------------------------

constexpr char kRingPath[] = "/.crosspoint/bootdiag.bin";
constexpr uint32_t kRingMagic = 0x57424431;  // 'WBD1'
constexpr uint16_t kRingVersion = 1;

// Whole-file read/modify/write. At 272 bytes that is one SD block either way, so a
// partial in-place update would buy nothing and would need seek bookkeeping the ring
// wrap makes fiddly.
struct RingHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t count;     // valid records, <= kCapacity
  uint16_t head;      // index of the OLDEST record
  uint16_t reserved;  // keeps nextSeq 4-byte aligned in the file image
  uint32_t nextSeq;
};
static_assert(sizeof(RingHeader) == 16, "RingHeader is written to SD verbatim");

constexpr size_t kFileBytes = sizeof(RingHeader) + sizeof(Record) * kCapacity;

// One 272-byte scratch image, reused by both writers. Static rather than stack: the
// sleep path runs on the loop task after the render task has been left holding its own
// buffers, and this codebase is sensitive to stack-into-heap spills (see the btnsample
// stack note in HalGPIO). Static rather than heap: it must work when the heap is too
// fragmented to serve anything, which is one of the states worth recording.
uint8_t s_image[kFileBytes];

bool readImage() {
  memset(s_image, 0, sizeof(s_image));
  HalFile file;
  if (!Storage.openFileForRead("DIAG", kRingPath, file)) {
    return false;
  }
  const int read = file.read(s_image, sizeof(s_image));
  file.close();
  if (read != static_cast<int>(sizeof(s_image))) {
    return false;
  }
  RingHeader header{};
  memcpy(&header, s_image, sizeof(header));
  if (header.magic != kRingMagic || header.version != kRingVersion || header.count > kCapacity ||
      header.head >= kCapacity) {
    return false;
  }
  return true;
}

void resetImage() {
  memset(s_image, 0, sizeof(s_image));
  const RingHeader header{kRingMagic, kRingVersion, 0, 0, 0, 1};
  memcpy(s_image, &header, sizeof(header));
}

RingHeader imageHeader() {
  RingHeader header{};
  memcpy(&header, s_image, sizeof(header));
  return header;
}

void imageSetHeader(const RingHeader& header) { memcpy(s_image, &header, sizeof(header)); }

void imageGetRecord(uint8_t slot, Record& out) {
  memcpy(&out, s_image + sizeof(RingHeader) + static_cast<size_t>(slot) * sizeof(Record), sizeof(Record));
}

void imageSetRecord(uint8_t slot, const Record& in) {
  memcpy(s_image + sizeof(RingHeader) + static_cast<size_t>(slot) * sizeof(Record), &in, sizeof(Record));
}

/// Slot holding the newest record, or kCapacity when the ring is empty.
uint8_t newestSlot(const RingHeader& header) {
  if (header.count == 0) {
    return kCapacity;
  }
  return static_cast<uint8_t>((header.head + header.count - 1) % kCapacity);
}

void imageAppend(Record& record) {
  RingHeader header = imageHeader();
  record.seq = header.nextSeq++;
  uint8_t slot;
  if (header.count < kCapacity) {
    slot = static_cast<uint8_t>((header.head + header.count) % kCapacity);
    header.count++;
  } else {
    // Full: overwrite the oldest and walk the head forward.
    slot = header.head;
    header.head = static_cast<uint16_t>((header.head + 1) % kCapacity);
  }
  imageSetRecord(slot, record);
  imageSetHeader(header);
}

bool writeImage() {
  Storage.mkdir("/.crosspoint");
  HalFile file;
  if (!Storage.openFileForWrite("DIAG", kRingPath, file)) {
    return false;
  }
  const size_t written = file.write(s_image, sizeof(s_image));
  file.close();
  return written == sizeof(s_image);
}

/// Load the ring, apply `mutate`, write it back. Rebuilds the file from scratch when it
/// is missing or unreadable, so a corrupt ring self-heals instead of silently going
/// unrecorded for the rest of the device's life.
template <typename Fn>
bool updateRing(Fn mutate) {
  if (!Storage.ready()) {
    return false;
  }
  if (!readImage()) {
    resetImage();
  }
  mutate();
  return writeImage();
}

Record makeSleepRecord() {
  Record record{};
  record.kind = KindSleep;
  record.code = crumbStage();
  record.reason = crumbTrigger();
  record.flags = crumbFlags();
  record.msA = static_cast<uint16_t>(s_crumbWait > UINT16_MAX ? UINT16_MAX : s_crumbWait);
  const unsigned long uptimeS = millis() / 1000UL;
  record.msB = static_cast<uint16_t>(uptimeS > UINT16_MAX ? UINT16_MAX : uptimeS);
  return record;
}

}  // namespace

// ---------------------------------------------------------------------------
// Boot phase trace
// ---------------------------------------------------------------------------

void markPhase(BootPhase phase) {
  const unsigned long ms = millis();
  s_phaseMs[static_cast<uint8_t>(phase)] = static_cast<uint16_t>(ms > UINT16_MAX ? UINT16_MAX : ms);
  s_phaseReached |= static_cast<uint16_t>(1u << static_cast<uint8_t>(phase));
}

bool phaseReached(BootPhase phase) { return (s_phaseReached & (1u << static_cast<uint8_t>(phase))) != 0; }

uint16_t phaseMs(BootPhase phase) { return s_phaseMs[static_cast<uint8_t>(phase)]; }

const char* phaseName(BootPhase phase) {
  const auto index = static_cast<uint8_t>(phase);
  return index < static_cast<uint8_t>(BootPhase::Count) ? kPhaseNames[index] : "?";
}

void setWakeCheck(const HalGPIO::WakeCheck& check) { s_wakeCheck = check; }

const HalGPIO::WakeCheck& wakeCheck() { return s_wakeCheck; }

void logSummary() {
  LOG_INF("BOOT", "Wake gate: %s (decided at %u ms, gate saw the press for %u ms, required %u ms)",
          HalGPIO::wakeVerdictName(s_wakeCheck.verdict), s_wakeCheck.decidedAtMs, s_wakeCheck.heldMs,
          CrossPointSettings::getPowerWakeHoldDuration());

  // One "name+costMs" token per reached phase, then the two absolute numbers worth
  // quoting. Deltas rather than absolutes because the cost of a phase is the actionable
  // figure and absolutes are just their running sum — and because the whole line has to
  // fit one 256-byte ring-buffer entry including logPrintf's timestamp/level prefix
  // (Logging.cpp, MAX_ENTRY_LEN). Unreached phases are skipped (e.g. `ser` on battery,
  // `recov` on a non-button boot), so a short trace is itself a signal about which path
  // the boot took.
  char line[160];
  size_t used = 0;
  uint16_t previous = 0;
  for (uint8_t i = 0; i < static_cast<uint8_t>(BootPhase::Count); i++) {
    if ((s_phaseReached & (1u << i)) == 0) continue;
    const int written = snprintf(line + used, sizeof(line) - used, "%s%s+%u", used ? " " : "", kPhaseNames[i],
                                 static_cast<unsigned>(s_phaseMs[i] - previous));
    if (written < 0 || static_cast<size_t>(written) >= sizeof(line) - used) break;
    used += static_cast<size_t>(written);
    previous = s_phaseMs[i];
  }
  // millis() excludes the ROM/2nd-stage bootloader (~200-300 ms), so time-to-logo as the
  // user experiences it is that much longer than `logo` reports.
  // Worst case (five-digit stamps throughout) still fits MAX_ENTRY_LEN: 29 prefix + 15
  // here + 160 line + 38 tail = 242 of 256. Keep that budget in mind when editing either.
  LOG_INF("BOOT", "phase cost ms: %s | logo=%u setup=%u (+bootloader)", line,
          s_phaseMs[static_cast<uint8_t>(BootPhase::FirstPaint)], previous);
}

// ---------------------------------------------------------------------------
// Sleep breadcrumb
// ---------------------------------------------------------------------------

const char* stageName(SleepStage stage) {
  switch (stage) {
    case SleepStage::None:
      return "none";
    case SleepStage::Requested:
      return "requested";
    case SleepStage::StatePersisted:
      return "state-saved";
    case SleepStage::ScreenPainted:
      return "screen-painted";
    case SleepStage::PanelAsleep:
      return "panel-asleep";
    case SleepStage::RailsConfigured:
      return "rails-set";
    case SleepStage::AwaitingRelease:
      return "await-release";
    case SleepStage::ReleaseTimedOut:
      return "release-timeout";
    case SleepStage::WakeArmed:
      return "wake-armed";
    case SleepStage::Count:
      break;
  }
  return "?";
}

const char* triggerName(SleepTrigger trigger) {
  switch (trigger) {
    case SleepTrigger::Unknown:
      return "unknown";
    case SleepTrigger::PowerHold:
      return "power-hold";
    case SleepTrigger::Timeout:
      return "timeout";
    case SleepTrigger::ButtonAction:
      return "button";
    case SleepTrigger::WakeGateRejected:
      return "gate-rejected";
    case SleepTrigger::UsbPowerBoot:
      return "usb-boot";
    case SleepTrigger::Count:
      break;
  }
  return "?";
}

void beginSleep(SleepTrigger trigger, bool keepClockAlive, bool fromReader) {
  uint8_t flags = 0;
  if (keepClockAlive) flags |= kFlagKeepClock;
  if (fromReader) flags |= kFlagFromReader;
  s_crumbWait = 0;
  crumbStore(static_cast<uint8_t>(SleepStage::Requested), static_cast<uint8_t>(trigger), flags, /*pending=*/true);
}

void markSleepStage(SleepStage stage) {
  if (!crumbValid()) {
    // A stage without a beginSleep() means an unexpected call order rather than a real
    // sleep; record it as an unknown trigger rather than dropping it, so the anomaly is
    // visible on the page instead of silently absent.
    crumbStore(static_cast<uint8_t>(stage), static_cast<uint8_t>(SleepTrigger::Unknown), 0, /*pending=*/true);
    return;
  }
  if (static_cast<uint8_t>(stage) <= crumbStage()) {
    return;  // never walk the breadcrumb backwards
  }
  crumbStore(static_cast<uint8_t>(stage), crumbTrigger(), crumbFlags(), crumbPending());
}

void noteReleaseWait(unsigned long waitedMs, bool timedOut) {
  s_crumbWait = static_cast<uint32_t>(waitedMs);
  if (!crumbValid()) {
    return;
  }
  if (timedOut) {
    crumbStore(static_cast<uint8_t>(SleepStage::ReleaseTimedOut), crumbTrigger(),
               static_cast<uint8_t>(crumbFlags() | kFlagReleaseTimeout), crumbPending());
  }
}

// ---------------------------------------------------------------------------
// Persisted history
// ---------------------------------------------------------------------------

void persistSleep() {
  if (!crumbValid()) {
    return;
  }
  const bool ok = updateRing([] {
    Record record = makeSleepRecord();
    imageAppend(record);
  });
  if (ok) {
    // Written — the next boot amends this record in place instead of appending a second
    // one for the same sleep.
    crumbStore(crumbStage(), crumbTrigger(), crumbFlags(), /*pending=*/false);
  }
}

void persistBoot() {
  const bool hadCrumb = crumbValid();
  const uint8_t finalStage = hadCrumb ? crumbStage() : 0;
  const bool pending = hadCrumb && crumbPending();
  const uint8_t crumbFlagBits = hadCrumb ? crumbFlags() : 0;
  const uint8_t crumbTriggerBits = hadCrumb ? crumbTrigger() : 0;
  const uint32_t waitMs = hadCrumb ? s_crumbWait : 0;

  const esp_reset_reason_t resetReason = esp_reset_reason();
  const esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();

  updateRing([&] {
    if (hadCrumb) {
      if (pending) {
        // The sleep never reached persistSleep() — one of the two early-boot paths, which
        // sleep before Storage.begin() and so could only leave a breadcrumb. Write it now.
        Record record{};
        record.kind = KindSleep;
        record.code = finalStage;
        record.reason = crumbTriggerBits;
        record.flags = static_cast<uint8_t>(crumbFlagBits | kFlagStageFromRtc);
        record.msA = static_cast<uint16_t>(waitMs > UINT16_MAX ? UINT16_MAX : waitMs);
        imageAppend(record);
      } else {
        // persistSleep() wrote the record before the release wait and the wake arming, so
        // its stage is one or two steps stale. The breadcrumb survived, which means the
        // MCU stayed powered — amend the record with what actually happened.
        const RingHeader header = imageHeader();
        const uint8_t slot = newestSlot(header);
        if (slot < kCapacity) {
          Record record{};
          imageGetRecord(slot, record);
          if (record.kind == KindSleep && record.code < finalStage) {
            record.code = finalStage;
            record.flags = static_cast<uint8_t>(record.flags | crumbFlagBits | kFlagStageFromRtc);
            record.msA = static_cast<uint16_t>(waitMs > UINT16_MAX ? UINT16_MAX : waitMs);
            imageSetRecord(slot, record);
          }
        }
      }
    }

    Record boot{};
    boot.kind = KindBoot;
    boot.code = static_cast<uint8_t>(s_wakeCheck.verdict);
    boot.reason = static_cast<uint8_t>(resetReason);
    boot.flags = static_cast<uint8_t>(wakeupCause);
    boot.msA = s_wakeCheck.decidedAtMs;
    boot.msB = s_wakeCheck.heldMs;
    boot.msC = phaseMs(BootPhase::SdMount);
    imageAppend(boot);
  });

  // Consumed either way: a crumb that could not be written is still spent, and leaving it
  // set would attribute this boot's sleep state to the next boot as well.
  s_crumbMagic = 0;
  s_crumbSleep = 0;
  s_crumbWait = 0;
}

uint8_t loadRecords(Record* out, uint8_t maxRecords) {
  if (out == nullptr || maxRecords == 0 || !Storage.ready() || !readImage()) {
    return 0;
  }
  const RingHeader header = imageHeader();
  const uint8_t wanted = header.count < maxRecords ? static_cast<uint8_t>(header.count) : maxRecords;
  for (uint8_t i = 0; i < wanted; i++) {
    // Newest first: walk back from the last written slot.
    const uint8_t slot = static_cast<uint8_t>((header.head + header.count - 1 - i + kCapacity) % kCapacity);
    imageGetRecord(slot, out[i]);
  }
  return wanted;
}

}  // namespace BootDiag
