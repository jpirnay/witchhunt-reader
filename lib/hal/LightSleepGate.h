#pragma once

#include <cstdint>

// What on the panel's side keeps the idle loop from halting the chip (HalPowerManager::lightSleep()): the decision
// alone, apart from the HAL, so the host can test it.
namespace LightSleepGate {

enum class PanelHold : uint8_t {
  None,
  RenderLock,      // a HalPowerManager::Lock is held: a render pass, or work of its kind, is under way
  RefreshPending,  // a display driver started a refresh that nothing has finished yet (HalDisplay::isRefreshPending)
};

// A refresh that is pending has its waveform on the panel, or its finish still to come: the wait for BUSY, the power
// off, the gray a single push still owes. Halted under it, the chip loses a BUSY edge a finish waits for by interrupt
// (EpdBus::waitRefreshComplete(), which then sits out a 30 s timeout), and a panel that asked to be powered off stays
// powered for as long as the device idles.
//
// A Lock outranks it: a pass that holds one finishes its own refresh, and LightSleepStats::rejLock keeps its meaning.
constexpr PanelHold panelHold(const bool lockHeld, const bool refreshPending) {
  if (lockHeld) return PanelHold::RenderLock;
  if (refreshPending) return PanelHold::RefreshPending;
  return PanelHold::None;
}

}  // namespace LightSleepGate
