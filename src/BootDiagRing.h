#pragma once

#include <climits>
#include <cstdint>
#include <cstring>

/// The pure half of the boot/sleep diagnostics: the on-card record format, the ring layout
/// that holds it, and the rules for reading a record back.
///
/// Split out from BootDiagnostics so it can be exercised on the host. Every defect this
/// feature has shipped so far has been in exactly this logic and every one of them was
/// found by flashing a device: a stage read as a verdict before it was final, a reset
/// reason trusted to mean something the C3 cannot express, and a newest-first scan that
/// broke on its own record. None of that needs hardware to catch — it needs a test — and
/// none of it needs Arduino, the HAL, NVS or an SD card to express, so this header depends
/// on nothing but <cstdint> and <cstring>.
///
/// Everything here operates on a caller-supplied byte image; file I/O, RTC memory and NVS
/// stay in BootDiagnostics.cpp.
namespace BootDiag {

/// One line of the sleep/wake history. 16 bytes, POD, and read back with memcpy rather
/// than a pointer cast — the C3 faults on unaligned multi-byte loads.
struct Record {
  uint32_t seq;    // monotonic across boots and power cycles; establishes ordering
  uint8_t kind;    // Kind
  uint8_t code;    // Sleep: SleepStage | Boot: WakeVerdict | ResumeStall: WakeTrace::Phase
  uint8_t reason;  // Sleep: SleepTrigger | Boot: reset reason | ResumeStall: storage operation
  uint8_t flags;   // Sleep/ResumeStall: kFlag* | Boot: esp_sleep_get_wakeup_cause()
  uint16_t msA;    // Sleep: release-wait ms | Boot: gate decidedAtMs | Stall/Aborted: seconds/count
  uint16_t msB;    // Sleep: uptime seconds | Boot: gate heldMs | ResumeStall: storage-op age seconds
  uint16_t msC;    // Sleep: unused | Boot: SD mount ms | Stall: SPI-operation age seconds
  uint16_t pad;    // Stall: SPI operation (low byte) + state (high byte)
};
static_assert(sizeof(Record) == 16, "Record is written to SD verbatim; keep it 16 bytes");

enum Kind : uint8_t {
  KindSleep = 0,
  KindBoot = 1,
  // Boots that woke, refused to come up, and went straight back to sleep. They are counted
  // in NVS as they happen (nothing else survives that path) and drained into the ring by
  // the next boot that actually completes — ONE record per distinct reason, carrying that
  // reason in `reason`/`code` and how many times it fired in `msA`. Per reason rather than
  // per run because "it refused 7 times" and "it refused 4 times for one reason and 3 for
  // another" are different faults.
  KindAborted = 2,
  // A book open that never produced a page. The resume watchdog writes one of these just
  // before restarting the device, so the reason for the restart outlives it — without it a
  // watchdog reboot is indistinguishable from the user pressing Reset.
  KindResumeStall = 3,
  // setup() itself had not finished. Distinct from KindResumeStall because `code` means a
  // different thing — a BootPhase rather than a WakeTrace phase — and because the two
  // narrow the fault to different halves of the boot.
  KindBootStall = 4,
};

enum class PreviousSession : uint8_t {
  Unknown,            // no usable boundary after this boot
  EndedAtSleepPath,   // a sleep record precedes this boot: an orderly end
  EndedWithoutSleep,  // a boot or stall precedes this boot: reset/crash while awake
};

/// Record::flags bits. Meanings are kind-specific, so stall and sleep bits may overlap.
enum SleepFlags : uint8_t {
  // Stall records only. The storage call had announced itself but had not acquired
  // the shared-SPI mutex. This implicates another bus owner rather than the SD transfer.
  kFlagStorageWaitingForSpi = 1 << 0,
  // Stall records only. The storage call acquired shared SPI but was waiting behind
  // another storage operation. This is the signature expected when that operation wedged.
  kFlagStorageWaitingForMutex = 1 << 1,
  // Stall records only. The storage call held both locks and was executing in SdFat or
  // the Arduino SPI HAL when sampled. Together with `reason`, this is the discriminator
  // for the issue-155 storage-wedge hypothesis.
  kFlagStorageActive = 1 << 2,
  // KindResumeStall/KindBootStall. Set even when storage was idle, so records made before
  // storage tracking existed remain distinguishable from evidence that actually sampled it.
  kFlagStorageSampled = 1 << 3,
  // Stall records only. The shared-SPI owner/state and BUSY pin were sampled into msC/pad.
  kFlagSpiSampled = 1 << 4,
  // Stall records only. Raw EPD_BUSY GPIO level, deliberately not interpreted by polarity.
  kFlagPanelBusyHigh = 1 << 5,
  kFlagKeepClock = 1 << 0,       // battery latch stays HIGH, MCU powered through sleep
  kFlagFromReader = 1 << 1,      // slept with a book open
  kFlagReleaseTimeout = 1 << 2,  // the power-button release wait gave up
  kFlagStageFromRtc = 1 << 3,    // final stage was amended from the RTC breadcrumb
  // Set when the record is written, cleared by the next boot that finalises it. While set,
  // `code` is only the stage reached at write time — the last steps had not run yet — so it
  // must not be read as a verdict.
  kFlagInProgress = 1 << 4,
  // The breadcrumb did not survive, so the outcome was deduced from the fact that the rail
  // went away rather than measured.
  kFlagStageInferred = 1 << 5,
  // KindResumeStall only. The long task was still ticking through its yield points when the
  // stall was recorded — slow, not wedged. Its absence is the stronger signal: nothing
  // reached a yield point at all, so the work is stuck inside one uninterruptible step
  // rather than grinding through many.
  kFlagStillTicking = 1 << 6,
};

inline uint16_t packSpiEvidence(uint8_t operation, uint8_t state) {
  return static_cast<uint16_t>(operation) | (static_cast<uint16_t>(state) << 8);
}

inline uint8_t stallSpiOperation(const Record& record) { return static_cast<uint8_t>(record.pad & 0xFF); }

inline uint8_t stallSpiState(const Record& record) { return static_cast<uint8_t>(record.pad >> 8); }

/// How far a deep-sleep attempt got. Ordered: a larger value is strictly more progress.
enum class SleepStage : uint8_t {
  None = 0,
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

/// Why the firmware decided to sleep.
enum class SleepTrigger : uint8_t {
  Unknown = 0,
  PowerHold,         // power button held past the sleep threshold
  Timeout,           // inactivity auto-sleep
  ButtonAction,      // a button mapped to the Sleep action
  WakeGateRejected,  // woke, gate refused the press, going straight back down
  UsbPowerBoot,      // USB power caused a cold boot; nothing else was initialised
  Count,
};

/// How a sleep record's `code` should be read once finalised.
///
/// Deliberately does NOT lean on esp_reset_reason() to separate a clean power-down from a
/// reset-button rescue: on the ESP32-C3 it cannot. reset_reason.c has no ESP_RST_EXT case
/// at all — RESET_REASON_CHIP_POWER_ON covers both a real power-on and a CHIP_PU (EN pin)
/// reset — so the two boots are indistinguishable at that level by hardware.
enum class SleepOutcome : uint8_t {
  ReachedDeepSleep,   // measured: the breadcrumb survived and showed the wake source armed
  ReleaseTimedOut,    // measured: the release wait gave up, so the device was still running
  PoweredOffAsAsked,  // the rail was cut, which is what this sleep's policy asked for
  PoweredOffUnasked,  // the rail was cut although the policy said the MCU should stay up
  DidNotSleep,        // the recorded stage itself shows it never got there
  Unfinished,         // still flagged in-progress — the finalising boot never happened
};

inline SleepOutcome outcomeOf(const Record& record) {
  if (record.kind != KindSleep) {
    return SleepOutcome::DidNotSleep;
  }
  if ((record.flags & kFlagInProgress) != 0) {
    return SleepOutcome::Unfinished;
  }
  // Measured, and the only failure verdict that survives a rail cut: the release wait gave
  // up, so the device was still executing long after it should have been asleep.
  if ((record.flags & kFlagReleaseTimeout) != 0) {
    return SleepOutcome::ReleaseTimedOut;
  }
  if ((record.flags & kFlagStageInferred) != 0) {
    // All that is known is that the rail went away. Whether that is the right answer
    // depends on what this sleep asked for: a latch-cut sleep powering the board off is the
    // intended end, a latch-kept one is not.
    return (record.flags & kFlagKeepClock) != 0 ? SleepOutcome::PoweredOffUnasked : SleepOutcome::PoweredOffAsAsked;
  }
  return record.code >= static_cast<uint8_t>(SleepStage::WakeArmed) ? SleepOutcome::ReachedDeepSleep
                                                                    : SleepOutcome::DidNotSleep;
}

// ---------------------------------------------------------------------------
// Ring layout
// ---------------------------------------------------------------------------

/// How many events the ring keeps. 16 covers eight full sleep/wake cycles at 272 bytes on
/// the card.
inline constexpr uint8_t kCapacity = 16;

constexpr uint32_t kRingMagic = 0x57424431;  // 'WBD1'
/// Bumped to 2 when version-1 sleep records turned out to be indistinguishable from an
/// artefact (they were written at the PanelAsleep stage and never finalised on a device
/// whose sleep cuts the battery latch). Stale versions are discarded rather than shown.
constexpr uint16_t kRingVersion = 2;

struct RingHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t count;     // valid records, <= kCapacity
  uint16_t head;      // index of the OLDEST record
  uint16_t reserved;  // keeps nextSeq 4-byte aligned in the file image
  uint32_t nextSeq;
};
static_assert(sizeof(RingHeader) == 16, "RingHeader is written to SD verbatim");

inline constexpr size_t kImageBytes = sizeof(RingHeader) + sizeof(Record) * kCapacity;

inline RingHeader ringHeaderOf(const uint8_t* image) {
  RingHeader header{};
  memcpy(&header, image, sizeof(header));
  return header;
}

inline void ringSetHeader(uint8_t* image, const RingHeader& header) { memcpy(image, &header, sizeof(header)); }

inline void ringGet(const uint8_t* image, uint8_t slot, Record& out) {
  memcpy(&out, image + sizeof(RingHeader) + static_cast<size_t>(slot) * sizeof(Record), sizeof(Record));
}

inline void ringSet(uint8_t* image, uint8_t slot, const Record& in) {
  memcpy(image + sizeof(RingHeader) + static_cast<size_t>(slot) * sizeof(Record), &in, sizeof(Record));
}

/// Reset an image to an empty, valid ring.
inline void ringInit(uint8_t* image) {
  memset(image, 0, kImageBytes);
  const RingHeader header{kRingMagic, kRingVersion, 0, 0, 0, 1};
  ringSetHeader(image, header);
}

/// Whether an image read off the card is a ring this build can use.
inline bool ringValid(const uint8_t* image) {
  const RingHeader header = ringHeaderOf(image);
  return header.magic == kRingMagic && header.version == kRingVersion && header.count <= kCapacity &&
         header.head < kCapacity;
}

/// Slot holding the newest record, or kCapacity when the ring is empty.
inline uint8_t ringNewestSlot(const RingHeader& header) {
  if (header.count == 0) {
    return kCapacity;
  }
  return static_cast<uint8_t>((header.head + header.count - 1) % kCapacity);
}

/// Append `record`, stamping it with the next sequence number. Overwrites the oldest entry
/// once full.
inline void ringAppend(uint8_t* image, Record& record) {
  RingHeader header = ringHeaderOf(image);
  record.seq = header.nextSeq++;
  uint8_t slot;
  if (header.count < kCapacity) {
    slot = static_cast<uint8_t>((header.head + header.count) % kCapacity);
    header.count++;
  } else {
    slot = header.head;
    header.head = static_cast<uint16_t>((header.head + 1) % kCapacity);
  }
  ringSet(image, slot, record);
  ringSetHeader(image, header);
}

/// Copy up to `maxRecords` entries into `out`, newest first. Returns how many were filled.
inline uint8_t ringLoadNewestFirst(const uint8_t* image, Record* out, uint8_t maxRecords) {
  const RingHeader header = ringHeaderOf(image);
  const uint8_t wanted = header.count < maxRecords ? static_cast<uint8_t>(header.count) : maxRecords;
  for (uint8_t i = 0; i < wanted; i++) {
    const uint8_t slot = static_cast<uint8_t>((header.head + header.count - 1 - i + kCapacity) % kCapacity);
    ringGet(image, slot, out[i]);
  }
  return wanted;
}

/// Classify the session before the newest boot. Abort records are summaries of boot
/// attempts that never reached the SD ring, not session boundaries, so walk past all of
/// them. A stall marker followed by a boot is itself evidence of an awake reset.
inline PreviousSession previousSessionOf(const Record* newestFirst, uint8_t count) {
  if (newestFirst == nullptr || count < 2 || newestFirst[0].kind != KindBoot) {
    return PreviousSession::Unknown;
  }
  for (uint8_t i = 1; i < count; i++) {
    switch (static_cast<Kind>(newestFirst[i].kind)) {
      case KindAborted:
        continue;
      case KindSleep:
        return PreviousSession::EndedAtSleepPath;
      case KindBoot:
      case KindResumeStall:
      case KindBootStall:
        return PreviousSession::EndedWithoutSleep;
    }
    return PreviousSession::Unknown;
  }
  return PreviousSession::Unknown;
}

/// Index of the first record belonging to the run that ended with THIS boot, and one past
/// its last, in a newest-first list.  Returns false when there is no such run.
///
/// Slot 0 is this boot's own record, because persistBoot() appends the abort summary before
/// it — so the walk has to step over one KindBoot and stop at the next, which is the
/// previous completed boot.  Getting that backwards is what made the abort counter read
/// zero for every device that had actually been looping.
inline bool currentRunRange(const Record* newestFirst, uint8_t count, uint8_t& first, uint8_t& last) {
  bool steppedOverOwnBoot = false;
  first = 0;
  last = 0;
  for (uint8_t i = 0; i < count; i++) {
    if (newestFirst[i].kind == KindBoot) {
      if (steppedOverOwnBoot) {
        last = i;
        return first < last;
      }
      steppedOverOwnBoot = true;
      first = static_cast<uint8_t>(i + 1);
      continue;
    }
  }
  last = count;
  return steppedOverOwnBoot && first < last;
}

/// Total aborted boots in the run that ended with this boot.  Sums every KindAborted record
/// in the run rather than taking the first: persistBoot() emits ONE record per distinct
/// abort reason, so a device that refused for two different reasons has two of them.
inline uint16_t abortsInCurrentRun(const Record* newestFirst, uint8_t count) {
  uint8_t first = 0;
  uint8_t last = 0;
  if (!currentRunRange(newestFirst, count, first, last)) {
    return 0;
  }
  uint32_t total = 0;
  for (uint8_t i = first; i < last; i++) {
    if (newestFirst[i].kind == KindAborted) {
      total += newestFirst[i].msA;
    }
  }
  return static_cast<uint16_t>(total > UINT16_MAX ? UINT16_MAX : total);
}

}  // namespace BootDiag
