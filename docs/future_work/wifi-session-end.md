# Wi-Fi session end without a restart

Handover, 2026-11-27. Leaving a screen that used Wi-Fi used to restart the device on every board, to get the heap
back. PR 1 (#941) ends a session in one shared step instead, `wifi_session::end(tag)`
(`src/network/WifiSessionEnd.cpp`). The step turns the radio off, forgets the session's unverifiable TLS hosts,
measures the internal heap and returns a verdict (`src/network/WifiSessionVerdict.h`):

- The C3s (X4, X3) restart every time (`HalCapabilities::restartsAfterWifiSession()`).
- A PSRAM board (X4 Pro, T5S3, Murphy, Picco) carries on in the same boot unless the free internal heap is below
  67,584 B or its largest block below 24,576 B. Those are the figures the background KOReader sync already used,
  now defined once.
- Any board restarts without touching the radio while a deep sleep is under way or the background sync worker
  holds the radio.

The open items are one file each in [wifi-session-end/](wifi-session-end/). A PR that closes an item deletes its
file, and a follow-up it finds goes into a new file. Neither touches this page or this topic's row in
[README.md](README.md), so PRs on different items never conflict over these docs.

## Where things are

- The design, binding for PR 2 and PR 3:
  [spec](../superpowers/specs/2026-11-26-wifi-exit-without-reboot-design.md), "Site changes, by PR". The PR 1 plan,
  as a pattern for the next two: [plan](../superpowers/plans/2026-11-26-wifi-exit-pr1.md).
- What was measured, and where the heap goes: [network-stack-findings.md](../network-stack-findings.md), "Wi-Fi
  session end on an ESP32-S3". In short, the first session costs about 18.8 KB once (three network tasks that
  ESP-IDF cannot stop, plus about 3.5 KB of small blocks); each later session keeps 32 B.
- Done in PR 1, carrying on where the heap allows: KOReader sync and the account check, Sync Time, Detect
  Timezone, Weather, the Settings actions (Wi-Fi Networks, Weather city search), the OPDS browser, and the File
  Transfer mode picker after a backed-out join.
- Still restarting on every board: the downloads (PR 2) and the web server (PR 3).

## Order of work

1. [PR 2, downloads](wifi-session-end/pr-2-downloads.md) and [PR 3, the web server](wifi-session-end/pr-3-web-server.md).
   They touch different files and can run in parallel. Each follows PR 1's pattern: where the site restarts today,
   call `wifi_session::end(tag)`. On `Reboot`, keep today's restart and its target. On `CarryOn`, navigate the way the
   site does when it does not restart, and restore whatever the restart used to restore (each item lists that).
2. The two leftovers PR 1 found but did not cause, both small:
   [clock screens under a busy worker](wifi-session-end/clock-screens-turn-the-radio-off-under-a-busy-worker.md),
   [radio left up by a global action](wifi-session-end/a-global-action-leaves-the-radio-up.md).
3. [Device runs not yet done](wifi-session-end/device-runs-left.md), then the small items:
   [SNTP stop](wifi-session-end/sntp-is-stopped-only-when-the-radio-was-up.md),
   [the C3 log line](wifi-session-end/c3-session-end-log-reads-high.md),
   [the 28-byte block](wifi-session-end/the-28-byte-block-each-session-keeps.md).

## Checking a site on a device

PR 1 ran on the Picco; any PSRAM board does. What each check looks for:

- **The log line.** Every session end logs `[WIFI] Session end [<tag>]: internal free=... largest=... (boot .../...,
  min 67584/24576) -> carry on` (or `-> restart`). On a carry-on, the uptime in the next log lines keeps rising (no
  restart), and `CMD:STATE` shows the screen the site should return to. On PSRAM builds `STATE` also reports
  `imaxblock`, the largest internal block (`heap` is internal free).
- **A second session after a carry-on** must join and finish. PR 1 ran five KOReader syncs in a row, then Weather.
- **The worker busy.** `CMD:SET:koPushIntervalPages=1`, open a book, turn a page (the log shows `Silent push
  queued`), go Home, open the site within about 4 s, and leave it. Expect `the background sync holds the radio;
  restarting`, then the site's restart target, and a clean `CMD:BOOTLOG`. Set the interval back afterwards.
- **Sleep from the site** with the radio up: long-press Power (`CMD:BTN:POWER:LONG`). On the Picco the short press
  is mapped to Confirm. The device must sleep without restarting and wake normally.
- **The minimums refusing.** A local build, never committed, with `kMinFreeInternalBytes` set to 500000 in
  `WifiSessionVerdict.h`. Expect `[ERR] ... under the minimums 500000/24576; restarting`, then today's target.
- **The C3s** must still restart at every site, with the radio fully torn down before.

Flashing the Picco: it runs Onyx's stock partition table, so write only `boot_app0.bin` at 0x15000 and
`firmware.bin` at 0xE0000 with esptool. Never `pio run -e picco -t upload`, which overwrites Onyx's bootloader and
recovery partition.

## Traps

- **Detect Timezone applies what it detects.** Over a phone hotspot that can be a city far away (PR 1's runs got
  America/Los_Angeles). Read `CMD:GET:clockTimezone` first and set it back afterwards. A serial `SET` saves the
  value, but the clock shows the new zone only after the next restart.
- **Weather with a fresh cache never brings the radio up**, so its exit has no session to end. Use a site that
  always joins (Sync Time, KOReader sync) when a check needs a session.
- **The credential stores** (`wifi.json`, `opds.json`, `koreader.json`) cannot be read or written over serial.
  Enter a network or account through the device UI. `CMD:TOUCH` on the on-screen keyboard works; the URL keyboard
  starts with `https://` already in the field, and its symbols are on a second layout.
- **Builds.** In a fresh worktree, a second `pio run` (another env, or `-t upload`) wipes the first env's build
  directory; so does a change of the SDK pin or of the set of files under `src/` or `lib/`. Copy `firmware.bin` out
  before the next run.
- **The heap map** that found the three network tasks used two temporary serial commands, `CMD:HEAPMAP`
  (`heap_caps_walk()` over the internal heap, plus the task list) and `CMD:WIFICYCLE` (join, one optional HTTPS
  request, radio off). They are on the local branch `spike/wifi-heap-map`, which is not meant to merge. Rebuild them
  from that description if a later change needs a new map.
