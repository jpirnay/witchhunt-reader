# Line spacing: a CJK scale, two wider steps, the book's own, per book

Status: decided by the maintainer on 2026-10-26 (section 2). Roadmap: `docs/future_work/cjk.md`, step 10.

## 1. Why

- **Today** the reader has one global setting, Tight, Normal or Wide, which scales the font's own line height by
  0.95, 1.0 or 1.1 (0.90 to 1.0 for the built-in Noto Sans). Noto CJK's own line height is about 1.45 em, so a CJK
  book gets 1.45 em at Normal and 1.59 em at Wide: Normal sits below the range the CJK layout requirements give.
- **The requirements.** JLREQ and CLREQ both set the line gap at half to one character (a pitch of 1.5 to 2.0 em), in
  both writing modes, Simplified and Traditional alike; short lines toward half, long lines toward one, and not beyond
  one except for heavy annotation. KLREQ gives no figure; its example is 160%, which is Hancom Hangul's default. Adobe's
  automatic leading for CJK frames is 175%. Upstream added "Extra wide" for CJK readers (crosspoint-reader #3037).
- **Books.** Commercial EBPAJ books declare `line-height: 1.75`; calibre conversions carry calibre's generic 1.2;
  many declare nothing. Writing-modes step 0 (#560) made the book's value mean what CSS says (a multiple of the font
  size) and kept it through the rule cache.
- **The study** (X3, Noto Sans CJK 14 pt: em 29 px, natural line 42 px): the same Japanese, Chinese and Korean pages
  at today's Normal and Wide and at 1.5, 1.7 and 2.0 em. 1.7 em costs about 15 % more pages than today's Normal, 2.0
  em about 35 %.
- **Gaps in the code.** The step-to-factor mapping is written three times (`CrossPointSettings::
  getReaderLineCompression`, `EpubReaderActivity::getEffectiveReaderLineCompression`, and a lambda in
  `EpubReaderActivity::drawCurrentPageToBuffer`). There is no per-book override, though most reader settings have one (eleven).

## 2. Decisions (maintainer, 2026-10-26)

1. **One scale for Chinese, Japanese and Korean books**, as a line pitch in ems of the body text:

   | Step | Tight | Normal | Wide | Extra wide | Widest |
   |---|---|---|---|---|---|
   | CJK books | 1.5 em | 1.7 em | 1.85 em | 2.0 em | 2.2 em |
   | Other books, factor on the font's line height | 0.95 | 1.0 | 1.1 | 1.2 | 1.3 |

   Tight to Extra wide stay within the requirements' range; Widest (2.2 em) is an accessibility step beyond it. For the
   built-in Noto Sans, whose own line height is taller, other books keep its lower factors and continue them: 0.90,
   0.95, 1.0, 1.1, 1.2 (upstream gave its Extra wide 1.05, too close to Wide's 1.0 to read as a step of its own). No
   existing Latin layout moves.
2. **Book's style**, a sixth choice: each block's pitch is the book's `line-height` where the book declares one,
   never tighter than Tight; a block without one gets Normal.
3. **A per-book override** in Quick overrides.
4. **One shared mapping** in place of the three copies.
5. Not in this round: the TXT and Markdown readers, which keep the font's line height.

## 3. Design

### 3.1 The steps and the setting

`CrossPointSettings::LINE_COMPRESSION` gains `EXTRA_WIDE = 3`, `WIDEST = 4` and `FROM_BOOK = 5`; 0 to 2 keep their
meaning, so saved settings and upstream's numbering (`EXTRA_WIDE = 3`) carry over. The setting's labels are Tight,
Normal, Wide, Extra wide, Widest and Book's style (`STR_EXTRA_WIDE` and `STR_WIDEST` are new; `STR_BOOK_S_STYLE`
exists for paragraph alignment). The per-book override takes the same values, or Default.

### 3.2 One mapping

`LineSpacing` (`lib/Epub/Epub/LineSpacing.h`), a pure function the reader, the sleep screen's page draw and the
dictionary all call:

```cpp
enum class LineSpacingScale : uint8_t { Font, BuiltinNotoSans, CjkPitch };
struct LineSpacing {
  float compression = 1.0f;  // pitch / the body font's line height, for every line the book does not set
  float bookFloor = 0.0f;    // Book's style: no block tighter than this compression; 0 = the book's value is not read
  uint16_t emPx = 0;         // the body text's em in pixels, which a book's line-height multiplies
};
LineSpacing lineSpacingFor(uint8_t step, LineSpacingScale scale, uint8_t pointSize, int fontLineHeightPx);
```

- **CJK books** (`TypographyProfile::cjkRepertoire`): `compression = pitchEm(step) × emPx / fontLineHeightPx`, so the
  pitch holds whatever font draws the book, a CJK face or a Latin one with CJK fallback.
- **Other books:** the factors of 2.1, by `Font` or `BuiltinNotoSans`.
- **Book's style:** `compression` is Normal's, `bookFloor` Tight's.
- **The em** is the reader's point size × 150 / 72: every font kind is converted or rasterised at 150 dpi
  (`fontconvert.py` `set_char_size(…, 150, 150)`, `ttf::ppemFromPoints`). At 14 pt that is 29 px, the advance of 一
  measured on the X3.
- `LineSpacing.h` holds its own step numbering; `CrossPointSettings` asserts that its enum matches.
- The dictionary's definition pages have no book: they take the other-books scale, and Book's style reads as Normal.

### 3.3 Book's style in the layout

- **Inheritance.** `line-height` is inherited in CSS, and books set it on `html`, `body` or a wrapper (the EBPAJ
  template on `html`, calibre on `.calibre`, its `body` class). The parser's flow stack already carries the inherited
  writing mode and direction, trimmed by depth at every start tag; its entry gains `lineHeightEm` and is pushed when
  any of the three changes. After `enterFlow`, the element's `cssStyle` carries the computed value, so every
  `BlockStyle::fromCssStyle` call (paragraphs, headings, block spans, tables) copies it into `BlockStyle::lineHeightEm`
  (0: no value). The first-push log line stays for writing mode and direction.
- The value is treated as a number, scaling with the block's font size, which is CSS's rule for unitless values; `em`
  and `%` values are inherited as lengths in CSS, a difference that only shows where a child changes font size.
- **The pitch.** The parser turns the em into a compression once, `bookPerEm = emPx / lineHeight(body font)`, and
  `effectiveLineHeight` takes a block's compression as `max(lineHeightEm × bookPerEm, bookFloor)` when Book's style is
  on and the block has a value, else `lineCompression`, times the block's font line height and size multiplier as
  today. A heading drawn in a taller face gets its own em's pitch, as line heights scale with the size.
- Lines that are not a block's (the blank line of a `<br>` separator, an `<hr>`'s margin, table rows, the body lines
  keep-with-next reserves) keep Normal's pitch.

### 3.4 Caches

- `Section::BuildParams` gains `bookLineHeightFloor` and `lineHeightEmPx`, appended to the property hash only when the
  floor is set, as the footnote-preview layout version is. The header is unchanged: like `fontSizeNormalization` and
  `inlineFootnotePreviews`, the two are keyed by the hash alone.
- No `SECTION_FILE_VERSION` bump: with Book's style off the parser's output is unchanged, so a Latin reader at Tight,
  Normal or Wide keeps every cache. A CJK book's `lineCompression` changes at every step, so a new hash: its old caches
  are not read and age out.
- The pipeline goldens are built with a fixed profile (`lineCompression` 1.0, Book's style off): unchanged.

### 3.5 Per-book override

As paragraph alignment has its own: `RecentBook::lineSpacingOverride` (-1 Default), `recent.json`
(`lineSpacingOverride`), `setReaderOverrides`, the reader's effective step, Quick overrides and the reader menu,
`ActivityResult`, and `drawCurrentPageToBuffer`'s own reading of the book's overrides (the sleep screen's page).

## 4. Validation

- Host: `LineSpacing` unit tests (every step, both scales, Noto Sans, Book's style); pipeline tests for Book's style
  (a declared 1.75 kept, calibre's 1.2 floored, an unset block at Normal, inheritance from `html`); every golden
  unchanged.
- X3: the three study pages at Normal (1.7 em) against the study's screens; a Latin book's page counts unchanged at
  Normal; Book's style on `変な家２` (1.75) and on the Hayakawa book (calibre's 1.2, floored at 1.5 em); the override
  set and cleared from Quick overrides.
