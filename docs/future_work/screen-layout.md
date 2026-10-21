# Screen layout

Status: open items as of 2026-10-21, from the header/body audit of PR #543. Screens lay out their
body with `src/components/themes/HeaderLayout.h` and `UITheme::getBodyRect()`; the items below were
found in the audit and left out of that PR.

## Overlaps

- **Keyboard, X4 landscape CCW:** the header still runs under the side boxes.
  `keyboardContentRect()` (`KeyboardEntryActivity.cpp`) reserves side columns only on the X3;
  reserving them on the X4 narrows its keyboard.
- **Wi-Fi legend:** overlaps the last network row when the list is full (pre-existing).
- **Reading stats in landscape at the Large UI font:** the cards do not fit; the history card can
  reach the Base subtitle line in landscape CW. Only the optional 30-day card is dropped today.
- **KOReader Sync screen:** rows on a 25 px pitch overlap the 30 px lines of the Large UI font.
- **Dictionary definition:** the headword and the status text collide inside the header band.
- **Detect Timezone:** its two lines sit 20 px apart.

## Wrong arithmetic

- The Markdown and XTC chapter lists count rows per page from a different rect than they draw in.
- System Information and Boot Diagnostics subtract the hint strip twice.
- BaseTheme's title-width budget mixes absolute and relative x.
- `CrossPointWebServer` (the screen) draws its header twice per frame.

## Cleanup

- About 20 screens that already lay out correctly still compute the body top themselves; switching
  them to `HeaderLayout` removes the duplicates (no visible change). The chapter lists could also use
  `overlayTitleY`.
- OtaUpdate has hard-coded English strings (not `tr(STR_*)`).
- Wrong comments: the Weather screen calls itself portrait; `ActivityManager.cpp` says the S3 loop
  task runs on core 0 (it runs on core 1).
- The SDK widget `freeink::ui::header()` overlaps title and subtitle (`subtitle` starts at
  `y + titleLineH` while the title is centred). This repo does not use it; for freeink-sdk.
