# Release Notes

User-facing changes only. Full commit history is in git log.

## 2.26 — 2026-08-28

- **Buttons now follow the screen, not the panel.** The four front buttons and the two side buttons sit on different edges of the device, so rotating it swaps which axis each pair moves along — but navigation still assumed portrait. In landscape a list stepped with the buttons beside the screen and jumped pages with the ones below it, the two directions were often reversed, and the hint labels named buttons that did something else. Every list, cursor, keyboard and picker now works out which physical button points the way you mean for the orientation you are holding, and each hint is drawn beside the button that performs it. Page turns, yes/no prompts and your own short/double/long press assignments keep their fixed buttons, as before.
- Fix: held upside down (inverted portrait), the labels on the four front buttons were drawn upside down.
- **Fix: verse and quotations lost their first letters** (#198). Poetry is usually marked up with the indent on the stanza and the hanging indent on each line. Only the first line of a stanza ever picked up the stanza's indent, so from the second line on the text started off the left edge of the screen and its first letter was clipped away. Text could also be pushed off-panel by a negative margin, or by a hanging indent deeper than the indent it hangs out of. Two visible consequences: a line continued with a line break keeps its paragraph's indent instead of getting a fresh first-line indent, and a table inside an indented block is now indented with it.
- **Fix: sleep-screen covers came out nearly black with the "Adaptive" filter.** The filter corrected the picture's own tonal range and the reader then dropped the panel's brightness measurements, so it re-corrected on top — on a sample cover 87% of the image landed in the two darkest levels against 31% with the filter off. Book images were never affected; only the sleep screen.
- **New "Equalize" cover filter** (Settings → Display → Sleep Screen Cover Filter). "Adaptive" stretches a picture between its darkest and lightest points, which does nothing for a cover that is mostly dark with a small bright title — five of seven sample covers came out effectively unchanged. Equalize spreads the tones by how much of the picture actually carries them, so those covers gain real contrast. It is a stronger transform, so smooth backgrounds show a little more dither texture; both are worth trying on your own covers. Applies to inline book images as well.
- **HTTPS connections are now really verified.** Every https request checked the server's certificate and then quietly repeated the request without checking when that failed, so a bad certificate was simply accepted — including on the KOReader sync connection, which carries your sync password. Verification failures are now reported instead. To reach a self-hosted server with a private or self-signed certificate there is a new **Skip HTTPS validation (risky)** toggle under Settings → System; firmware updates stay verified whatever it is set to.
- Fix: signing in to or syncing with some KOReader servers reported a Wi-Fi login page ("captive portal") when the server was simply saying it had nothing stored for that book yet. Servers spell that several ways, and two of them — an empty JSON string and a bare `null` — were being read as a portal.
- **Checking the time is about three seconds faster.** The system clock's time request used to sit in a random delay of up to five seconds before the first packet left the device, on an exchange that takes a few hundredths of a second. The reader now asks directly, and only falls back to the old path if that gets no answer. This delay was paid on every sync, timezone detection and weather update that needed the clock set.
- Wi-Fi connects more reliably: the radio no longer dozes between beacons while connecting, which is what turned a weak moment into a ten-second dead attempt before the retry, and the channel scan is back at its longer dwell after a shorter one made the reader miss access points it could see perfectly well.
- **About 30 KB more memory free**, on a device with roughly 380 KB in total: the Wi-Fi driver's non-critical code is loaded from flash rather than kept resident, two system tasks had their stacks cut to what they actually use, and unused cloud-service components are no longer built in. More free memory means fewer of the restarts and slow rebuilds that a fragmented heap causes. The trade is lower Wi-Fi throughput during transfers, which is a fair swap for a device that syncs a page number and occasionally downloads a book.
- Fix: a PNG uploaded through the web file browser was converted to a JPEG on its way to the card (#196). Only JPEGs are re-encoded now, which is what that step was for.
- "Auto page turn" moved to the top of the reader menu, beside the other reading controls.
- Fix (X4): heavy ghosting after an anti-aliased page. The panel's charge pump was left powered between an anti-aliased page and the next black-and-white one, and could not clear the grey charge at reduced strength.
- Upgrade notes: chapters are re-indexed once per book the first time you open them, because indented blocks are laid out differently — your place in every book is kept. The two new setting labels ("Skip HTTPS validation" and the "Equalize" filter) are English-only in this release; the other 14 languages show the English text until the next translation pass.

## 2.25 — 2026-08-22

- **New: dictionary lookup.** Copy a StarDict dictionary into `/dictionaries` on the card, choose it under Settings → Reader → Dictionary, then look a word up from the reader menu or from any button you map to the Dictionary action. Left/Right steps word by word, the side buttons jump between lines, Confirm shows the definition. Definitions keep the dictionary's own headings and emphasis when there is memory for it and fall back to plain text otherwise. With more than one dictionary installed, Confirm on the definition screen switches to the next one and remembers the choice. Plurals, possessives, verb endings and a dictionary's own synonym list are tried automatically, and a hyphenated compound falls back to its parts ("multi-billionaire" → "billionaire"). A word the dictionary does not have is drawn as an outline before you press anything, so you can see there is nothing to look up. Pronunciation renders properly: Noto Sans now carries the phonetic alphabet, and a font that lacks a symbol draws a close lookalike instead of a row of boxes. Nothing is bundled — dictionaries are files you put on the card; see `docs/dictionary.md`.
- **Lists move faster.** Up and Down step one item; Left and Right now jump a whole screen and keep going while held, so crossing a few hundred chapters or files no longer means holding a button and waiting. Double-tapping Up or Down does the same jump on the screens whose Left/Right are already taken — the file browser, where Options has moved to a long press on Right and the long-press delete is gone (the context menu offers the same thing).
- Long titles in the file browser now wrap over up to three lines instead of being cut off. A folder holding one series used to show the same truncated text on every row, because the part that tells the books apart is the part that got cut.
- Fix: a double-tap on Up/Down registered as two single steps about nine times out of ten — a screen refresh between the two taps was enough to lose the gesture.
- Fix: on a list that fits on one screen, two quick taps threw the selection to the far end instead of moving two rows, and a third tap wrapped it back to the top.
- Fix: on the starred-books screens and the Wi-Fi list, one press did two things at once — it moved the selection *and* triggered the row's action (rename or delete, connect or rescan).
- Recent Books shows covers in a 2×2 grid instead of 3×3. At three columns a cover was drawn about 105×158 pixels, too small to recognise the artwork; the grid now works its size out from the panel, and covers are drawn at their stored size rather than scaled down, which also removes the faint grid pattern that rescaling left in them.
- Fix: a book whose cover is stored in a format the reader cannot decode (an AVIF named "cover.png", for instance) was unpacked from the book again on every pass through the shelf, about 1.3s each time — and a cover that failed also left the *next* book showing as coverless for the rest of the session. Scanning covers no longer writes a helper file to the card for every book it passes, either.
- Fix: opening Recent Books in cover mode could restart the device, as could a chapter whose cache was dropped before it was ever read.
- **Tables** (#186). Tables came out as loose text whenever a cell spanned some but not all of the columns — which is how Standard Ebooks writes row headers, so nearly every table in those books fell back. Three more table fixes came with it: a table that crosses a page break repeats its header row at the top of the next page, a `<caption>` no longer prints below the table it belongs to, and a table keeps its own indentation instead of always spanning the full width.
- Fix: tables could disappear from a chapter completely, and a chapter could be cut short and its shortened form cached, both because memory checks were set far above what the work behind them actually needs. The reader noticed the short chapter and quietly built it a second time, so this also removes duplicated work on table-heavy books.
- Fix: a KOReader or OPDS server address typed with a single slash ("https:/example.com") could never connect, and reported an unrelated redirect error after three timeouts — one slash and two are hard to tell apart on e-ink. Such an address is now repaired as you enter it, and one already saved is repaired at the next boot.
- Fix: the clock in the status bar overlapped the battery percentage when the battery read 100%.
- Fix: an X3 with a USB cable attached could produce no serial output at all for a whole session — the log you are asked for when something goes wrong. Plugging the cable in after boot now opens the log too, instead of needing a restart.
- Pages are built with about 7.5% fewer memory allocations, which leaves the heap less broken up over a long reading session.
- Upgrade notes: chapters are re-indexed once per book the first time you open them, because table cells now record how many columns they span — your place in every book is kept. If you install a dictionary, prefer a plain `.dict` over a `.dict.dz`: the compressed form needs a 32 KB unbroken block of memory that a long reading session cannot always spare (`dictunzip` on a computer converts it).

## 2.24 — 2026-08-20

- Fix: wide images were shown with a large black bar underneath, and other pictures came out stretched or squashed. Six separate problems in how images are sized and drawn are fixed, including a cover that looked right at the top and repeated a single line for the rest of the way down.
- Chapters full of pictures now open straight away. They used to spend up to a minute preparing every image in the chapter, including ones on pages you never reach.
- Fix: pictures could stop appearing for the rest of a book after a brief moment of low memory.
- Fix: some books would not open at all — every chapter failed. They open normally now.
- Fix: Cyrillic text was missing letters.
- Fix: "Normalize font size" had no effect on books that set their text size in a stylesheet. Those books stayed slightly off the size you chose and looked a little blurred. Headings still keep the size their publisher gave them.
- The first footnote in a book no longer sets off "Gathering footnotes" and then "Indexing". Notes are prepared one chapter at a time now, so jumping to a note and back is immediate, and books whose later chapters you never open no longer pay for them. Previews also read better — no stray link or heading text mixed in — and coming back from a note returns you to the paragraph you were reading rather than roughly the right page.
- The reader menu, table of contents, footnote list and bookmarks are about three times quicker to open and to move around in.
- Fixed two freezes that could only be cleared with the reset button: one after a long press — most often when starting a KOReader sync from inside a book — and one when opening the menu, contents, footnote list or bookmarks. Two unexpected restarts around footnotes are fixed as well.
- Fix: waking straight back into a book could draw the page on top of the sleep image instead of replacing it (X4).
- Fix: signing in to a KOReader server failed after the device had been fully switched off, with an error that gave no clue why.
- Fix: the clock on X4 lost time badly. Several ordinary actions — finishing a sync, leaving the web server, downloading a font, installing an update — restart the device quietly in the background, and each one set the clock back to when it last checked the time. It now keeps the time across those restarts.
- Wi-Fi now always connects to the strongest access point. It could previously pick a distant one when several share a network name, take six or seven seconds, or report "no access point found" and quietly try again. Connecting takes about three seconds and behaves the same way every time.
- Fix: on the newer X4 units with the updated screen, the display was upside down and quick page refreshes showed nothing at all.
- Reading statistics are only loaded when you open a screen that shows them, which leaves more memory free while you read.
- The firmware went from almost completely full to about 93% of the space available, mostly by storing the fonts more compactly. Nothing about the fonts themselves changed. This is what made room for the fixes above.
- Upgrade notes: chapters are re-indexed once the first time you open each book, because of the image and text-size fixes above — your place in every book is kept. Pictures are prepared again for the same reason, so the first look at an illustrated chapter is a little slower than the second. If your X4's clock has been badly wrong, it will put itself right the next time the device is online.

## 2.23 — 2026-08-16

- Fix: letters were unevenly thick within the same word (#149). The reader fonts were built without grid-fitting their stems, so an identical stem shipped as 3 pixels of ink in one letter and 4 in the next — at Bookerly 14 it split `b d f i n r u` from the rest of the alphabet. All 40 built-in reader faces (Bookerly and Noto Sans, 10-18pt, four styles each) and 24 of the 28 SD card font families are rebuilt. Letter *spacing* is untouched, so no book repaginates and no reading position moves.
- Waking from sleep is faster and less fussy. The boot no longer runs behind your finger — it used to stop and wait for the power button to be released, and since nothing on screen confirms the press was accepted, people held on, which is exactly what made it slow (measured on X4: 3.9s to a visible page instead of 6.8s). The hold needed to wake is also shorter, and a quick double-click now wakes the device, which it previously refused even though a double-click is what put it to sleep.
- Fix: reading could jump back to the start of the chapter (#147). The anchor a chapter rebuild resolves into was written once when the chapter was entered and then left frozen, so any mid-chapter rebuild — a background build aborting on low memory, a footnote gather, a failed page load — dropped you at the top of the chapter, and the next page render saved that position, so it survived a reboot. The anchor now follows the page you are actually on.
- Fix: a single failed allocation while turning a page deleted the chapter's cache and re-indexed the whole chapter. The cache file is almost always intact in that case; the reader now frees what nobody is waiting on and re-reads the same file, and only rebuilds if that fails too.
- Read-ahead and in-place chapter builds are admitted by a memory check that was asking for more than any build has ever needed, so it could never pass. The check is corrected; the safety margins themselves are unchanged.
- The SD card font "Readerly" is replaced by "Libron" from the same author under the same licence — Readerly was withdrawn upstream and could no longer be built. Its listing also claimed Cyrillic coverage it never had.
- Upgrade notes: the built-in fonts come with the firmware, but SD card fonts are files on your card — Settings → Fonts will now offer an update for every family you have installed, and the fix only applies once you take it. Readerly is no longer in the catalogue; an installed copy keeps working, but Libron is its replacement.

## 2.22 — 2026-08-14

- Background chapter build (read-ahead) now actually runs on device: its memory floors were unreachable — one by 12 bytes, another excluding every book with embedded CSS — correct builds were being discarded as "degraded", and background work ran at the 10 MHz idle clock. A section that took 28s to build now takes ~2s, the up-to-1.2s input freezes during a background slice are gone, and read-ahead waits 1.5s of stillness instead of 4s so it also works while skimming.
- Fewer restarts while reading: the fragmentation caused by a page render interleaving with a live chapter build is fixed (font page slots released, build arena adopted mid-build, arena-aware image decode floors).
- Added web-UI plugins loaded from the SD card (`/.crosspoint/plugins`), with four bundled: metadata-editor (edit title/author/series/description and manage covers, incl. Open Library / Goodreads / Google Books cover search), organize-by-author, find-duplicates and a reference plugin. Plugins run in the browser; a manifest declares which capabilities and remote hosts they may use.
- Added Calibre `book.opf` metadata sidecars: a `book.opf` beside a book overrides its embedded title, author, language, series and description without rewriting the EPUB.
- Added "Adaptive" as a fourth Sleep Screen Cover Filter, applied to BMP and full-screen PNG sleep images and to book covers, with line art detected and left alone.
- Inline images in books now render in grayscale (and get adaptive tone mapping) when anti-aliasing is on.
- KOReader sync: new "On Progress Conflict" setting ("Ask every time" / "Use furthest", changeable from the compare screen), automatic discovery of the document id the server actually uses (KOReader defaults to a content hash, we defaulted to filename — the two never saw each other), an optional sync when a book is finished, and a configurable auto-push page threshold.
- Idle light sleep: the chip now halts between button polls when idle, with unchanged button response. Light-sleep and deep-sleep behaviour is reported on the System Information screen.
- Faster boot (~0.45s off the recovery-combo check) and ~0.5s faster wake straight back into a book on X3.
- Sleep images are now decoded once and cached in a sidecar, instead of three or four times per sleep.
- Books with no real footnotes no longer pay a ~2.8s whole-book scan before the first page.
- Web file browser: images preview in-page instead of downloading, and dropping a folder now uploads its contents with the folder structure intact.
- Fix: an oversized table cell deleted itself and every earlier cell in the same table; tables now lay out row by row, which also removes the limits that made large tables abort a page build.
- Fix: the e-ink panel and the SD card share one SPI bus but only storage locked it, so an interleaved refresh and SD read could corrupt data or panic the system.
- Fix: a failed footnote gather recorded "this book has no footnotes" permanently, surviving reboots.
- Fix: finishing a book synced the start of the last chapter instead of the end.
- Fix: applying remote KOReader progress resolved the position correctly and then discarded it.
- Fix: KOSync now accepts any 2xx response from compatible servers.
- Fix: watchdog-triggered resets are now reported as crashes instead of rebooting silently.
- Fix: X3 reported USB connected and showed the charging icon when detached with a full battery.
- Fix: the web Settings page could stall or trip the watchdog by opening three simultaneous connections.
- Fix: replacing a book cover by hand had no effect, because the cached thumbnail was never invalidated.
- Fix: a finished book left its metadata sidecar behind when moved to /COMPLETED.
- Fix: sleep and wake button handling now agree with each other.
- Hardening: stored credentials (Wi-Fi, KOReader, OPDS) are decoded with a bounded buffer.
- Translations: the 13 strings that were still English-only (the new KOReader sync and sleep settings, the adaptive filter, "Normalize font size", NTP server and the new System Information rows) are now translated in all 15 fully-maintained languages — German, French, Spanish, Italian, Dutch, Portuguese (PT/BR), Polish, Russian, Ukrainian, Belarusian, Slovenian, Swedish, Turkish and Vietnamese.
- Upgrade notes: book covers are re-generated once (they are now cached as 8-bit greyscale so they can be tone-mapped), sleep images gain a small hidden `.pxc` cache file beside them, existing KOReader configurations keep "Ask every time" while new installs default to "Use furthest", a `book.opf` beside a book now takes precedence over the book's own metadata, and web plugins run unsandboxed on a web server that has no authentication — install only ones you trust.

## 2.21 - 2026-08-03
 - Recognition of newer X3 / X4 batches with alternative display controller
 - Implemented "Normalize font size" as a user setting that snaps publisher near-body font wrappers (e.g. around whole paragraphs) back to native 100%
 - Fix: The webserver settings screen could be mangled if the koreader password contained special character
 - Fix: Koreader server authorisation and registration could fail under heavy memory load
 - Fix: Font download could fail under heavy memory load
 - Fix: OTA update could fail under heavy memory load
 - Fix: The involuntary "wake on short power button press" has been resolved
 - Fix: Relative image sizing recognizes outer blocks

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
