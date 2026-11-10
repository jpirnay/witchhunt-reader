# The CJK UI faces on the card

Status: open as of 2026-11-10, after the UI language packs moved to the card (2.41).

The strings of Chinese, Japanese and Korean are card packs like every other language's, but their six CJK UI faces
(`lib/EpdFont/builtinFonts/cjk_ui_{zh,ja,ko}_{10,12}.h`) are still in the image: 97,132 B of compressed bitmaps, plus
~16 KB of glyph tables and the groups. The active language's two are inflated into the language slot as its
extension, after its strings (docs/i18n.md).

## Why they did not move with the strings

- **The slot's extension is half a face.** It holds the flat bitmaps and their offsets; the glyph tables and intervals
  stay in the image. A pack carrying those bytes would fit exactly one firmware's glyph set: the pack on master,
  regenerated when a translation adds characters, would draw boxes on every firmware cut before. The language packs
  are version-independent; this would not be.
- **The image faces draw more than the active language's strings.** The language picker draws the three languages'
  names in their own scripts whatever is installed, and CJK book titles take the characters the translations share
  from these faces in every UI language, before the CJK UI packs on the card (`CjkUiPack`) are asked.
- **The CJK UI path changes and can only be proved on a device**: the faces are drawn from the slot, the others per
  glyph through `FontDecompressor`.

## What moving them takes

1. A self-describing face in the pack's extension: per size the metrics (advance, ascender, descender), the intervals,
   the packed glyph table, the flat bitmap and its offsets, at offsets a small header states. `gen_i18n.py --packs`
   builds it from the committed headers: inflate each group (`compress_ui_face.py` `packed_from_rows`), check the flat
   bitmap against `PackedCrc32`. The slot takes the inflated extension as it is; `CjkUiFonts` builds its two
   `EpdFontData` from it rather than from the image's tables.
2. A built-in face of the picker's names only (简体中文, 日本語, 한국어, with U+3000 and the stand-in U+25A1), ~1 KB at
   10 and 12 pt, cut from the same headers, so the picker and the stand-in work without any pack.
3. The non-active languages' faces leave the providers; CJK titles then come from the CJK UI packs or the reader's
   font only. The offers already pair a CJK language's pack with its interface pack.
4. The firmware stops including the six headers; the host tests and `ui_font_coverage.py` keep reading them.

Saves ~110 KB of the image. Device checks: a Chinese, Japanese and Korean UI from their packs, the picker's names on a
card without them, a CJK title in an English UI with and without ChineseUI.
