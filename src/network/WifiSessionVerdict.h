#pragma once

#include <cstdint>

// What a Wi-Fi session needs from the internal heap, and the verdict at a Wi-Fi screen's exit: reboot, or carry on
// in the same boot (wifi_session::end(), WifiSessionEnd.h). Pure, so the host tests cover it.
//
// The minimums are the background KOReader sync's (KOReaderAutoSync::heapAllowsBackgroundSession), measured from the
// worker's logs on an X4 Pro for exactly the question asked here: can a Wi-Fi session start now. The worker's stack is
// part of the free figure because a background sync may well be the next session after a screen carries on.
// "Internal" is MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT: on the PSRAM boards the Wi-Fi stack's buffers and every task
// stack come from there, and it is the pool a session leaves smaller.
namespace wifi_session {

enum class Exit : uint8_t { CarryOn, Reboot };

constexpr uint32_t kWorkerStackBytes = 10240;  // KOReaderSyncWorker::WORKER_STACK_BYTES (asserted there)
// Association and the WPA supplicant; the TLS client gates its own handshake.
constexpr uint32_t kAssociationHeadroomBytes = 56 * 1024;
constexpr uint32_t kMinFreeInternalBytes = kWorkerStackBytes + kAssociationHeadroomBytes;
// The largest single block a session draws: a request's chunk, 14 KB at worst on an X4 Pro.
constexpr uint32_t kMinLargestInternalBytes = 24 * 1024;

// restartsAfterSession is HalCapabilities::restartsAfterWifiSession(): true where the reboot also gives back the
// secondary framebuffer the session released (the C3s), so they reboot whatever the heap says.
constexpr Exit verdict(const bool restartsAfterSession, const uint32_t freeInternal, const uint32_t largestInternal) {
  if (restartsAfterSession) return Exit::Reboot;
  if (freeInternal < kMinFreeInternalBytes || largestInternal < kMinLargestInternalBytes) return Exit::Reboot;
  return Exit::CarryOn;
}

}  // namespace wifi_session
