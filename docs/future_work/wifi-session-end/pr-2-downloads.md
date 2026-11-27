# PR 2: no restart after a download on the PSRAM boards

Part of [Wi-Fi session end](../wifi-session-end.md).

**Open, designed, not started.** The design is the spec's "PR 2: downloads"
([spec](../../superpowers/specs/2026-11-26-wifi-exit-without-reboot-design.md)). The sites still restart on every
board after a session:

- `FontDownloadActivity`: fonts, dictionaries, UI language packs. Its `onExit()` restarts even when the radio is
  already down.
- `OtaUpdateActivity`: only the check for an update. An install still restarts.

What the restart covers for them today, and what a carry-on must do instead:

- `releaseMemoryForDownload()` (`src/activities/NetworkMemoryTrim.cpp`) drops the buried Settings rows on every
  board, and nothing rebuilds them. `ActivityManager` restarts if such a screen is resumed (its
  `buriedStateReleased` check). On boards where `restartsAfterWifiSession()` is false, keep the buried screens'
  state and the loaded SD font. Log the download's lowest internal free figure in the device run. If that is too
  tight, the fallback is to rebuild a released Settings on resume, not to bring the restart back.
- After an installed UI language pack, re-apply the active language (`UiLanguage::apply(I18N.getLanguage())`, or
  what it needs). Today the pack is read only at the next start.
- Navigation follows the entry route. Pushed over Settings: `finish()`. Opened by replace (the reader's
  `PackOffer`, the OTA handoff): the same target as today's restart (the book for a book, otherwise Settings or
  Home).
- The OTA check: on a carry-on, back to Settings on its tab and row.

**Next step:** a plan from the spec's PR 2 section, built like PR 1's
([plan](../../superpowers/plans/2026-11-26-wifi-exit-pr1.md)). Then a device run on a PSRAM board, per "Checking a
site on a device" on the topic page: a font download, a dictionary, a UI language pack (the UI must change without
a restart), and an OTA check with no update available.
