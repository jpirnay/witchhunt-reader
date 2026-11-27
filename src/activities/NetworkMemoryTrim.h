#pragma once

class GfxRenderer;

// Frees the ~52 KB secondary framebuffer, the font cache and the scaled-glyph
// cache before a network session, so both esp_wifi_init and any later TLS handshake can obtain the
// large contiguous blocks they need on a constrained heap.
//
// CALL THIS BEFORE THE RADIO COMES UP — not just before TLS. Association is
// already allocation-heavy: bringing up the WiFi stack needs its own tens of KB
// for RX/TX buffers and the WPA supplicant. Two observed failure modes when the
// ~52 KB buffer is still resident: phy_init aborts outright with "failed to
// allocate memory for RF calibration data", or esp_wifi allocations return null
// and association fails silently, surfacing as a plain connect timeout with no
// association event. This matters most for
// activities launched from SettingsActivity, which stays on the activity stack
// below its children with four per-category SettingInfo vectors (~78 entries,
// each holding two nested vectors) resident and interleaved across the heap.
// Activities launched from Home do not carry that and have more headroom.
//
// Only the secondary buffer is released; the primary write target stays so the
// caller can keep rendering status and result screens. That is the difference
// from the web-server session, which releases both because it never draws
// again. Implements Pattern 1a
// from docs/secondary-buffer-management.md: RED RAM is seeded while
// frameBufferActive is still valid, so setSingleBufferFastDiff(true) diffs
// against a valid baseline rather than stale controller content (X4; no-op on
// X3).
//
// There is no reallocSecondaryBuffer() pairing. Every caller reboots on exit on
// the boards where this released the buffer: directly, or through
// wifi_session::end() (network/WifiSessionEnd.h), which does so wherever
// HalCapabilities::restartsAfterWifiSession() says (the PSRAM boards refuse the
// release). If you add a caller that keeps running afterwards on a C3, follow the
// full Scenario 1 procedure in the doc instead of using this helper.
//
// Idempotent: safe to call again before TLS to drop font cache that status
// screens repopulated since the first call.
void trimMemoryForNetworkSession(const GfxRenderer& renderer, const char* logTag);

// trimMemoryForNetworkSession(), then everything else the session will not draw or
// read again, for a download that needs the heap in one piece: the font manager and
// the firmware update, whose 16 KB TLS records (or the chunks that replace them) need
// contiguous blocks the X3 barely has with Wi-Fi up. On the boards where
// HalCapabilities::restartsAfterWifiSession() is true, the settings lists of the
// activities buried below are dropped (ActivityManager::releaseBuriedActivityState())
// and so is the loaded SD font, so a caller must reboot on exit there, as both do
// through wifi_session::end(). The other boards keep both: their screens below come
// back in this boot. The SD font registry stays everywhere; the font manager installs
// through it. Call it from the current activity, before WiFi comes up and again before
// the transfer; idempotent.
void releaseMemoryForDownload(const GfxRenderer& renderer, const char* logTag);
