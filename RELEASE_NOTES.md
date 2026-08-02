# Release Notes

User-facing changes only. Full commit history is in git log.

## 2.20 — 2026-08-02

- Major page-render speedup on styled books (most commercial EPUBs set a body font size): X4 page render ~450ms → ~50-100ms, X3 ~950ms → ~545ms.
- Page turns feel snappier — the anti-aliasing touch-up is now dropped when you navigate away mid-render instead of finishing it first.
- Fixed a freeze (up to ~28s) finishing a book that has sequels in the same folder.
- Faster library browsing, cover thumbnails, Wi-Fi/web file transfer, and opening of very large books.
- Fixed crashes/failures opening very large books (1700+ chapters) under low memory.
- Fixed a blank cover/title page on books with SVG-wrapped cover art.
- Added drop-cap support (large stylized first letters).
- Fixed button input on the "book finished" screen, dropped page-turns on side buttons, and a footnote-list double-skip.
- Left/Right buttons and labels now work correctly in the reversed (rotated) mounting orientation.
- X3: fixed the charging icon sticking on after unplugging, and the SD card not powering down in sleep (battery drain).
- More reliable Wi-Fi connects (DNS stall fix) and better AP selection when several share an SSID.
- Known issue: newer X3/X4 hardware batches with updated display panels aren't supported yet.

## 2.10.1 — 2026-07-17

- Fixed some EPUB3 books (common Calibre/Pandoc output) failing to open due to a metadata-parsing regression introduced in 2.10.

## 2.10 — 2026-07-17

- Reintroduced progressive JPEG support and reduced image-decoder memory use.
- Added guide dots and inline footnote previews from a book-level cache.
- Added configurable OPDS download folder/filename format.
- Fixed a crash entering deep sleep on the web-transfer screen, an AP low-memory crash on X3, and an out-of-bounds page bug on fast page-turns.
- Fixed ZIP scanning for large/fragmented EPUBs and reduced CSS-related OOM risk.
- Faster shutdown; ~3KB flash saved via zopfli compression.

## 2.09-freeink — 2026-07-14

- Switched to the official freeink display SDK (from open-x4-sdk).
- Fixed several X4 ghosting issues (popup and section-crossing).
- Added an optional X4 display overclock for faster screen refresh, and SDK/version info on the System Information screen.

## 2.09 — 2026-07-14

- Fixed a cover-thumbnail deadlock on a corrupted cached cover image.
- Fixed dither-band artifacts in JPEG decoding.
- Kept full CPU speed while home-screen covers are still resolving (faster library home screen).
- Fixed the web server blocking book listing for Calibre-connected devices.

## 2.08 — 2026-07-13

- Fixed a sporadic missing page update.
- Folded the X4 anti-aliasing pass into the page-refresh window (fewer/faster refreshes).

## 2.07 — 2026-07-09

- Cut first-open time on large books via buffered EPUB cache I/O.
- Fixed secondary-framebuffer release during first-open book indexing.
- Completed the Swedish translation.

## 2.06 — 2026-07-08

- Added support for different font sizes and improved font scaling/reuse.
- Fixed KOReader sync (KOSync) failing under low memory.
- Fixed an OOM reboot from loading the printed-page list all at once.
- Fixed sleep-screen cover Fit/Crop display issues.

## 2.05 — 2026-07-02

- Improved font upscaling quality.
- Added support for rendering large embedded covers on the sleep screen.
- Added an optional physical page-number display.
- Fixed several ghosting issues (indexing popup, X4) and a classic-theme list display bug.

## 2.04 — 2026-06-29

- Added a live "page X of ~Y" estimate while a section is still building.
- Added a book-keyed HTML cache to skip re-inflation on reopen.
- Fixed reader resuming at the chapter start instead of the saved position.
- Re-rendered UI fonts with a native monochrome rasterizer for crisper text.

## 2.03 — 2026-06-26

- Fixed tall images not being sliced properly.
- Fixed a display-refresh race that could cause ghosting.
- Hardened OTA update-metadata fetching; added low-memory safety nets.

## 2.02 — 2026-06-24

- Reworked the web server to release memory properly (stability).
- Fixed anti-aliasing/half-refresh behavior when the sunlight fix is active.
- Fixed a couple of settings submenu bugs.

## 2.01 — 2026-06-24

- Fixed an XML parser regression that could cause books to fail to open.

## 2.00 — 2026-06-23

- Added USB serial file transfer (MicroReader-compatible), with host-side Total/Double Commander plugins.
- Made serial downloads reliable (fixed dropped bytes on transfer).
- Added "more books by this author" lookup and a Power-button shortcut to select a footnote.
- Fixed CSS parent/child margin collapsing.

## 1.99 — 2026-06-18

- Added file-browser sorting (name/date/size/type) and an options menu; bounded-RAM browsing for very large folders.
- Added smallcaps support and persistent file timestamps (RTC-backed).
- Fixed a downscaling darkness issue and carousel-exit ghosting.
- Improved cover-thumbnail handling and background slicing for carousel covers.

## 1.50 — 2026-06-15

- Replaced the JPEG decoder (TJpgDec), fixing several JPEG/GIF decoding bugs and X4 ghosting issues.
- Improved rendering speed for image-heavy documents and CSS-heavy books.
- Reworked the recent-books grid view and fixed stale highlights in it.
- Unified the input handling for TXT/MD readers with the main reader.
