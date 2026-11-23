# The CJK UI faces on the card

Status: open as of 2026-11-23. The UI language packs moved to the card in 2.41; how the CJK faces work today is in
[../i18n.md](../i18n.md#chinese-japanese-and-korean).

The strings of Chinese, Japanese and Korean are card packs like every other language's, but their six CJK UI faces
(`lib/EpdFont/builtinFonts/cjk_ui_{zh,ja,ko}_{10,12}.h`, included by `src/CjkUiFonts.cpp`) are still in the image. By
the headers' array sizes:
- compressed bitmaps 95,234 B, and 99 group records of 20 B (1,980 B);
- glyph tables 18,504 B (3,084 glyphs of 6 B);
- interval tables 15,648 B (one per language, shared by its two sizes).

With a language pack slot, the active language's two faces are inflated into it as its extension, after its strings.
Without one (card mode, #774: no usable `spiffs` partition and no PSRAM image) there is no extension, and the active
language's faces decode per glyph like the other two
([../i18n.md](../i18n.md#card-mode-a-board-with-no-room-for-the-slot)).

## Why they did not move with the strings

- **The slot's extension is half a face.** It holds the flat bitmaps and their offsets; the glyph tables and intervals
  stay in the image. A pack carrying those bytes would fit exactly one firmware's glyph set: the pack on master,
  regenerated when a translation adds characters, would draw boxes on every firmware cut before. The language packs
  are version-independent; this would not be.
- **The image faces draw more than the active language's strings.** The language picker draws the three languages'
  names in their own scripts whatever is installed, and CJK book titles take the characters the translations share
  from these faces in every UI language, after the CJK UI packs on the card (`CjkUiPack`) and the reader's font
  (`FallbackFaceProvider::serve`).
- **The CJK UI path changes and can only be proved on a device**: the faces are drawn from the slot, the others per
  glyph through `FontDecompressor`.

## What moving them takes

1. A self-describing face in the pack's extension: per size the metrics (advance, ascender, descender), the intervals,
   the packed glyph table, the flat bitmap and its offsets, at offsets a small header states. The pack format already
   has an extension field (`lib/I18n/LangPack.h`; `build_pack_file` in `scripts/gen_i18n.py`), which
   `language_packs()` leaves empty today. `gen_i18n.py --packs` builds the face from the committed headers: inflate each
   group (`compress_ui_face.py` `packed_from_rows`), check the flat bitmap against `PackedCrc32`. The slot takes the
   inflated extension as it is; `CjkUiFonts` builds its two `EpdFontData` from it rather than from the image's tables.
2. A built-in face of the picker's names only (简体中文, 日本語, 한국어, with U+3000 and the stand-in U+25A1), ~1 KB at
   10 and 12 pt, cut from the same headers, so the picker and the stand-in work without any pack.
3. The non-active languages' faces leave the providers; CJK titles then come from the CJK UI packs or the reader's
   font only. The offers already pair a CJK language's pack with its interface pack (`UiLanguagePacks`).
4. The firmware stops including the six headers; the host tests and `ui_font_coverage.py` keep reading them.
5. Card mode. Today it decodes the active language's faces per glyph from the image; once they leave it, a board in
   card mode has nothing to decode from (an inference from steps 1-4, not yet designed). Options: inflate the faces
   to a file on the card as `strings.bin` is, or read them per glyph from the pack. To be decided with step 1.

Saves the arrays above, about 131 KB by the headers, less the ~1 KB of step 2: computed from the array sizes, not
measured from a build. The figure written here before (~110 KB, with "~16 KB of glyph tables and the groups") could
not be reproduced from the headers. Device checks: a Chinese, Japanese and Korean UI from their packs, with the slot
and in card mode; the picker's names on a card without them; a CJK title in an English UI with and without ChineseUI.
