# Credits

If in doubt consider all the work being done here based on the work of others - especially crosspoint reader (as the ancestor of this version) and microreader have been a great source of inspiration.

## Project ancestry & inspiration
- **crosspoint-reader** by Dave Allie and others — the direct ancestor this firmware is forked from. https://github.com/crosspoint-reader/crosspoint-reader (MIT).
- **MicroReader** by CidVonHighwind — a source of inspiration, and still the most memory-efficient reader for the X4. https://github.com/CidVonHighwind/microreader
  - The **USB serial file transfer** feature ([`lib/SerialTransfer`](lib/SerialTransfer), `SerialTransferActivity`) is a clean-room, wire-compatible reimplementation of MicroReader's serial protocol — independently written from its `tools/serial_cmd.py`, with no firmware code copied — so MicroReader's host tools (its Calibre device plugin and `serial_cmd.py`) work against this firmware too. Full credit to CidVonHighwind for the original protocol and host tooling.
- **FreeInk SDK** - the shared X3/X4 hardware/display/utility libraries, included as a submodule. https://github.com/Free-Ink/freeink-sdk (to which we contributed our buffer memory management, the split display update cycle and some other goodies we previously had in our own sdk, see below)
- **CrossPoint XDK** (No longer used, but ancestry) — the shared X3/X4 hardware/display/utility libraries, included as a submodule. https://github.com/jpirnay/crosspoint-xdk (modified fork of https://github.com/crosspoint-reader/community-sdk ).

## Ported work

Code ported or adapted from other projects keeps a provenance line in the file that holds it, and its commit names the authors.

- **CrossInk** by Julia Nguyen ([uxjulia/CrossInk](https://github.com/uxjulia/CrossInk), MIT) — commits ported or adapted, each named in the source where it is used: 1d6b82f (batched web file lists), e092f81da (protected files on upload and WebDAV), 1d954ae34 (existing books kept during OPDS downloads), 234675e5c (first OPDS author), 56a29f618 (oversized KOReader authentication responses), 7d49b69fa (colours in extended BMP files), 61c8d78f (a table's own CSS box), 60cc4da5 (the icon-tab reader menu), 1babd9da (dictionary), 52e9fc49 (spine sizes), 902e6573 (battery percentage), 20d74368 (Unicode spaces), 8bd0001a (XML prefixes), 53aa4f39 (bookmark files), e7760b58 (reading sessions), and 59b53529 (a CJK line-break rule, by way of crosspoint-reader #3883).
- **Somnius/CrossInk-Indic** by Lef (MIT) — the Myanmar shaper and its categories, commits dbfb96a24 and 7d0c7c87, reworked in [`lib/OtShaper`](lib/OtShaper).

### crosspoint-reader pull requests

The pull requests of [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) that code here is ported from, as the source files name them (titles and authors from GitHub, 2026-11-10; "closed, not merged" and "open" are pull requests ported before or without upstream merging them).

| PR | Title | Author | Upstream |
| --- | --- | --- | --- |
| [#1842](https://github.com/crosspoint-reader/crosspoint-reader/pull/1842) | feat: home cover settings | @pablohc | closed, not merged |
| [#2053](https://github.com/crosspoint-reader/crosspoint-reader/pull/2053) | fix: harden web file operations | @sabraman | open |
| [#2189](https://github.com/crosspoint-reader/crosspoint-reader/pull/2189) | feat: auto-connect saved Wi-Fi networks | @axhoff | merged |
| [#2230](https://github.com/crosspoint-reader/crosspoint-reader/pull/2230) | fix: Replace full-image cache buffer with streaming band buffer to reduce memory usage | @itsthisjustin | merged |
| [#2288](https://github.com/crosspoint-reader/crosspoint-reader/pull/2288) | feat: Support for Korean line breaks and glyph spacing | @itsthisjustin | merged |
| [#2326](https://github.com/crosspoint-reader/crosspoint-reader/pull/2326) | fix: suppress sub-pixel font-size watermark spans during EPUB parsing | @mchuck | open |
| [#2335](https://github.com/crosspoint-reader/crosspoint-reader/pull/2335) | fix: Switch XTC cover BMP output from 1-bit to 2-bit grayscale | @itsthisjustin | merged |
| [#2358](https://github.com/crosspoint-reader/crosspoint-reader/pull/2358) | feat: Selection Popup | @Uri-Tauber | merged |
| [#2360](https://github.com/crosspoint-reader/crosspoint-reader/pull/2360) | feat: Hidden wifi ssid support | @HgGamer | merged |
| [#2385](https://github.com/crosspoint-reader/crosspoint-reader/pull/2385) | fix: prevent out-of-memory crash when opening directories with many files | @10ne1 | open |
| [#2386](https://github.com/crosspoint-reader/crosspoint-reader/pull/2386) | fix: render EPUB images with missing or wrong file extension | @artwem | open |
| [#2452](https://github.com/crosspoint-reader/crosspoint-reader/pull/2452) | feat: Lazy incremental EPUB section indexing | @itsthisjustin | merged |
| [#2475](https://github.com/crosspoint-reader/crosspoint-reader/pull/2475) | fix: oom exceptions for OPDS, KOSync, and OTA via wolfssl | @itsthisjustin | merged |
| [#2525](https://github.com/crosspoint-reader/crosspoint-reader/pull/2525) | perf: light-sleep idle + refresh downclock — ~3.2x active reading time (X3-measured) | @BrianPugh | merged |
| [#2541](https://github.com/crosspoint-reader/crosspoint-reader/pull/2541) | feat: Arabic/Farsi/Urdu bidi reordering and contextual shaping — PR 1/3 | @YouHusam | merged |
| [#2547](https://github.com/crosspoint-reader/crosspoint-reader/pull/2547) | fix: Flatten TextBlock word storage into single allocation | @itsthisjustin | merged |
| [#2583](https://github.com/crosspoint-reader/crosspoint-reader/pull/2583) | feat: Slim dictionary | @Uri-Tauber | merged |
| [#2603](https://github.com/crosspoint-reader/crosspoint-reader/pull/2603) | fix: fall back to NCX when EPUB 3 nav TOC is sparse | @Cabalist | open |
| [#2604](https://github.com/crosspoint-reader/crosspoint-reader/pull/2604) | fix(css-parser): don't save unusable rules to RAM | @brianhuster | merged |
| [#2652](https://github.com/crosspoint-reader/crosspoint-reader/pull/2652) | fix(epub): preserve word continuation when splitting CJK text on MAX_WORD_SIZE | @brianhuster | merged |
| [#2665](https://github.com/crosspoint-reader/crosspoint-reader/pull/2665) | feat(epub): native <ruby> support for Chinese, Japanese | @brianhuster | merged |
| [#2683](https://github.com/crosspoint-reader/crosspoint-reader/pull/2683) | fix: restore antialiasing after manual refresh | @thiagokokada | merged |
| [#2686](https://github.com/crosspoint-reader/crosspoint-reader/pull/2686) | fix: empty panic reason on raw faults + garbled Last logs: from unsynchronized ring buffer | @DarkStarDS9 | open |
| [#2706](https://github.com/crosspoint-reader/crosspoint-reader/pull/2706) | fix: name dictionary lookup failures (low-memory vs decompress vs read) instead of 'Not found' | @W-Floyd | merged |
| [#2734](https://github.com/crosspoint-reader/crosspoint-reader/pull/2734) | feat: Add browser-side plugin system with SD card support | @itsthisjustin | closed, not merged |
| [#2735](https://github.com/crosspoint-reader/crosspoint-reader/pull/2735) | perf: skip ruby scratch allocations on ruby-less lines | @W-Floyd | merged |
| [#2768](https://github.com/crosspoint-reader/crosspoint-reader/pull/2768) | fix: restore spaces between CJK words | @Uri-Tauber | merged |
| [#2772](https://github.com/crosspoint-reader/crosspoint-reader/pull/2772) | perf: drop all-empty ruby vectors from TextBlock | @Uri-Tauber | merged |
| [#2791](https://github.com/crosspoint-reader/crosspoint-reader/pull/2791) | fix: dictionary lookup OOM on .dict.dz definitions | @W-Floyd | merged |
| [#2836](https://github.com/crosspoint-reader/crosspoint-reader/pull/2836) | feat: render HTML dictionary definitions via EPUB engine | @Uri-Tauber | merged |
| [#2858](https://github.com/crosspoint-reader/crosspoint-reader/pull/2858) | feat(keyboard): user-selectable layout set with a language key | @winst0niuss | merged |
| [#2861](https://github.com/crosspoint-reader/crosspoint-reader/pull/2861) | feat: add adaptive tone mapping for custom sleep images | @Sichroteph | open |
| [#2880](https://github.com/crosspoint-reader/crosspoint-reader/pull/2880) | feat: guard against cross-chip firmware installs | @Uri-Tauber | merged |
| [#2892](https://github.com/crosspoint-reader/crosspoint-reader/pull/2892) | fix: hyphenate focus-split words and break after visible hyphens | @Uri-Tauber | merged |
| [#2957](https://github.com/crosspoint-reader/crosspoint-reader/pull/2957) | feat: convert all lists and tabs to FUI | @itsthisjustin | merged |
| [#2983](https://github.com/crosspoint-reader/crosspoint-reader/pull/2983) | feat: Add support for x4pro & papermono devices | @itsthisjustin | merged |
| [#2987](https://github.com/crosspoint-reader/crosspoint-reader/pull/2987) | fix(epub): resolve contents entries whose href only matches by filename | @jetaudio | open |
| [#2998](https://github.com/crosspoint-reader/crosspoint-reader/pull/2998) | fix: Guard GPIO13 power control for Xteink C3 boards only | @itsthisjustin | merged |
| [#3027](https://github.com/crosspoint-reader/crosspoint-reader/pull/3027) | fix: stop long chapters aborting on the build tables' growth | @s4y | closed, not merged |
| [#3036](https://github.com/crosspoint-reader/crosspoint-reader/pull/3036) | fix: render NFD Hangul filenames from macOS transfers | @serialx | merged |
| [#3083](https://github.com/crosspoint-reader/crosspoint-reader/pull/3083) | feat: compress built-in fonts with per-glyph GlyphStream codec (frees 556 KB flash) | @serialx | closed, not merged |
| [#3123](https://github.com/crosspoint-reader/crosspoint-reader/pull/3123) | fix: match capitalized umlaut Ä/Ö/Ü and ẞ words in dictionary lookup | @sorig | open |
| [#3126](https://github.com/crosspoint-reader/crosspoint-reader/pull/3126) | fix: keep the glyph arena usable under heap pressure | @serialx | merged |
| [#3134](https://github.com/crosspoint-reader/crosspoint-reader/pull/3134) | fix(epub): ignore trailing non-XML data after </html> tag in ChapterHtmlSlimParser | @bjesus | merged |
| [#3203](https://github.com/crosspoint-reader/crosspoint-reader/pull/3203) | feat: add usb mass storage mode for x4 pro | @uxjulia | merged |
| [#3215](https://github.com/crosspoint-reader/crosspoint-reader/pull/3215) | fix(x4pro): hold power-latch pins HIGH through deep sleep | @Belphemur | merged |
| [#3219](https://github.com/crosspoint-reader/crosspoint-reader/pull/3219) | fix(kosync): advertise percentage progress support | @mehalter | merged |
| [#3221](https://github.com/crosspoint-reader/crosspoint-reader/pull/3221) | fix(epub): some problems when extra paragraph spacing is off | @brianhuster | merged |
| [#3233](https://github.com/crosspoint-reader/crosspoint-reader/pull/3233) | fix(KOSync): keep WiFi awake during KOReader authentication and sync | @jadehawk | merged |
| [#3235](https://github.com/crosspoint-reader/crosspoint-reader/pull/3235) | fix: show full IP address on Calibre screen | @fdkevin0 | merged |
| [#3244](https://github.com/crosspoint-reader/crosspoint-reader/pull/3244) | fix: prevent size_t underflow on ZipFile read errors | @sfoulad | merged |
| [#3252](https://github.com/crosspoint-reader/crosspoint-reader/pull/3252) | fix: disable WiFi power-save during OPDS/book HTTP downloads | @sfoulad | merged |
| [#3253](https://github.com/crosspoint-reader/crosspoint-reader/pull/3253) | fix: strip Authorization header on cross-origin HTTP redirect | @sfoulad | closed, not merged |
| [#3317](https://github.com/crosspoint-reader/crosspoint-reader/pull/3317) | fix: release dictionary font caches on exit | @Uri-Tauber | merged |
| [#3319](https://github.com/crosspoint-reader/crosspoint-reader/pull/3319) | fix: Decouple reader menu gestures from touch controls | @adiskill | merged |
| [#3353](https://github.com/crosspoint-reader/crosspoint-reader/pull/3353) | fix(webserver): normalize every user-supplied path and escape file names in the files page | @s0lness | merged |
| [#3354](https://github.com/crosspoint-reader/crosspoint-reader/pull/3354) | fix: keep reading position and bookmarks when a book is renamed or moved | @s0lness | open |
| [#3355](https://github.com/crosspoint-reader/crosspoint-reader/pull/3355) | fix: restore superscript styling on footnote links | @uxjulia | merged |
| [#3361](https://github.com/crosspoint-reader/crosspoint-reader/pull/3361) | fix(reader): turn dictionary pages with short power-button press | @aminalaee | open |
| [#3390](https://github.com/crosspoint-reader/crosspoint-reader/pull/3390) | fix: don't display elements with hidden HTML attribute | @jjharpham | merged |
| [#3425](https://github.com/crosspoint-reader/crosspoint-reader/pull/3425) | fix(kosync): reject failed and incomplete document-ID reads | @Daviex | open |
| [#3438](https://github.com/crosspoint-reader/crosspoint-reader/pull/3438) | fix: cap serial read timeout so a stray byte can't stall the loop | @suhsoohong | open |
| [#3469](https://github.com/crosspoint-reader/crosspoint-reader/pull/3469) | fix: vertical banding on X3 grayscale images | @bos | closed, not merged |
| [#3480](https://github.com/crosspoint-reader/crosspoint-reader/pull/3480) | fix: dictionary lookup prefers exact case match | @YurBoiRene | closed, not merged |
| [#3539](https://github.com/crosspoint-reader/crosspoint-reader/pull/3539) | fix: follow an SVG wrapper named as the cover image | @osakanataro | open |
| [#3562](https://github.com/crosspoint-reader/crosspoint-reader/pull/3562) | feat: Replace UTC offset with timezone and DST settings | @itsthisjustin | merged |
| [#3611](https://github.com/crosspoint-reader/crosspoint-reader/pull/3611) | fix: improve selected-word cleanup and case-insensitive dictionary lookup | @alycklama | open |
| [#3652](https://github.com/crosspoint-reader/crosspoint-reader/pull/3652) | perf: avoid main-loop contention during EPUB rendering | @serialx | merged |
| [#3698](https://github.com/crosspoint-reader/crosspoint-reader/pull/3698) | perf: remove ButtonNavigator allocation churn | @serialx | merged |
| [#3700](https://github.com/crosspoint-reader/crosspoint-reader/pull/3700) | fix: justify Korean text by word spaces only | @serialx | merged |
| [#3706](https://github.com/crosspoint-reader/crosspoint-reader/pull/3706) | feat: hyphenation manager | @Uri-Tauber | open |
| [#3730](https://github.com/crosspoint-reader/crosspoint-reader/pull/3730) | fix: load the X3 battery capacity into its BQ27220 fuel gauge | @martinqnguyen | merged |
| [#3732](https://github.com/crosspoint-reader/crosspoint-reader/pull/3732) | fix: let Back leave File Transfer during a slow HTTP upload | @martinqnguyen | merged |
| [#3737](https://github.com/crosspoint-reader/crosspoint-reader/pull/3737) | fix: stop dropping log lines at the low-power clock | @serialx | merged |
| [#3764](https://github.com/crosspoint-reader/crosspoint-reader/pull/3764) | fix: reopen on the viewed page after following a link | @suhsoohong | merged |
| [#3787](https://github.com/crosspoint-reader/crosspoint-reader/pull/3787) | feat: add Indic language support without HarfBuzz | @ssafayet | open |
| [#3799](https://github.com/crosspoint-reader/crosspoint-reader/pull/3799) | fix: resolve EPUB footnotes and preserve progress | @skhrv | open |
| [#3804](https://github.com/crosspoint-reader/crosspoint-reader/pull/3804) | feat(kosync): add server profiles and extended metadata | @jadehawk | merged |
| [#3806](https://github.com/crosspoint-reader/crosspoint-reader/pull/3806) | fix: take the XTC bit depth from its pages | @T-Damer | open |
| [#3834](https://github.com/crosspoint-reader/crosspoint-reader/pull/3834) | fix(bidiutils): canonicalize reversed tuple in emission path | @Uri-Tauber | merged |
| [#3838](https://github.com/crosspoint-reader/crosspoint-reader/pull/3838) | fix: keep SD-font kerning and ligatures in edge cases | @serialx | merged |
| [#3863](https://github.com/crosspoint-reader/crosspoint-reader/pull/3863) | fix: stable tip positions in keyboard activity | @Uri-Tauber | merged |
| [#3875](https://github.com/crosspoint-reader/crosspoint-reader/pull/3875) | fix: preserve paragraph continuity across soft flushes | @brianhuster | merged |
| [#3883](https://github.com/crosspoint-reader/crosspoint-reader/pull/3883) | fix(layout): allow breaking line after hyphens, dashes | @brianhuster | merged |
| [#3891](https://github.com/crosspoint-reader/crosspoint-reader/pull/3891) | feat: Improve epub rendering | @itsthisjustin | merged |
| [#3934](https://github.com/crosspoint-reader/crosspoint-reader/pull/3934) | feat: Add support for embedded @font-face fonts in EPUBs | @itsthisjustin | open |
| [#3979](https://github.com/crosspoint-reader/crosspoint-reader/pull/3979) | fix: preserve reader data and correct cache/catalog handling | @phucisstupid | closed, not merged |
| [#3980](https://github.com/crosspoint-reader/crosspoint-reader/pull/3980) | fix(dict): show all matching `.syn` definitions instead of stopping at 1st one | @brianhuster | merged |
| [#3988](https://github.com/crosspoint-reader/crosspoint-reader/pull/3988) | perf(dictionary): reuses the definition buffer | @Uri-Tauber | merged |
| [#3994](https://github.com/crosspoint-reader/crosspoint-reader/pull/3994) | fix: add sleep screen confirmation | @Uri-Tauber | merged |
| [#4076](https://github.com/crosspoint-reader/crosspoint-reader/pull/4076) | fix: avoid restoring stale home cover pixels | @lpla | merged |
| [#4078](https://github.com/crosspoint-reader/crosspoint-reader/pull/4078) | fix: keep the tilt setting when the IMU probe fails | @martinqnguyen | merged |
| [#4079](https://github.com/crosspoint-reader/crosspoint-reader/pull/4079) | fix: truncate the Wi-Fi SSID by width, not bytes | @martinqnguyen | merged |
| [#4091](https://github.com/crosspoint-reader/crosspoint-reader/pull/4091) | fix: draw corrupt .cpfont glyph entries as missing | @eszter007 | merged |
| [#4106](https://github.com/crosspoint-reader/crosspoint-reader/pull/4106) | fix: preserve UTF-8 when masking passwords | @lpla | merged |
| [#4110](https://github.com/crosspoint-reader/crosspoint-reader/pull/4110) | fix(dictionary): support multiple idx entries with same string; fix regression with recursive lookup after merging one-syn-to-multi-idx fix | @brianhuster | merged |
| [#4142](https://github.com/crosspoint-reader/crosspoint-reader/pull/4142) | fix: corrects spacing in justified EPUB text | @Uri-Tauber | merged |
| [#4211](https://github.com/crosspoint-reader/crosspoint-reader/pull/4211) | fix: open the hotspot captive portal on phones | @osteotek | merged |
| [#4253](https://github.com/crosspoint-reader/crosspoint-reader/pull/4253) | fix: restore font browser navigation and retry | @lpla | merged |
| [#4270](https://github.com/crosspoint-reader/crosspoint-reader/pull/4270) | fix: decode inline dict type | @Uri-Tauber | merged |
| [#4285](https://github.com/crosspoint-reader/crosspoint-reader/pull/4285) | fix: keep the web server responsive and within memory while plugins load | @osteotek | merged |
| [#4289](https://github.com/crosspoint-reader/crosspoint-reader/pull/4289) | perf: stream plugin relay replies instead of buffering them | @osteotek | merged |

## Vendored third-party components (`lib/`)
These are bundled directly in the repository. Each retains its upstream copyright header in source.

- **TJpgDec — Tiny JPEG Decompressor** by ChaN (R0.03) — baseline-JPEG decode engine for the EPUB image path. http://elm-chan.org/fsw/tjpgd/ — Copyright (C) 2021 ChaN, BSD-1-Clause. Vendored under [`lib/TJpgDec`](lib/TJpgDec); modified from upstream in `tjpgdcnf.h` (config + the `JD_FASTPATH` IRAM macro) and `tjpgd.c` (the `JD_FASTPATH` annotations on the hot decode functions, plus a `BYTECLIP` clamp on the grayscale output path fixing an upstream wrap-around bug — black speckle in high-contrast covers when `JD_FASTDECODE≥1`); `tjpgd.h` is verbatim.
- **minibidi** by Ahmad Khalifa and Thomas Wolff (from mintty) — the Unicode bidirectional algorithm, for right-to-left text. https://github.com/mintty/mintty — MIT. Vendored under [`lib/MiniBidi`](lib/MiniBidi) by way of crosspoint-reader's copy (MIT, @ e8299b5f); modified: no Arabic shaping in `applyBidiVisual` (`applyBidiVisualShaped` keeps it), the divine-name match moved to [`lib/TextShaping`](lib/TextShaping), a releasable workspace, bracket mirroring done before reordering (UAX #9 L4), and a second class table (`bidiclasses_ext.t`) generated by `scripts/gen_bidi_classes_ext.py` from Unicode 17.0.0.
- **OtShaper** by SSafayet (crosspoint-reader #3787, MIT) — a port of HarfBuzz 14.5.0's Indic and Universal Shaping Engine shapers and of its GSUB/GPOS engine; it does the OpenType shaping of the ten Indic scripts and, with the Arabic shaper ported from HarfBuzz 14.5.0's `hb-ot-shaper-arabic.cc` (also "Old MIT"), of Arabic, Persian and Urdu. https://github.com/crosspoint-reader/crosspoint-reader/pull/3787 — HarfBuzz's licence is "Old MIT", [`lib/OtShaper/COPYING`](lib/OtShaper/COPYING). Vendored under [`lib/OtShaper`](lib/OtShaper) at `dd45ba61` (the shaper's files as at `0fbd1575`), with `lib/Utf8/IndicScripts.h` and `IndicReorder`; modified: nothrow fixed-capacity storage (`OtFixedVector.h`) in place of `std::vector`, the design's deviation 1; a bounded `indicReorderForDisplay` that allocates nothing (deviation 9); and a plan that keeps its planned lookups' digests and remembers consonant positions (`Plan::mayApply`, `Plan::consonantPosition`, deviation 13). The test fonts are Noto Sans layout subsets (OFL, [`test/complex_shaper/data/OFL.txt`](test/complex_shaper/data/OFL.txt)), subsets of Noto Sans Devanagari, and the shaped-book subsets of Noto Sans Devanagari, Bengali, Tamil and Malayalam (OFL, [`test/fixtures/fonts/README.md`](test/fixtures/fonts/README.md)). From writing modes step 2 PR 2 on, the firmware links the engine's face, table and coverage code for the install's `CPac` (`OtFilterWriter`, ours, a port of #3787's `lookup_filters()`). From PR 3 on, it shapes a book's Indic words at section build ([`docs/text-shaping.md`](docs/text-shaping.md)): the glue, [`lib/TextShaping/IndicShaper.cpp`](lib/TextShaping/IndicShaper.cpp) (ours), ports #3787's `scanRun`, the piecewise loop of `appendShapedRun` and the glyph rounding of `appendGlyphs` (`lib/EpdFont/ComplexShaper.cpp` at `0fbd1575`), and three of #3787's rules are ported lines: the Hyphenator's fallback breaks only between syllables (`appendFallbackIndexes`), no focus-reading split of an Indic word (`ParsedText::applyBionicReadingTransform`), and no drop cap captured from an Indic letter (`ChapterHtmlSlimParser::consumeText`, from `f0757278`).
- **yxml** by Yoran Heling — the XML/HTML SAX parser backend (`SaxParser`), used by the EPUB and OPDS parsers. https://dev.yorhel.nl/yxml — Copyright (c) 2013-2014 Yoran Heling, MIT. Vendored under [`lib/SaxParser`](lib/SaxParser).
- **uzlib** by Joergen Ibsen and Paul Sokolovsky — tiny DEFLATE/inflate, used for ZIP/EPUB extraction and PNG inflate. https://github.com/pfalcon/uzlib — Copyright (c) 2003 Joergen Ibsen, (c) 2014-2018 Paul Sokolovsky, zlib license. Vendored under [`lib/uzlib`](lib/uzlib).
- **QR-Code-generator (qrcodegen)** by Project Nayuki — QR code generation. https://github.com/nayuki/QR-Code-generator — Copyright (c) Project Nayuki, MIT. Vendored under [`lib/QRCode`](lib/QRCode).
- **FreeType's anti-aliasing rasterizer (`ftgrays.c`, FreeType 2.13.3)** by David Turner, Robert Wilhelm and Werner Lemberg — draws the glyph outlines of TrueType and CFF (`.ttf`, `.otf`) fonts on the device. https://freetype.org — Portions of this software are copyright © 2024 The FreeType Project (www.freetype.org). All rights reserved. FreeType License (FTL, [`FTL.TXT`](lib/EpdFont/ttf/third_party/freetype/FTL.TXT)). Vendored under [`lib/EpdFont/ttf/third_party/freetype`](lib/EpdFont/ttf/third_party/freetype) with six patches listed in its `PATCHES.md`, regenerated by `scripts/vendor_ftgrays.py`. Only the rasterizer is used; the TrueType and CFF loaders around it are this project's own.

## External libraries (PlatformIO `lib_deps`)
Pulled from the PlatformIO registry at build time.

- **ArduinoJson** by Benoît Blanchon — JSON parsing/serialization. https://github.com/bblanchon/ArduinoJson — MIT.
- **arduinoWebSockets** by Markus Sattler — WebSocket client (KOReader sync). https://github.com/Links2004/arduinoWebSockets — LGPL-2.1.

## Fonts in the built-in faces (lib/EpdFont/builtinFonts)

- **Inter** by Rasmus Andersson — the menu (UI) faces. https://rsms.me/inter — SIL Open Font License 1.1, [`OFL.txt`](lib/EpdFont/builtinFonts/source/Inter/OFL.txt).
- **Noto Sans** (the Noto Project) — a built-in reading font and the small UI faces. SIL Open Font License 1.1, [`OFL.txt`](lib/EpdFont/builtinFonts/source/NotoSans/OFL.txt).
- **Noto Sans CJK** (Adobe) — the characters of the Chinese, Japanese and Korean translations, as the CJK UI faces (`cjk_ui_*`, built by `lib/EpdFont/scripts/build_cjk_ui_fonts.py`). https://github.com/notofonts/noto-cjk — SIL Open Font License 1.1, [`assets/sd-fonts/OFL.txt`](assets/sd-fonts/OFL.txt).
- **Noto Sans Hebrew** (the Noto Project) — its letters and points, cut into every built-in UI face from the Font Manager's NotoSansHebrew family below.

## Fonts the Font Manager downloads (assets/sd-fonts)

- **Noto Sans CJK and Noto Serif CJK** (Adobe) — subset per language into NotoSansJP … NotoSerifKR by `lib/EpdFont/scripts/cjk_subset.py`, with U+FFFD from Noto Sans. https://github.com/notofonts/noto-cjk — SIL Open Font License 1.1, [`assets/sd-fonts/OFL.txt`](assets/sd-fonts/OFL.txt).
- **Noto Sans and Noto Serif of the ten Indic scripts** (Devanagari, Bengali, Gurmukhi, Gujarati, Oriya, Tamil, Telugu, Kannada, Malayalam, Sinhala; the Noto Project) — each merged with Noto Sans or Noto Serif's Latin by `lib/EpdFont/scripts/merge_noto_latin.py` into NotoSansDevanagari … NotoSerifSinhala, the Font Manager's Indic families. https://github.com/notofonts — SIL Open Font License 1.1, [`assets/sd-fonts/OFL.txt`](assets/sd-fonts/OFL.txt).
- **Noto Naskh Arabic and Noto Sans Arabic** (the Noto Project) — merged with Noto Serif's or Noto Sans's Latin by `lib/EpdFont/scripts/merge_noto_latin.py` into NotoNaskhArabic and NotoSansArabic, the Font Manager's Arabic families. https://github.com/notofonts/arabic — SIL Open Font License 1.1, [`assets/sd-fonts/OFL.txt`](assets/sd-fonts/OFL.txt).
- **Noto Sans Hebrew and Noto Serif Hebrew** (the Noto Project) — merged with Noto Sans's or Noto Serif's Latin by `lib/EpdFont/scripts/merge_noto_latin.py` into NotoSansHebrew and NotoSerifHebrew, the Font Manager's Hebrew families. https://github.com/notofonts/hebrew — SIL Open Font License 1.1, [`assets/sd-fonts/OFL.txt`](assets/sd-fonts/OFL.txt).
- **Noto Sans Myanmar and Noto Serif Myanmar** (the Noto Project) — merged with Noto Sans's or Noto Serif's Latin by `lib/EpdFont/scripts/merge_noto_latin.py` into NotoSansMyanmar and NotoSerifMyanmar, the Font Manager's Myanmar families. https://github.com/notofonts/myanmar — SIL Open Font License 1.1, [`assets/sd-fonts/OFL.txt`](assets/sd-fonts/OFL.txt).

## Hyphenation patterns the Font Manager downloads (assets/sd-fonts/Hyphenation-*)

- **Typst hypher** by Laurenz Mädje and contributors — the tries of every pack but Hungarian's, compiled by hypher (at `d81cc50`) from the [hyph-utf8](https://github.com/hyphenation/tex-hyphen) TeX patterns, each pattern set under its own licence as hyph-utf8 lists it. https://github.com/typst/hypher — MIT OR Apache-2.0. [`scripts/hyphenation_trie.py`](scripts/hyphenation_trie.py) is a port of hypher's trie builder (`tests/generate.rs`), which rebuilds hypher's tries byte for byte.
- **Huhyphn v20031107** by NAGY Bence — the Hungarian patterns, from CTAN, kept as published in [`assets/hyphenation-patterns/hu`](assets/hyphenation-patterns/hu/README.md) and built into `Hyphenation-Hungarian/hyph-hu.cphyph` by `scripts/build_hyphenation_packs.py`. Copyright (C) 2003 NAGY Bence, GNU GPL version 2 ([`COPYING`](assets/hyphenation-patterns/hu/COPYING)); the pack, built from them, is under the same licence. Neither is compiled into the firmware.
