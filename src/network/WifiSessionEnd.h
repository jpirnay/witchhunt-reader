#pragma once

#include "WifiSessionVerdict.h"

// The end of a Wi-Fi session, at a Wi-Fi screen's exit: the radio off, the session's TLS memory forgotten, the internal
// heap measured and logged, and the verdict (WifiSessionVerdict.h). A screen keeps the condition it rebooted under and
// calls end() inside it:
//
//   if (wifi_session::end("TAG") == wifi_session::Exit::Reboot) silentRestartToX();
//   // else its ordinary exit: the navigation already queued, or finish()
//
// The C3s reboot every time (HalCapabilities::restartsAfterWifiSession()), which also gives back the secondary
// framebuffer the session released; the PSRAM boards carry on while the heap passes the minimums.
// Design: docs/superpowers/specs/2026-11-26-wifi-exit-without-reboot-design.md.
namespace wifi_session {

// The internal heap the session-end log lines compare against. Called once, at the end of setup().
void recordBootBaseline();

// Returns Reboot without touching the radio while a deep sleep is in progress (the silentRestart*() that follows does
// nothing then, and the sleep path turns the radio off itself) or while the background KOReader sync worker holds it
// (deinitialising Wi-Fi under a job's open socket crashes).
Exit end(const char* tag);

}  // namespace wifi_session
