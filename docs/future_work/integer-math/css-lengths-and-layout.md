# CSS lengths and layout in fixed point

Part of [Integer maths](../integer-math.md). Area 3. Counts from `scripts/float_inventory.py` on 2026-11-21, after area 1.

**Open.** CSS lengths (`CssLength::value`, em, %, vw), the font-size multiplier, line height and line compression are
floats from the parser to the page: about 290 call sites in 37 functions, 95 of them in
`ChapterHtmlSlimParser::startElement`, once per element of every chapter.

**Where.** `ChapterHtmlSlimParser.cpp` (198: `startElement`, `finalizePendingDropCap`, `buildImageFromEpubPath`,
`initializeFontSizeBaseline`, `effectiveLineHeight`, `currentBlockSizePct`, `lineImageTop`, `layoutTableRow`,
`keepTogetherBreak`, `addAncestorInsets`, ...), `CssParser.cpp` (33: `parseDeclarationIntoStyle`,
`interpretInitialLetter`, `interpretLineHeight`, `interpretVerticalAlignLength`, `tryInterpretLength`),
`FontSizeLadder.h` (23), `CssStyle.h` (21: `CssLength::toPixels`, `toPixelsInt16`, `normalizeFontSizeForElement`),
`LineSpacing.h` (4), `BlockStyle.h` (3), `Section.cpp` (2). CSS numbers are parsed with `strtof`.

**Format.** The CSS cache stores `CssLength` as a float (`CssParser.cpp:103`, `CSS_LENGTH_BYTES`) and the style's
float multipliers: bump `CSS_CACHE_VERSION`. The section header stores the line compression as a float
(`Section.cpp`, `header::kLineCompression`): bump `SECTION_FILE_VERSION`. `CrossPointSettings::getReaderLineCompression`
returns a float. Fetch master right before the bump.

**Next step.** A fixed-point length (for example 1/256 px or 1/1000 em) with an integer CSS number parser, then the
parser's arithmetic on it. The font-size multiplier is a `TextScale` already (`lib/EpdFont/TextScale.h`, area 2): the
parser turns its float into one where it hands a size to layout (`textScaleOf` in `CssStyle.h`). The epub pipeline
goldens and the CSS suites show layout changes; a pixel here and there from rounding is expected.
