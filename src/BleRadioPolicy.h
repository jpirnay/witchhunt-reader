#pragma once

#include <cstddef>
#include <cstdint>

// The Bluetooth LE radio's rules (src/BleRadio.cpp): when it may start, when a start is rolled back, when a running
// radio stops. Pure and constexpr, so the host tests run them on a pretend heap and a pretend clock.
//
// The shape of these rules is ported from CrossPoint Reader #3785 (Tuan Q. Nguyen, with Justin Mitchell) and #4074
// (Tuan Q. Nguyen), lib/BlePageTurner/src/RadioPolicy.h; the numbers are ours, measured on the S3 boards.
namespace BleRadioPolicy {

// Internal RAM (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT): the radio's cost is all there.
struct Heap {
  size_t freeBytes;
  size_t largestBlock;
};

// What the radio takes and what has to stay beside it.
struct Profile {
  size_t stackBytes;      // internal RAM the controller and the host take while up
  size_t minimumLargest;  // the internal block kept whole beside the radio, before and after the start
};

// S3 with NimBLE's host in PSRAM (scripts/s3_nimble_psram.h), SDK BleKeyboardHost on NimBLE-Arduino 2.5.1, measured at
// Home with CMD:STATE on 2026-11-28: begin() took 46,608 B (X4 Pro), 46,648 B (T5S3) and 46,568 B (Picco) of internal
// RAM, and up to 45,056 B of the largest block (X4 Pro). Margin: one allocator page.
inline constexpr size_t kS3StackBytes = 46648 + 4096;
// The prebuilt S3 core sends malloc and new above 4,096 B to PSRAM (sdkconfig: CONFIG_SPIRAM_USE_MALLOC=y,
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096), so beside the radio nothing in the reader asks internal RAM for a bigger
// block; task stacks and DMA buffers do. Six such requests stay possible, as CrossPoint keeps for its S3 profile.
inline constexpr size_t kS3InternalAllocMax = 4096;
inline constexpr size_t kS3MinimumLargest = 6 * kS3InternalAllocMax;
inline constexpr Profile kS3Profile{kS3StackBytes, kS3MinimumLargest};

// A radio up this long with no remote linked or linking stops: it holds the CPU at full speed and keeps the chip out of
// light sleep. The SDK's reconnect plans (armBondedReconnect) scan for 120 s after they are armed; past that nothing
// links a remote that has not been asked for again, so the radio would only spend the battery.
inline constexpr uint32_t kIdleNoLinkMs = 120u * 1000u;

enum class Verdict : uint8_t {
  Ok,
  StorageBusy,   // USB Drive has the card: the radio stays off
  HeapLow,       // too few bytes in total for the radio and the room beside it
  HeapInPieces,  // the bytes are there, but no block the reader can still use would be left
};

// Before begin(): the radio's bytes plus the block kept beside it, and that block already whole.
constexpr Verdict beforeStart(const Heap heap, const bool storageBusy, const Profile& profile = kS3Profile) {
  if (storageBusy) return Verdict::StorageBusy;
  if (heap.freeBytes < profile.stackBytes + profile.minimumLargest) return Verdict::HeapLow;
  if (heap.largestBlock < profile.minimumLargest) return Verdict::HeapInPieces;
  return Verdict::Ok;
}

// Right after begin(): the start found the bytes, so a radio that left no whole block behind split the heap. Rolled
// back (end()) when this is not Ok.
constexpr Verdict afterStart(const Heap heap, const bool storageBusy, const Profile& profile = kS3Profile) {
  if (storageBusy) return Verdict::StorageBusy;
  return heap.largestBlock >= profile.minimumLargest ? Verdict::Ok : Verdict::HeapInPieces;
}

enum class Keep : uint8_t {
  Keep,
  StopStorage,     // USB Drive took the card while the radio was up
  StopIdleNoLink,  // kIdleNoLinkMs with nothing linked
};

// Every loop pass while the radio is up. `idleMs` counts from the start or from the last moment a remote was linked.
// A linked remote is never idle: it sits quiet between two presses for as long as the page is read.
constexpr Keep whileRunning(const bool linked, const bool connecting, const uint32_t idleMs, const bool storageBusy) {
  if (storageBusy) return Keep::StopStorage;
  if (!linked && !connecting && idleMs >= kIdleNoLinkMs) return Keep::StopIdleNoLink;
  return Keep::Keep;
}

}  // namespace BleRadioPolicy
