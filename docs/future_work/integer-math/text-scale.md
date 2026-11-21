# Text scale in fixed point

Part of [Integer maths](../integer-math.md). Area 2. Counts from `scripts/float_inventory.py` on 2026-11-21, after area 1.

**Open.** A text scale is a `float` through the renderer's text API (`getTextWidthScaled`, `drawTextScaled`,
`glyphRunWidth`, `scaledTextAdvanceX`, `fontBaseScale`, small caps' `SCALE`) and into layout and drawing: about 350
call sites in 61 functions. It runs per glyph and per word, so this is the area where the C3 spends the most time in
soft-float.

**Where.** `GfxRenderer.cpp` (162: `drawGlyphRun`, `drawCodepoints`, `drawResolvedGlyphAtScale`, `getGlyphInk`,
`drawTextAtScale`, `applyBase`, `captureScaledGlyphPlane`, ...), `ParsedText.cpp` (42: `extractLine`,
`fitsCombined`, ...), `TextBlock.cpp`/`.h` (44: `render`, `wordBox`, `wordScale`), `EpdFont.cpp` (22:
`getTextBounds`), `OtGpos.cpp`/`OtFace.cpp` (21), `VerticalDraw.cpp` (11), `UiShapedText.cpp` (8),
`DictionaryWordSelectActivity.cpp` (8), `SdCardFontManager.cpp` (6), `FontDecompressor.cpp` (5, a log line),
`ChapterHtmlSlimParser.cpp` (5: `addLineToPage`, `startNewTextBlock`), `FontScalingTestActivity.cpp`, `main.cpp`
(`setupDisplayAndFonts`), `FontLookup.h`, `GfxRenderer.h` (`insertScaledFont`).

**Format.** `TextBlock::serialize` writes `renderStyle.fontSizeMultiplier` as a float (`TextBlock.cpp:1156`), so a
fixed-point type there bumps `SECTION_FILE_VERSION`. Rounding may move a word by a pixel in rare cases; the epub
pipeline goldens show where.

**Next step.** A fixed-point scale type (for example 1/1024 units in a `uint16_t`, `round(v * q / 1024)`) through
the text API, the shaper's `ot::Scale` and the text block. Coordinate with
[CSS lengths and layout](css-lengths-and-layout.md), which produces the multiplier. Measure a page render and a
section build on the X3 before and after.
