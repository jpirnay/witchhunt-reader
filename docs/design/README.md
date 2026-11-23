# Design records

These are design records for shipped features, cited from code comments by path (and, for the list scheme, the CJK fonts, `cjk-ttf-fonts.md`, and CJK typography, `cjk-typography.md`, by section number). They describe why the code is shaped as it is; the code is the authority on what it does now.
Open work lives in `docs/future_work/`.

Code in `lib/EpdFont/ttf/` cites `cjk-ttf-fonts.md` as "spec N.N"; older comments there cite the TTF engine spec,
`docs/superpowers/specs/2026-10-13-ttf-fonts-design.md`.

The CJK layout code in `lib/Epub/Epub/` (`CjkPunctuation`, `TypographyProfile`, `ParsedText`) and its tests
(`test/cjk_layout/`, `test/typography/`, `test/epub_pipeline/CjkRulesTest.cpp`) cite `cjk-typography.md` as
"spec 5.N" (punctuation widths, push-in, the CJK–Latin gap, the grid); its section 11 records what was built, from the
line breaking underneath to ruby and emphasis dots. Their ruby comments ("spec 7.N") and the rule
checker's "spec section 9.1" cite the earlier CJK layout spec, `docs/superpowers/specs/2026-10-16-cjk-layout-design.md`
on branch `docs/cjk-layout-design` (commit `7e1136008`). Section 11.8 of `cjk-typography.md` records line spacing;
`cjk-ttf-fonts.md` also records the page glyph arena of dense CJK pages (#500, section 14) and how the CJK work is
tested on a device (section 15).

`lib/Epub/Epub/BookFonts.cpp` cites `book-fonts-probe.md` for the Publisher Fonts probe: which spine heads it reads,
how it ranks a book's families by their text, which faces it copies, and the first-open times behind its budget.

`writing-modes.md` is the record of right-to-left and vertical text (writing modes steps 0-5): the steps and their PRs,
page progression, reading a Hebrew book, what vertical text does and its limits, UI text in Hebrew, Greek, Arabic, the
Indic scripts and Burmese, how build speed was won, and the analysis behind one shaper for Arabic and the Indic scripts.
The shaping itself is the reference page `docs/text-shaping.md`.

`flash-area.md` is the record of the raw flash area (`lib/hal/FlashArea.h`): what keeps caches in it and how each uses
it, the one `NorFlash` interface they go through, and its backends, the `spiffs` partition and an image in PSRAM kept as
a file on the card for a board without that partition.

`reader-priority-scheme.md` is the record of the reader's priority scheme: the complete page first, then the page's own
pictures, then everything else, with input above all. Section 3 holds the rules R0-R8 (`InputFirst.h` cites R0,
`test/page_gray_during_build/BuildPageGrayTest.cpp` R2); section 4 what each panel controller reads from the host's
framebuffers and for how long (`BuildPageGray.h`, `IndexingPopupGate.h`). Sections 2, 5 and 7 hold the findings,
options and decisions behind #808-#823. `docs/background-rendering.md` describes the passes as they run.

`reader-bars.md` is the record of the reader bars (#747-#764): why they exist, their tabs and the Layout sheet, where
the sheet does not fit (the Murphy held sideways), the book laid out live under the bars with the C3s' deferred font
change and its heap figures, and the profiling that cut the wait for a layout change.

`stylesheet-scope.md` is the record of per-chapter stylesheet scope (#835, #843): the sheet table's order ranges and
import edges, the head scan, when every sheet still applies, what the scan does not see, and what stays book-wide.

`cache-cipher.md` is the record of D2, a protected book's cache enciphered at rest (`lib/CacheCipher`,
`lib/BookProtection/CacheAtRest`, the folder rule in `HalStorage`): why ChaCha20 as a keystream addressed by file
offset, the budgets and the X3 benchmark behind that choice, the one-sector header, the keys and nonces, how an older
cache is emptied, what stays plain by the owner's decision, and the chapter-build cost on the X3 and the Murphy. Code
cites it as "D2".

`integer-math.md` is the record of the firmware's move off floating point in six areas (#862-#868): why, the
fixed-point type each area chose (`ImageScale`, `TextScale`, `CssNumber`/`CssLength`, `ProgressPpb`, the clock's
drift model, `ByteSize`/`FixedDecimal`/`WeatherUnits`), the format bumps, what stays float, how
`scripts/float_inventory.py` measures and `scripts/check_no_float.py` guards it, and what no device has run.
