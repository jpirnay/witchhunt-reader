# PR 3: no restart after the web server on the PSRAM boards

Part of [Wi-Fi session end](../wifi-session-end.md).

**Open, designed, not started.** The design is the spec's "PR 3: the web server"
([spec](../../superpowers/specs/2026-11-26-wifi-exit-without-reboot-design.md)). File Transfer
(`CrossPointWebServerActivity` with `ServerSession`, in station and hotspot mode) and `CalibreConnectActivity`
still restart on every board when the server stops.

What the restart covers for them today, and what a carry-on must do instead:

- `releaseFrameBuffers()` nulls the renderer's framebuffer pointer on every board, although the S3s keep the
  buffers. On boards where `restartsAfterWifiSession()` is false, do not call it. Keep `buffersReleased` so that
  `render()` still paints nothing during the session.
- State read only at boot, which a web or serial session can change. After a carry-on, refresh each of these:
  - the plugin subscriptions: `plugin_events::refreshSubscriptions()`;
  - the active UI language: a language pack can arrive by upload;
  - the UI theme and UI font size set through the web API or serial `SET`;
  - the SD-font family list after plain uploads into a font root: `discover()`.

  Do it unconditionally, unless one of them measures slow on the device.
- **Since #935** (merged 2026-11-27), `SettingsActivity::applyUiLookIfChanged()` re-applies the theme and font size
  at once on the PSRAM boards, but only for a change made on the device's Settings screen. Share it, or what it
  calls, for the web and serial path instead of writing a second copy.
- Builder task: list every `SettingsList.h` row that the web API or serial `SET` can write whose effect is applied
  only at boot or only by a device screen's hook, and apply each one after the session.
- `CrossPointWebServer.cpp`: reset the file-scope upload state when the server starts and stops, and close
  `wsUploadFile` if it is open.
- Calibre: Back lands on File Transfer's Calibre row through today's restart target. A plain `finish()` would open
  the picker on its first row, so a carry-on must return to the Calibre row.

**Next step:** a plan from the spec's PR 3 section. Then a device run on a PSRAM board:
- an upload and a delete through the web page;
- a UI language pack and an SD font uploaded, and a theme changed through the web API, each in effect without a
  restart;
- the hotspot mode;
- Calibre's Back;
- a second session after the carry-on.
