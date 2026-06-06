# Float layout: per-line width from active float zones

## Goal

Text flowing around a left-floated image for as many lines as the image is tall,
then resuming full-width below it.  This covers drop-cap initials, chapter
decorators, and sidebar thumbnails that appear in many Project Gutenberg and
commercial EPUBs.

## Scope gate

- In scope: rectangular left/right floats beside paragraph text, single-pass
  layout, no re-flow.
- Out of scope: multi-column, table-based layout, overlapping floats, floats
  that force page breaks, right-to-left scripts.

## Constraints

- ESP32-C3: single core, ~50 KB free heap during layout, no FPU.
- No new heap allocation in the hot path (word-break loop).
- Must not touch the display HAL, GfxRenderer, or SD layer.
- Fits the existing `ParsedText` / `ChapterHtmlSlimParser` architecture without
  a new class boundary.

---

## Data model

### `FloatZone` (new, in `BlockStyle.h`)

```cpp
struct FloatZone {
  int16_t top;    // y relative to page top (set when image is placed)
  int16_t bottom; // top + imageHeight
  int16_t width;  // imageWidth + gap (4 px)
  // 6 bytes — three int16_t, no padding on any target
};
```

Stored inline in `BlockStyle` as a fixed-size array:

```cpp
static constexpr int kMaxFloatZones = 2;
FloatZone floatZones[kMaxFloatZones] = {};
int8_t floatZoneCount = 0;
```

`kMaxFloatZones = 2` covers every EPUB seen in practice (one float per
paragraph, occasionally two adjacent paragraphs carry their own float).
At 2 × 6 + 1 = 13 bytes added to `BlockStyle`, stack cost is negligible.

### Lifecycle

1. Parser places inline image at provisional `yPos = currentPageNextY`
   (unchanged from today).
2. Parser adds a `FloatZone{top=currentPageNextY, bottom=currentPageNextY +
   imageHeight, width=imageWidth+4}` to the **next** block's `BlockStyle`
   before calling `startNewTextBlock`.
3. `addLineToPage` fixes `deferredPageImage_->yPos` on the first line exactly
   as today; the `FloatZone.top` value is adjusted to match at the same time.
4. `FloatZone` is NOT propagated through `getCombinedBlockStyle` — it is
   consumed by the single paragraph it was attached to.  If that paragraph
   overflows a page, the remaining words start a fresh block (continuation
   flush) with no float, which is correct: the image was on the previous page.

---

## Algorithm change in `ParsedText`

### New helper: `widthForLine`

```cpp
// Returns the available line width at a given 0-based line index.
// blockStartY is currentPageNextY at the time layoutAndExtractLines is called.
int ParsedText::widthForLine(int lineIndex, int lineHeight, int16_t blockStartY,
                             int pageWidth) const {
  const int lineTop = blockStartY + lineIndex * lineHeight;
  int indent = 0;
  for (int i = 0; i < blockStyle.floatZoneCount; ++i) {
    const auto& z = blockStyle.floatZones[i];
    if (lineTop < z.bottom && lineTop + lineHeight > z.top) {
      indent += z.width;
    }
  }
  return pageWidth - indent;
}
```

No heap, no float arithmetic, O(floatZoneCount) = O(2).

### Changes to `computeLineBreaks` (Knuth-Plass DP)

**Problem:** the DP iterates by *word start index*, not line number.  `i == 0`
is line 0, but `i == k` for k > 0 is not line k in general.

**Solution: greedy pre-pass to assign line numbers to word-start indices.**

Before the DP, run one greedy forward pass (same logic as
`computeHyphenatedLineBreaks`) to record which word-start index begins each
line.  Store a small `lineStartWordIndex[]` vector (same size as word count,
typically < 60 entries).  Then in the DP inner loop replace:

```cpp
const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
```

with:

```cpp
const int lineIdx  = lineIndexForWord[i];   // O(1) lookup
const int effective = widthForLine(lineIdx, lineHeight, blockStartY, pageWidth)
                      - (lineIdx == 0 ? firstLineIndent : 0);
```

`lineIndexForWord` is a `std::vector<int>` of size `totalWordCount`.  At
≤ 60 words that is 240 bytes on the stack — acceptable.  It is filled once
in the pre-pass and discarded when `computeLineBreaks` returns.

The same substitution is applied in:
- `computeHyphenatedLineBreaks` — replace `isFirstLine` bool with line counter.
- `computeSingleLineBreakNoHyphen` — simpler: passes `lineStartIndex` already,
  so just call `widthForLine(lineIndex, ...)`.
- `extractLine` — for justification width, replace `pageWidth` with
  `widthForLine(breakIndex, ...)`.

### Passing `blockStartY` and `lineHeight` into `ParsedText`

`layoutAndExtractLines` gains two parameters:

```cpp
void layoutAndExtractLines(
    const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
    const std::function<...>& processLine,
    bool includeLastLine = true,
    int16_t blockStartY = 0,   // new — currentPageNextY at call site
    int lineHeight = 0);       // new — 0 = no float zones active (fast path)
```

`lineHeight == 0` is the fast path: `widthForLine` is never called and
`blockStyle.floatZoneCount` is never consulted.  All existing callers pass
nothing and get the fast path for free.

The parser passes `lineHeight = renderer.getLineHeight(fontId) * lineCompression`
(already computed before calling `makePages`) and `blockStartY =
currentPageNextY` only when `blockStyle.floatZoneCount > 0`.

---

## Parser changes (`ChapterHtmlSlimParser`)

Replace the current `marginLeft` increment in `startNewTextBlock`:

```cpp
// BEFORE (today's fix — indents every line equally)
blockStyleWithIndent.marginLeft += pendingInlineImage_.width + 4;

// AFTER — attaches a float zone instead
if (blockStyleWithIndent.floatZoneCount < BlockStyle::kMaxFloatZones) {
  auto& z = blockStyleWithIndent.floatZones[blockStyleWithIndent.floatZoneCount++];
  z.top    = static_cast<int16_t>(currentPageNextY);   // provisional; fixed in addLineToPage
  z.bottom = static_cast<int16_t>(currentPageNextY + pendingInlineImage_.height);
  z.width  = static_cast<int16_t>(pendingInlineImage_.width + 4);
}
```

`addLineToPage` already fixes `deferredPageImage_->yPos` on the first line.
Extend it to also fix `z.top` and `z.bottom` in the block's float zones:

```cpp
if (isFirstLineOfBlock && deferredPageImage_) {
  const int ascender = renderer.getFontAscenderSize(fontId);
  const int imgH     = deferredPageImage_->getImageBlock().getHeight();
  const int imgY     = std::max(0, currentPageNextY + ascender - imgH);
  deferredPageImage_->yPos = static_cast<int16_t>(imgY);
  // Fix the float zone top/bottom to match the corrected image y.
  // The zone was attached to currentTextBlock's blockStyle.
  if (currentTextBlock) {
    auto& bs = currentTextBlock->getBlockStyle();
    for (int i = 0; i < bs.floatZoneCount; ++i) {
      bs.floatZones[i].top    = static_cast<int16_t>(imgY);
      bs.floatZones[i].bottom = static_cast<int16_t>(imgY + imgH);
    }
  }
  const int extra = imgH - ascender;
  if (extra > 0) currentPageNextY += extra;
  deferredPageImage_.reset();
}
```

---

## Page-overflow interaction

When a paragraph with a float zone is split across a page boundary
(`makePages` calls `layoutAndExtractLines` a second time as a continuation
flush), `isContinuation_` is true.  The continuation block must NOT inherit
the float zone, because the image is on the previous page.  This is enforced
by not propagating `FloatZone` through `getCombinedBlockStyle` and by
clearing `floatZoneCount` to 0 on the continuation `ParsedText` (the
`ParsedText` constructor default-initialises it to 0 already).

---

---

## CSS `clear` handling

When an element has `clear:left`, `clear:right`, or `clear:both`, the parser
must advance `currentPageNextY` to the highest `bottom` of all active float
zones before placing the element's block, then zero out `floatZoneCount`.

```cpp
// In startNewTextBlock, before applying blockStyle:
if (cssStyle.hasClear() && cssStyle.cssClear != CssClear::None) {
  for (int i = 0; i < activeFloatZoneCount; ++i)
    currentPageNextY = std::max(currentPageNextY, activeFloatZones[i].bottom);
  activeFloatZoneCount = 0;  // zones consumed
}
```

`activeFloatZones` / `activeFloatZoneCount` is a parser-level accumulator
(not inside `BlockStyle`) that holds zones from the current page's images.
On page emit it is zeroed. On section-file build start it is zeroed.

This is identical to the "exclusion zone leap-frog" described by other sources.

---

## Margin inflation

The zone `width` includes image margins so the text gap is correct:

```
zone.width  = imageWidth + marginLeft + marginRight + 4   // 4 = gap
zone.height = imageHeight + marginBottom                   // marginTop shifts top
zone.top    = placedY + marginTop
```

`padding` and `border` are ignored (MCU shortcut — they are visual, not
layout-altering for our use case).  Negative margins are clamped: zone width
minimum is 4 (the gap) so a negative margin never widens available text area.

---

## Block vs inline elements

The parser already handles this correctly without new code:
- **Block elements** (`p`, `div`, `h1`–`h6`) call `startNewTextBlock`, which
  sets `blockStartY = currentPageNextY` for the new `ParsedText`.
  `widthForLine` then uses the correct Y for that block's lines.
- **Inline elements** (`span`, `b`, `a`) accumulate words into the current
  block without advancing `currentPageNextY`.  No change needed.

The zone stays alive in `blockStyle.floatZones` across block boundaries
because the block's `BlockStyle` carries it.  Each new block receives the
zone when the parser attaches it in `startNewTextBlock`.  Zones expire
naturally when `widthForLine` finds `blockStartY + lineIndex * lineHeight ≥
zone.bottom`.

---

## Commit sequence (refactor-for-review discipline)

Each commit is one concern; no two concerns mix.

| # | Commit title | Files touched | Behaviour change? |
|---|---|---|---|
| 1 | `feat: add FloatZone to BlockStyle` | `BlockStyle.h` | No — field added, always 0 |
| 2 | `feat: widthForLine helper in ParsedText` | `ParsedText.cpp/.h` | No — dead code until called |
| 3 | `feat: pass blockStartY/lineHeight to layoutAndExtractLines` | `ParsedText.h/.cpp`, `ChapterHtmlSlimParser.cpp` | No — new params default to 0/no-op |
| 4 | `feat: float zone line-break in computeLineBreaks and computeHyphenatedLineBreaks` | `ParsedText.cpp` | Yes — first float-aware layout |
| 5 | `feat: parser attaches FloatZone instead of marginLeft for inline images` | `ChapterHtmlSlimParser.cpp` | Yes — replaces today's whole-paragraph indent |
| 6 | `feat: fix FloatZone top/bottom in addLineToPage` | `ChapterHtmlSlimParser.cpp` | Yes — correct y anchoring |
| 7 | `test: float layout unit tests` | `test/float_layout/` | No |

Commits 1–3 are pure additions with no observable behaviour change; they can
be reviewed and merged before 4–6.  Commit 7 can precede 4 as a failing test
that 4–6 make pass (TDD style).

---

## Memory cost

| Item | Bytes |
|---|---|
| `FloatZone` array in `BlockStyle` | +13 bytes per `BlockStyle` instance (stack) |
| `lineIndexForWord` in `computeLineBreaks` | `wordCount × 4` bytes, stack-local, typically 60 words × 4 = 240 bytes |
| No new heap allocations | — |

Steady-state heap cost: **zero**.

---

## HAL / abstraction checklist

- No SD access, no display calls, no GPIO, no `Storage`, no `HalDisplay`.
- No hardcoded screen dimensions.
- No user-facing strings (no `tr()` needed).
- `FloatZone` lives in `lib/Epub`, not in `src/` — correct layer.
- `BlockStyle` is already serialised/deserialised for the page cache;
  `FloatZone` is **not** serialised (it is computed at parse time, discarded
  after layout).  The cache stores finished `PageImage` positions, not layout
  parameters.

---

## Self-review (refactor-for-review)

- [ ] Each commit does exactly one thing.
- [ ] No rename/signature ripple rides on a behaviour commit.
- [ ] `widthForLine` fast-paths when `floatZoneCount == 0` — existing callers
      are unaffected and untouched.
- [ ] No "while I'm here" changes (justification cap, margin collapsing, etc.
      are separate issues tracked in `LayoutQualityTest.cpp`).
