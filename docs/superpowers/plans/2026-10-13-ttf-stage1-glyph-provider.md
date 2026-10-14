# TTF Stage 1: GlyphProvider and a single font lookup. Implementation plan

> **Status (2026-10-14): done.** All five tasks are complete on `refactor/glyph-provider`, which is open as
> PR #468. The commits are `217bc90b9`, `e94a4dd47`, `c50dd4b38`, `a3eb0f4ad`, and `d7d5091f0` (comments and
> clang-format). Results: host suite 1,956/1,956; `default` and `x4pro` build; C3 flash −134 B; `bench_font`
> within 0.6% of master; X3 smoke test clean. Three corrections found while executing are folded into
> the text below and marked *(corrected)*.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prepare the font path for TTF fonts without changing any behaviour:

- replace `EpdFontData`'s untyped glyph-miss callback with a `GlyphProvider` interface;
- route `GfxRenderer`'s 13 font lookups through one helper that can ask a resolver for an ID it does
  not know.

**Architecture:**

- **`GlyphProvider`** (`lib/EpdFont/GlyphProvider.h`) has three calls: `missGlyph`, `ownsBitmap` and
  `kerning`.
  - `EpdFontData` gains a `provider` pointer as its last field, so the generated built-in font headers
    keep compiling.
  - `EpdFont` asks the provider on an interval miss, and for kerning when the font carries no kern
    tables.
- **`SdCardFont`** implements the interface with one `StyleProvider` per style, replacing
  `OverflowContext`.
- **`GfxRenderer::getGlyphBitmap`** asks `provider->ownsBitmap` instead of casting the context to
  `SdCardFont`.
- **The old two fields are removed last.**
- **The font lookup** is a header-only template (`lib/GfxRenderer/FontLookup.h`) so it can be tested
  on the host. `GfxRenderer` uses it at all 13 sites through a private `findFontFamily()`.

**Tech Stack:** C++20 (host tests, gtest via CMake), gnu++2a firmware (PlatformIO, ESP32-C3/S3),
clang-format 21.1.8.

**Spec:** `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md`, sections 3.1, 3.3, 8 (Stage 1) and
9.2, on branch `docs/ttf-fonts-design`.

## Global Constraints

- **No behaviour change** for built-in, `.cpfont` SD, scaled or UI fonts (spec 9.2).
- **All host tests pass, and the page-dump goldens stay byte-identical.** The suite was 1,715 tests on
  2026-10-08. Build with `-- -k 0`; `epub_build_inventory` fails on Windows by design.
- **Firmware envs `default` (ESP32-C3) and `x4pro` (ESP32-S3) both build.**
- **`EpdFontData` gets its new field LAST.** The generated headers in `lib/EpdFont/builtinFonts/*.h`
  initialise it positionally and stop at `ligaturePairCount`, so they must keep compiling unchanged.
- **Heap and perf:**
  - No heap allocation is added on any lookup, measure or draw path, and no bare `new`.
  - A font that has kern tables never pays for a provider call.
- **Formatting:** C++ is formatted with clang-format **21.1.8**, the version CI uses. Local 22.x pads
  differently; see `bin/clang-format-fix`.
- **Toolchain** (Git Bash):
  - `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8` before every `pio`
    command;
  - pio is `"$HOME/.platformio/penv/Scripts/pio.exe"`;
  - host tests build with `/c/msys64/ucrt64/bin` gcc and cmake **from Bash, never PowerShell**.
- **Branch:** `refactor/glyph-provider`, created from `master`.

## Review Focus

1. **A ring glyph with no pixels** (a space loaded by `SdCardFont`'s miss ring) must draw nothing.
   `ownsBitmap` must return true with a null bitmap; otherwise the renderer would read
   `data->bitmap + dataOffset` at an offset that belongs to another font. Test: Task 2,
   `ZeroSizeRingGlyphIsOwnedWithNoBitmap`.
2. **A font with kern tables must never ask the provider for kerning.** That is a correctness question
   (tables win) and a cost question (a virtual call per pair on the hot measure path). Test: Task 1,
   `KerningUsesTablesWhenTheFontHasThem`.
3. **An unknown font ID with no resolver installed must behave exactly as today:** `getTextWidth`
   returns 0, draws do nothing. Test: Task 4, `MissWithoutResolverReturnsNull`.
4. **A resolver that declines must leave the map untouched,** and a resolver must be asked at most once
   per lookup. A declined ID must not end up registered as an empty family. Tests: Task 4,
   `DeclinedIdIsNotRegistered` and `ResolverIsAskedOncePerMiss`.
5. **A compressed built-in font** (`groups != nullptr`, every reader face) must still take the
   decompressor branch first in `getGlyphBitmap`; the provider branch must never run for it. No host
   test can reach `GfxRenderer`, so Task 5 checks it on the device (reader and UI fonts render) and by
   review of the branch order.

---

### Task 1: `GlyphProvider` interface; `EpdFont` consults it

**Files:**
- Create: `lib/EpdFont/GlyphProvider.h`
- Modify: `lib/EpdFont/EpdFontData.h` (forward declaration and the new last field of `EpdFontData`)
- Modify: `lib/EpdFont/EpdFont.cpp` (`EpdFont::findGlyph`, `EpdFont::getKerning`)
- Create: `test/glyph_provider/CMakeLists.txt`, `test/glyph_provider/GlyphProviderTest.cpp`
- Modify: `test/CMakeLists.txt` (add `add_subdirectory(glyph_provider)` right after
  `add_subdirectory(glyph_fallback)`)

**Interfaces:**
- Produces:
  - `class GlyphProvider`, with:
    - `virtual const EpdGlyph* missGlyph(uint32_t codepoint) = 0;`
    - `virtual bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const;` (default false)
    - `virtual int8_t kerning(uint32_t leftCp, uint32_t rightCp);` (default 0)
  - the field `GlyphProvider* EpdFontData::provider`, the last member.

- [x] **Step 1: Create the branch**

```bash
cd /c/_development/witchhunt-reader
git switch master && git pull --ff-only && git switch -c refactor/glyph-provider
```

- [x] **Step 2: Write the failing test**

Create `test/glyph_provider/CMakeLists.txt`:

```cmake
add_executable(GlyphProviderTest
  GlyphProviderTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(GlyphProviderTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(GlyphProviderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GlyphProviderTest)
```

Add `add_subdirectory(glyph_provider)` to `test/CMakeLists.txt` on the line after
`add_subdirectory(glyph_fallback)`.

Create `test/glyph_provider/GlyphProviderTest.cpp`:

```cpp
// EpdFont's side of GlyphProvider: who is asked for a glyph the tables lack, and for kerning.
#include <gtest/gtest.h>

#include <map>
#include <utility>
#include <vector>

#include "EpdFont.h"
#include "GlyphProvider.h"

namespace {

class FakeProvider final : public GlyphProvider {
 public:
  std::map<uint32_t, EpdGlyph> glyphs;
  std::vector<uint32_t> asked;
  std::vector<std::pair<uint32_t, uint32_t>> kernAsked;
  int8_t kernValue = 0;

  const EpdGlyph* missGlyph(const uint32_t codepoint) override {
    asked.push_back(codepoint);
    const auto it = glyphs.find(codepoint);
    return it == glyphs.end() ? nullptr : &it->second;
  }
  int8_t kerning(const uint32_t leftCp, const uint32_t rightCp) override {
    kernAsked.emplace_back(leftCp, rightCp);
    return kernValue;
  }
};

EpdGlyph makeGlyph(const uint8_t width, const uint8_t height, const uint16_t advanceX) {
  EpdGlyph g{};
  g.width = width;
  g.height = height;
  g.advanceX = advanceX;
  g.left = 1;
  g.top = 12;
  g.dataLength = glyphDataBytes(width, height, true);
  g.dataOffset = 40;
  return g;
}

// No interval table, no glyph array, no kern tables: every lookup has to go to the provider.
EpdFontData providerOnly(GlyphProvider* provider) {
  EpdFontData d{};
  d.advanceY = 20;
  d.ascender = 16;
  d.descender = -4;
  d.is2Bit = true;
  d.provider = provider;
  return d;
}

constexpr uint32_t kReplacement = 0xFFFD;

}  // namespace

TEST(GlyphProvider, EveryLookupGoesToTheProviderWhenTheFontHasNoIntervals) {
  FakeProvider provider;
  provider.glyphs['A'] = makeGlyph(9, 12, 10 << 4);
  const EpdFontData data = providerOnly(&provider);
  const EpdFont font(&data);

  const EpdGlyphRef glyph = font.getGlyph('A');

  ASSERT_TRUE(glyph);
  EXPECT_EQ(&provider.glyphs['A'], glyph.sdRecord);
  EXPECT_EQ(10 << 4, glyph.advanceX);
  EXPECT_EQ(9, glyph.width);
  EXPECT_EQ(12, glyph.height);
  EXPECT_EQ(1, glyph.left);
  EXPECT_EQ(12, glyph.top);
  EXPECT_EQ(40u, epdGlyphBitmapOffset(&data, glyph));
  EXPECT_EQ(std::vector<uint32_t>{'A'}, provider.asked);
}

TEST(GlyphProvider, AMissingGlyphFallsBackToTheReplacementThroughTheProvider) {
  FakeProvider provider;
  provider.glyphs[kReplacement] = makeGlyph(8, 8, 9 << 4);
  const EpdFontData data = providerOnly(&provider);
  const EpdFont font(&data);

  const EpdGlyphRef glyph = font.getGlyph('Z');

  ASSERT_TRUE(glyph);
  EXPECT_EQ(&provider.glyphs[kReplacement], glyph.sdRecord);
  ASSERT_FALSE(provider.asked.empty());
  EXPECT_EQ('Z', provider.asked.front());
  EXPECT_EQ(kReplacement, provider.asked.back());
}

TEST(GlyphProvider, NoGlyphAnywhereGivesAnInvalidRef) {
  FakeProvider provider;
  const EpdFontData data = providerOnly(&provider);
  const EpdFont font(&data);

  EXPECT_FALSE(font.getGlyph('Z'));
}

TEST(GlyphProvider, KerningAsksTheProviderWhenTheFontHasNoKernTables) {
  FakeProvider provider;
  provider.kernValue = -3;
  const EpdFontData data = providerOnly(&provider);
  const EpdFont font(&data);

  EXPECT_EQ(-3, font.getKerning('A', 'V'));
  ASSERT_EQ(1u, provider.kernAsked.size());
  EXPECT_EQ(std::make_pair(uint32_t{'A'}, uint32_t{'V'}), provider.kernAsked.front());
}

TEST(GlyphProvider, KerningUsesTablesWhenTheFontHasThem) {
  static const EpdKernClassEntry kLeft[] = {{'A', 1}};
  static const EpdKernClassEntry kRight[] = {{'V', 1}};
  static const int8_t kMatrix[] = {-5};
  FakeProvider provider;
  provider.kernValue = 7;
  EpdFontData data = providerOnly(&provider);
  data.kernLeftClasses = kLeft;
  data.kernRightClasses = kRight;
  data.kernMatrix = kMatrix;
  data.kernLeftEntryCount = 1;
  data.kernRightEntryCount = 1;
  data.kernLeftClassCount = 1;
  data.kernRightClassCount = 1;
  const EpdFont font(&data);

  EXPECT_EQ(-5, font.getKerning('A', 'V'));
  EXPECT_EQ(0, font.getKerning('V', 'A'));  // not in the tables: 0, NOT the provider's 7
  EXPECT_TRUE(provider.kernAsked.empty());
}

TEST(GlyphProvider, AFontWithoutAProviderBehavesAsBefore) {
  const EpdFontData data{};
  const EpdFont font(&data);

  EXPECT_FALSE(font.getGlyph('A'));
  EXPECT_EQ(0, font.getKerning('A', 'V'));
}
```

- [x] **Step 3: Run the test to verify it fails**

Run:

```bash
cd /c/_development/witchhunt-reader
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S test -B build/test && cmake --build build/test --target GlyphProviderTest
```

Expected: a compile error, `GlyphProvider.h: No such file or directory`.

- [x] **Step 4: Create `lib/EpdFont/GlyphProvider.h`**

```cpp
#pragma once

#include <cstdint>

#include "EpdFontData.h"

/// Supplies what a font's own tables do not hold.
///
/// EpdFont asks it for a glyph after the interval table misses, and for pair kerning when the font
/// carries no kern tables. GfxRenderer asks it whether a glyph's bitmap is one it keeps itself.
/// SdCardFont implements it for its glyph-miss ring; a font with no tables at all can answer every
/// lookup through it.
class GlyphProvider {
 public:
  virtual ~GlyphProvider() = default;

  /// The glyph for `codepoint`, or nullptr when the font has none. The record must stay valid at
  /// least until the next missGlyph() call on this provider.
  virtual const EpdGlyph* missGlyph(uint32_t codepoint) = 0;

  /// True when `glyph` is a record whose bitmap this provider keeps itself. `bitmap` then receives
  /// that bitmap, which is nullptr for a glyph with no pixels. False means the bitmap is at
  /// EpdFontData::bitmap + dataOffset as for any other glyph.
  virtual bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const {
    (void)glyph;
    (void)bitmap;
    return false;
  }

  /// Pair kerning in 4.4 fixed-point pixels. Asked only when the font carries no kern tables.
  virtual int8_t kerning(uint32_t leftCp, uint32_t rightCp) {
    (void)leftCp;
    (void)rightCp;
    return 0;
  }
};
```

- [x] **Step 5: Add the field to `EpdFontData`**

In `lib/EpdFont/EpdFontData.h`:

- Add `class GlyphProvider;` just above the `typedef struct {` that opens `EpdFontData`.
- Add the field after `void* glyphMissCtx;`, as the **last** member, immediately before
  `} EpdFontData;`:

```cpp
  /// Glyphs the tables above do not have, and kerning for a font with no kern tables. Null for
  /// built-in fonts. See GlyphProvider.h.
  GlyphProvider* provider;
```

- [x] **Step 6: Make `EpdFont` ask the provider**

In `lib/EpdFont/EpdFont.cpp`, `EpdFont::findGlyph`. Replace the early return

```cpp
  if (count == 0 && !data->glyphMissHandler) return {};
```

with

```cpp
  if (count == 0 && !data->glyphMissHandler && !data->provider) return {};
```

and replace the block that starts `// Codepoint not in interval table — try on-demand loading (SD card fonts).`
(the `if (data->glyphMissHandler) { ... }` block) with:

```cpp
  // Codepoint not in the interval table: ask the font's provider (SD on-demand ring, or a font
  // with no tables at all).
  const EpdGlyph* loaded = nullptr;
  if (data->provider) {
    loaded = data->provider->missGlyph(cp);
  } else if (data->glyphMissHandler) {
    loaded = data->glyphMissHandler(data->glyphMissCtx, cp);
  }
  if (loaded) {
    // A provider's record, not an array one: there is no index to report, and the pointer is what
    // GlyphProvider::ownsBitmap() recognises it by.
    return EpdGlyphRef{loaded,
                       loaded->advanceX,
                       0,  // no array index: this glyph is not in the array
                       loaded->width,
                       loaded->height,
                       static_cast<int8_t>(loaded->left),
                       static_cast<int8_t>(loaded->top),
                       true};
  }
```

Add `#include "GlyphProvider.h"` to the includes of `EpdFont.cpp`, after `#include "GlyphFallback.h"`.

In `EpdFont::getKerning`, replace the two early returns at the top

```cpp
  if (!data->kernMatrix && !data->kernRowOffsets) {
    return 0;
  }
  if (!data->kernLeftClasses && !data->kernLeftCodepoints) {
    return 0;
  }
```

with

```cpp
  const bool hasKernTables =
      (data->kernMatrix || data->kernRowOffsets) && (data->kernLeftClasses || data->kernLeftCodepoints);
  if (!hasKernTables) {
    return data->provider ? data->provider->kerning(leftCp, rightCp) : 0;
  }
```

- [x] **Step 7: Run the new tests to verify they pass**

```bash
cmake --build build/test --target GlyphProviderTest && build/test/glyph_provider/GlyphProviderTest
```

Expected: `[  PASSED  ] 6 tests.`

- [x] **Step 8: Run the existing font suites to confirm nothing moved**

```bash
cmake --build build/test -j 8 -- -k 0
ctest --test-dir build/test -R "SdFont|DifferentialRounding|GlyphFallback|FontSizeLadder|GlyphProvider" --output-on-failure
```

Expected: all of them pass.

- [x] **Step 9: Commit**

```bash
git add lib/EpdFont/GlyphProvider.h lib/EpdFont/EpdFontData.h lib/EpdFont/EpdFont.cpp test/glyph_provider test/CMakeLists.txt
git commit -m "EpdFont: add GlyphProvider and consult it for misses and table-less kerning"
```

---

### Task 2: `SdCardFont` serves its ring through `GlyphProvider`; the renderer asks the provider

**Files:**
- Modify: `lib/EpdFont/SdCardFont.h`:
  - `#include "GlyphProvider.h"`;
  - replace `struct OverflowContext` and `overflowCtx_` with `StyleProvider` and `styleProviders_`;
  - delete `fromMissCtx`.
- Modify: `lib/EpdFont/SdCardFont.cpp`:
  - `applyGlyphMissCallback`;
  - the two places that set `miniData.glyphMissHandler` and `glyphMissCtx` (inside `prewarmStyle`);
  - the cast in `onGlyphMiss`;
  - delete the definition of `fromMissCtx`.
- Modify: `lib/GfxRenderer/GfxRenderer.cpp` (`GfxRenderer::getGlyphBitmap`)
- Modify: `test/sd_font_arena/SdFontArenaTest.cpp` (one existing test switches API; two tests are
  added)

**Interfaces:**
- Consumes: `GlyphProvider` and `EpdFontData::provider`, from Task 1.
- Produces: `SdCardFont::StyleProvider`, private, implementing `missGlyph` and `ownsBitmap`. After
  this task `SdCardFont` sets only `provider`, and `GfxRenderer` reads only `provider`.

- [x] **Step 1: Write the failing tests**

In `test/sd_font_arena/SdFontArenaTest.cpp`, change the body of
`GlyphMissesServeFromTheHeapWithTheArenaFull` so that it goes through the provider. Replace

```cpp
  ASSERT_NE(nullptr, data.glyphMissHandler);
  for (uint32_t g = 1; g < LETTERS; g++) {
    const EpdGlyph* glyph = data.glyphMissHandler(data.glyphMissCtx, FIRST_CP + g);
    ASSERT_NE(nullptr, glyph) << "miss for glyph " << g << " was refused";
    ASSERT_TRUE(font.isOverflowGlyph(glyph));
    const uint8_t* bitmap = font.getOverflowBitmap(glyph);
    ASSERT_NE(nullptr, bitmap);
```

with

```cpp
  ASSERT_NE(nullptr, data.provider);
  for (uint32_t g = 1; g < LETTERS; g++) {
    const EpdGlyph* glyph = data.provider->missGlyph(FIRST_CP + g);
    ASSERT_NE(nullptr, glyph) << "miss for glyph " << g << " was refused";
    const uint8_t* bitmap = nullptr;
    ASSERT_TRUE(data.provider->ownsBitmap(glyph, bitmap));
    ASSERT_NE(nullptr, bitmap);
```

(The remaining lines of the loop, which compare the bitmap bytes, stay as they are.)

Add these two tests before the closing `}  // namespace`:

```cpp
// The renderer asks the provider whose bitmap a glyph is. An array glyph is not the ring's: its
// bitmap is at EpdFontData::bitmap + dataOffset like any other.
TEST(SdFontArena, ProviderOwnsOnlyRingBitmaps) {
  const std::string path = writeTemp(buildFont(64, /*kerning=*/false));
  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  ASSERT_EQ(0, font.prewarm("A", 0x01));
  const EpdFont& epd = *font.getEpdFont(0);
  ASSERT_NE(nullptr, epd.data->provider);

  const EpdGlyphRef arrayGlyph = epd.getGlyph('A');
  ASSERT_TRUE(arrayGlyph);
  const uint8_t* bitmap = reinterpret_cast<const uint8_t*>(1);
  EXPECT_FALSE(epd.data->provider->ownsBitmap(arrayGlyph.sdRecord, bitmap));

  const EpdGlyphRef ringGlyph = epd.getGlyph('C');  // not prewarmed: comes from the miss ring
  ASSERT_TRUE(ringGlyph);
  bitmap = nullptr;
  EXPECT_TRUE(epd.data->provider->ownsBitmap(ringGlyph.sdRecord, bitmap));
  EXPECT_NE(nullptr, bitmap);
}

// A glyph with no pixels (a space) is still the ring's: the renderer must get "owned, no bitmap"
// rather than fall through and read EpdFontData::bitmap at an offset that belongs to nothing.
TEST(SdFontArena, ZeroSizeRingGlyphIsOwnedWithNoBitmap) {
  const std::string path = writeTemp(buildFont(0, /*kerning=*/false));
  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  const EpdFontData& data = *font.getEpdFont(0)->data;  // the stub: nothing prewarmed
  ASSERT_NE(nullptr, data.provider);

  const EpdGlyph* glyph = data.provider->missGlyph('B');
  ASSERT_NE(nullptr, glyph);
  const uint8_t* bitmap = reinterpret_cast<const uint8_t*>(1);
  EXPECT_TRUE(data.provider->ownsBitmap(glyph, bitmap));
  EXPECT_EQ(nullptr, bitmap);
}
```

- [x] **Step 2: Run them to verify they fail**

```bash
cmake --build build/test --target SdFontArenaTest && build/test/sd_font_arena/SdFontArenaTest
```

Expected: a compile error in `SdFontArenaTest.cpp`, an invalid use of the incomplete type
`GlyphProvider`. `SdCardFont.h` does not include `GlyphProvider.h` yet, so the tests cannot call the
provider. *(Corrected: the plan first expected a runtime failure on the provider assertion.)*

- [x] **Step 3: Give `SdCardFont` a `StyleProvider`**

In `lib/EpdFont/SdCardFont.h`, add `#include "GlyphProvider.h"` next to the other local includes.

Delete the public declaration of `fromMissCtx` and its two comment lines:

```cpp
  // Extract SdCardFont* from an opaque glyphMissCtx pointer.
  // Used by GfxRenderer::getGlyphBitmap() to recover the SdCardFont from EpdFontData::glyphMissCtx.
  static SdCardFont* fromMissCtx(void* ctx);
```

Replace

```cpp
  // Overflow context: glyphMissHandler needs to know which style it's serving
  struct OverflowContext {
    SdCardFont* self;
    uint8_t styleIdx;
  };
  OverflowContext overflowCtx_[MAX_STYLES] = {};
```

with

```cpp
  // Each style's EpdFontData points at its own StyleProvider: a miss goes to the shared ring, and
  // the renderer asks it whether a glyph's bitmap is one the ring holds.
  class StyleProvider final : public GlyphProvider {
   public:
    SdCardFont* self = nullptr;
    uint8_t styleIdx = 0;

    const EpdGlyph* missGlyph(const uint32_t codepoint) override { return SdCardFont::onGlyphMiss(this, codepoint); }
    bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const override {
      if (!self->isOverflowGlyph(glyph)) return false;
      bitmap = self->getOverflowBitmap(glyph);
      return true;
    }
  };
  StyleProvider styleProviders_[MAX_STYLES];
```

In the same header, change the comment above the `onGlyphMiss` declaration from
`// Static callback for EpdFontData::glyphMissHandler (per-style via OverflowContext)` to
`// The miss path behind StyleProvider::missGlyph; ctx is that StyleProvider.`

In `lib/EpdFont/SdCardFont.cpp`:

- Replace the body of `applyGlyphMissCallback`:

```cpp
void SdCardFont::applyGlyphMissCallback(uint8_t styleIdx) {
  styleProviders_[styleIdx].self = this;
  styleProviders_[styleIdx].styleIdx = styleIdx;
  styles_[styleIdx].stubData.provider = &styleProviders_[styleIdx];
}
```

- In `prewarmStyle`, two places set the mini data's hook. Find them with
  `grep -n "miniData.glyphMissHandler" lib/EpdFont/SdCardFont.cpp`. Replace each pair of lines

```cpp
      s.miniData.glyphMissHandler = &SdCardFont::onGlyphMiss;
      s.miniData.glyphMissCtx = &overflowCtx_[styleIdx];
```

  (the first copy is indented by six spaces, the second by two; keep each site's indentation) with:

```cpp
      s.miniData.provider = &styleProviders_[styleIdx];
```

- In `SdCardFont::onGlyphMiss`, replace `auto* oc = static_cast<OverflowContext*>(ctx);` with
  `auto* oc = static_cast<StyleProvider*>(ctx);`. The rest of the function reads `oc->self` and
  `oc->styleIdx`, which `StyleProvider` has.
- Delete the definition
  `SdCardFont* SdCardFont::fromMissCtx(void* ctx) { return static_cast<OverflowContext*>(ctx)->self; }`.

- [x] **Step 4: Make the renderer ask the provider**

In `lib/GfxRenderer/GfxRenderer.cpp`, `GfxRenderer::getGlyphBitmap`, replace

```cpp
  // For SD card fonts, check if the glyph was loaded on demand into the overflow
  // buffer.  getOverflowBitmap() returns:
  //   - bitmap pointer for overflow glyphs with bitmap data
  //   - nullptr for overflow glyphs without bitmap data (e.g. space: width=0, height=0)
  //   - nullptr for non-overflow glyphs (normal prewarmed path)
  // We distinguish overflow-with-no-bitmap from non-overflow by checking isOverflowGlyph().
  // sdRecord is the record this ref resolved from, and is null for built-in fonts. The overflow
  // checks key on its pointer identity to tell a ring-loaded glyph from an array one, which is
  // why resolution has to carry it rather than just the values.
  if (fontData->glyphMissCtx && glyph.sdRecord) {
    auto* sdFont = SdCardFont::fromMissCtx(fontData->glyphMissCtx);
    if (sdFont->isOverflowGlyph(glyph.sdRecord)) {
      return sdFont->getOverflowBitmap(glyph.sdRecord);  // may be nullptr for zero-width glyphs
    }
  }
```

with

```cpp
  // A glyph the font's provider loaded itself (SdCardFont's miss ring) keeps its bitmap there.
  // ownsBitmap() answers by the record's pointer identity, which is why resolution carries
  // sdRecord; it is null for built-in fonts. An owned glyph with no pixels (a space) yields
  // nullptr and must not fall through to the flat bitmap below.
  if (fontData->provider && glyph.sdRecord) {
    const uint8_t* owned = nullptr;
    if (fontData->provider->ownsBitmap(glyph.sdRecord, owned)) return owned;
  }
```

Add `#include <GlyphProvider.h>` beside the other EpdFont includes at the top of `GfxRenderer.cpp`.

- [x] **Step 5: Run the SD font suites to verify they pass**

```bash
cmake --build build/test -j 8 -- -k 0
ctest --test-dir build/test -R "SdFont|GlyphProvider|DifferentialRounding" --output-on-failure
```

Expected: all pass, including `ProviderOwnsOnlyRingBitmaps` and `ZeroSizeRingGlyphIsOwnedWithNoBitmap`.

- [x] **Step 6: Build the firmware**

This is the only compile check for `GfxRenderer.cpp`. A cold build takes more than 10 minutes, so run
it in the background:

```bash
export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8
"$HOME/.platformio/penv/Scripts/pio.exe" run -e default > /c/tmp/stage1-task2-default.log 2>&1; tail -5 /c/tmp/stage1-task2-default.log
```

Expected: `[SUCCESS]`.

- [x] **Step 7: Commit**

```bash
git add lib/EpdFont/SdCardFont.h lib/EpdFont/SdCardFont.cpp lib/GfxRenderer/GfxRenderer.cpp test/sd_font_arena/SdFontArenaTest.cpp
git commit -m "SdCardFont: serve the glyph-miss ring through GlyphProvider

GfxRenderer asks the provider whether it owns a glyph's bitmap instead of
casting EpdFontData::glyphMissCtx to SdCardFont, which any other provider
would have been misread as."
```

---

### Task 3: Remove the old glyph-miss hook

**Files:**
- Modify: `lib/EpdFont/EpdFontData.h`:
  - delete `glyphMissHandler` and `glyphMissCtx`, with their doc comments;
  - reword the `EpdGlyphRef` comments that name them.
- Modify: `lib/EpdFont/EpdFont.cpp`: drop the `glyphMissHandler` branch and the `glyphMissHandler`
  term in the early return.
- Modify: `lib/EpdFont/SdCardFont.h`: reword the comments that still say `glyphMissHandler`.
- Modify: `test/differential_rounding/DifferentialRoundingTest.cpp`: the designated initialiser.

**Interfaces:**
- Consumes: Tasks 1 and 2 (nothing sets or reads the old fields any more).
- Produces: an `EpdFontData` whose last member is `GlyphProvider* provider`.

- [x] **Step 1: Make the test fixture use the new field**

This step does not fail the build: C++20 designated initialisers may skip members. The gate for this
task is Step 3's grep plus the whole suite. *(Corrected.)*

In `test/differential_rounding/DifferentialRoundingTest.cpp`, replace the two lines

```cpp
  .glyphMissHandler  = nullptr,
  .glyphMissCtx      = nullptr,
```

with

```cpp
  .provider          = nullptr,
```

- [x] **Step 2: Remove the fields and the legacy branch**

In `lib/EpdFont/EpdFontData.h`, delete this block, which sits right above `provider`:

```cpp
  /// On-demand glyph loading for fonts that don't keep all glyphs in RAM (e.g. SD card fonts).
  /// Called by getGlyph() when a codepoint is not found in the interval table.
  /// Returns a valid EpdGlyph* with correct metadata, or nullptr to fall back to the
  /// replacement glyph.  The returned pointer is valid until the next glyphMissHandler
  /// call that causes a ring-buffer eviction — callers must consume it (measure or draw)
  /// before requesting another missed glyph.
  const EpdGlyph* (*glyphMissHandler)(void* ctx, uint32_t codepoint);

  /// Context pointer for glyphMissHandler (typically SdCardFont*).  Also used by
  /// GfxRenderer::getGlyphBitmap() to retrieve overflow bitmaps via SdCardFont.
  void* glyphMissCtx;
```

In the same file, update two comments:

- In the `EpdGlyphRef` doc, `valid only until the next glyphMissHandler call that evicted the overflow
  ring` becomes `valid only until the next GlyphProvider::missGlyph() call that evicted the overflow
  ring`.
- The `sdRecord` doc `SdCardFont::isOverflowGlyph() and getOverflowBitmap() key on pointer identity to
  tell a ring-loaded glyph from an array one,` becomes `GlyphProvider::ownsBitmap() keys on pointer
  identity to tell a provider-loaded glyph from an array one,`.

In `lib/EpdFont/EpdFont.cpp`, `findGlyph`:

- the early return becomes `if (count == 0 && !data->provider) return {};`;
- the lookup becomes:

```cpp
  // Codepoint not in the interval table: ask the font's provider (SD on-demand ring, or a font
  // with no tables at all).
  if (data->provider) {
    if (const EpdGlyph* loaded = data->provider->missGlyph(cp)) {
      // A provider's record, not an array one: there is no index to report, and the pointer is
      // what GlyphProvider::ownsBitmap() recognises it by.
      return EpdGlyphRef{loaded,
                         loaded->advanceX,
                         0,  // no array index: this glyph is not in the array
                         loaded->width,
                         loaded->height,
                         static_cast<int8_t>(loaded->left),
                         static_cast<int8_t>(loaded->top),
                         true};
    }
  }
```

In `lib/EpdFont/SdCardFont.h`:

- `// Debug counters for on-demand glyph misses (glyphMissHandler).` becomes
  `// Debug counters for on-demand glyph misses (StyleProvider::missGlyph).`;
- `// Shared on-demand overflow buffer (ring buffer of glyphs loaded via glyphMissHandler)` becomes
  `// Shared on-demand overflow buffer (ring of glyphs loaded by StyleProvider::missGlyph)`.

- [x] **Step 3: Prove nothing refers to the old hook**

```bash
grep -rn "glyphMissHandler\|glyphMissCtx\|fromMissCtx\|OverflowContext" lib src test bench freeink-sdk --include=*.h --include=*.cpp --include=*.c
```

Expected: no output.

- [x] **Step 4: Run the whole host suite**

```bash
cmake --build build/test -j 8 -- -k 0
ctest --test-dir build/test -j 8 --output-on-failure
```

Expected: everything passes except tests that are `NOT_BUILT` only because of
`epub_build_inventory` (known Windows `<dlfcn.h>` failure). The test count is 1,715 plus the 8 added
in Tasks 1-2 on the master of 2026-10-08; it moves with master.

- [x] **Step 5: Build both firmware boards**

```bash
export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8
"$HOME/.platformio/penv/Scripts/pio.exe" run -e default > /c/tmp/stage1-task3-default.log 2>&1; tail -3 /c/tmp/stage1-task3-default.log
"$HOME/.platformio/penv/Scripts/pio.exe" run -e x4pro > /c/tmp/stage1-task3-x4pro.log 2>&1; tail -3 /c/tmp/stage1-task3-x4pro.log
```

Expected: `[SUCCESS]` for both. (Run them one after the other, never in parallel; see the
PlatformIO build-dir note in the toolchain constraint.)

- [x] **Step 6: Commit**

```bash
git add lib/EpdFont/EpdFontData.h lib/EpdFont/EpdFont.cpp lib/EpdFont/SdCardFont.h test/differential_rounding/DifferentialRoundingTest.cpp
git commit -m "EpdFont: remove the glyphMissHandler callback in favour of GlyphProvider"
```

---

### Task 4: One font lookup with an optional resolver

**Files:**
- Create: `lib/GfxRenderer/FontLookup.h`
- Create: `test/font_lookup/CMakeLists.txt`, `test/font_lookup/FontLookupTest.cpp`
- Modify: `test/CMakeLists.txt` (add `add_subdirectory(font_lookup)` after `add_subdirectory(font_size_ladder)`)
- Modify: `lib/GfxRenderer/GfxRenderer.h`:
  - `fontMap` becomes `mutable`;
  - add the `fontResolver_` member, `setFontResolver()` and the private `findFontFamily()`.
- Modify: `lib/GfxRenderer/GfxRenderer.cpp`: all 13 `fontMap.find(fontId)` sites *(corrected: the plan first said 16)*.

**Interfaces:**
- Produces:
  - `template <typename Family> struct FontResolverHook { std::optional<Family> (*resolve)(void* ctx, int fontId) = nullptr; void* ctx = nullptr; };`
  - `template <typename Family> const Family* findOrResolveFont(std::map<int, Family>& fonts, int fontId, const FontResolverHook<Family>& hook);`
  - `void GfxRenderer::setFontResolver(std::optional<EpdFontFamily> (*resolve)(void* ctx, int fontId), void* ctx);`
  - `const EpdFontFamily* GfxRenderer::findFontFamily(int fontId) const;` (private)

  Stage 2c installs the resolver; nothing installs one in this stage.

- [x] **Step 1: Write the failing test**

Create `test/font_lookup/CMakeLists.txt`:

```cmake
add_executable(FontLookupTest
  FontLookupTest.cpp
)

target_include_directories(FontLookupTest PRIVATE
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(FontLookupTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FontLookupTest)
```

Add `add_subdirectory(font_lookup)` to `test/CMakeLists.txt` after `add_subdirectory(font_size_ladder)`.

Create `test/font_lookup/FontLookupTest.cpp`:

```cpp
// The renderer's single font lookup: a hit is a plain map hit, a miss may ask a resolver once.
#include <gtest/gtest.h>

#include <map>
#include <optional>

#include "FontLookup.h"

namespace {

struct Resolver {
  int calls = 0;
  std::optional<int> answer;

  static std::optional<int> resolve(void* ctx, const int fontId) {
    auto* self = static_cast<Resolver*>(ctx);
    self->calls++;
    (void)fontId;
    return self->answer;
  }
  FontResolverHook<int> hook() { return FontResolverHook<int>{&Resolver::resolve, this}; }
};

}  // namespace

TEST(FontLookup, HitDoesNotAskTheResolver) {
  std::map<int, int> fonts{{7, 70}};
  Resolver r;
  r.answer = 99;

  const int* found = findOrResolveFont(fonts, 7, r.hook());

  ASSERT_NE(nullptr, found);
  EXPECT_EQ(70, *found);
  EXPECT_EQ(0, r.calls);
}

TEST(FontLookup, MissWithoutResolverReturnsNull) {
  std::map<int, int> fonts{{7, 70}};

  EXPECT_EQ(nullptr, findOrResolveFont(fonts, 8, FontResolverHook<int>{}));
  EXPECT_EQ(1u, fonts.size());
}

TEST(FontLookup, MissAsksTheResolverAndRegistersWhatItSupplies) {
  std::map<int, int> fonts;
  Resolver r;
  r.answer = 42;

  const int* first = findOrResolveFont(fonts, 8, r.hook());
  const int* second = findOrResolveFont(fonts, 8, r.hook());

  ASSERT_NE(nullptr, first);
  EXPECT_EQ(42, *first);
  EXPECT_EQ(first, second);  // the second lookup is a plain hit on the registered entry
  EXPECT_EQ(1, r.calls);
}

TEST(FontLookup, ResolverIsAskedOncePerMiss) {
  std::map<int, int> fonts;
  Resolver r;  // declines

  EXPECT_EQ(nullptr, findOrResolveFont(fonts, 8, r.hook()));
  EXPECT_EQ(1, r.calls);
}

TEST(FontLookup, DeclinedIdIsNotRegistered) {
  std::map<int, int> fonts{{7, 70}};
  Resolver r;  // declines

  findOrResolveFont(fonts, 8, r.hook());

  EXPECT_EQ(1u, fonts.size());
  EXPECT_EQ(0u, fonts.count(8));
}
```

- [x] **Step 2: Run it to verify it fails**

```bash
cmake -S test -B build/test && cmake --build build/test --target FontLookupTest
```

Expected: a compile error, `FontLookup.h: No such file or directory`.

- [x] **Step 3: Create `lib/GfxRenderer/FontLookup.h`**

```cpp
#pragma once

#include <map>
#include <optional>

/// A font ID the renderer does not know yet can be supplied on demand. The resolver returns the
/// family for `fontId`, or std::nullopt when the ID is not one it serves. Used for TTF size-ladder
/// rungs, which exist as IDs long before anything draws with them.
template <typename Family>
struct FontResolverHook {
  std::optional<Family> (*resolve)(void* ctx, int fontId) = nullptr;
  void* ctx = nullptr;
};

/// Looks `fontId` up in `fonts`. On a miss, asks `hook` once; a family it supplies is registered
/// and returned. Returns nullptr when neither knows the ID. A hit costs exactly a map find.
template <typename Family>
const Family* findOrResolveFont(std::map<int, Family>& fonts, const int fontId, const FontResolverHook<Family>& hook) {
  const auto it = fonts.find(fontId);
  if (it != fonts.end()) return &it->second;
  if (!hook.resolve) return nullptr;
  std::optional<Family> family = hook.resolve(hook.ctx, fontId);
  if (!family) return nullptr;
  return &fonts.emplace(fontId, *family).first->second;
}
```

- [x] **Step 4: Run the test to verify it passes**

```bash
cmake --build build/test --target FontLookupTest && build/test/font_lookup/FontLookupTest
```

Expected: `[  PASSED  ] 5 tests.`

- [x] **Step 5: Route `GfxRenderer` through it**

In `lib/GfxRenderer/GfxRenderer.h`:

- Add `#include "FontLookup.h"` to the local includes and `#include <optional>` to the standard ones.
- Change `std::map<int, EpdFontFamily> fontMap;` to:

```cpp
  // Mutable because a lookup from a const draw or measure call may register a font the resolver
  // supplies on demand (findFontFamily). Same compromise as sdCardFonts_ below.
  mutable std::map<int, EpdFontFamily> fontMap;
  FontResolverHook<EpdFontFamily> fontResolver_;
```

- Add to the **public** section, next to `getFontMap()`:

```cpp
  /// Lets a font source supply families for IDs nobody registered yet, on first use. The
  /// resolver is asked once per unknown ID; what it returns is registered like insertFont().
  void setFontResolver(std::optional<EpdFontFamily> (*resolve)(void* ctx, int fontId), void* ctx) {
    fontResolver_ = FontResolverHook<EpdFontFamily>{resolve, ctx};
  }
```

- Add to the **private** section:

```cpp
  // The one place a font ID becomes a family: fontMap, then the resolver for an unknown ID.
  const EpdFontFamily* findFontFamily(const int fontId) const {
    return findOrResolveFont(fontMap, fontId, fontResolver_);
  }
```

In `lib/GfxRenderer/GfxRenderer.cpp`, convert all 13 sites. Every site has the same shape and uses the
iterator only as `fontIt->second`. Write this script with the Write tool as
`/c/tmp/stage1_fontlookup.py` (not as a heredoc, which mangles backslashes), then run it:

The local is named `family`. `fontFamily` is already a parameter name in two render helpers in this
file, and `family` is unused.

```python
import pathlib, sys

path = pathlib.Path("lib/GfxRenderer/GfxRenderer.cpp")
src = path.read_text(encoding="utf-8")
find_line = "const auto fontIt = fontMap.find(fontId);"
sites = src.count(find_line)
if sites != 13:
    sys.exit(f"expected 13 fontMap.find sites, found {sites}")
src = src.replace(find_line, "const EpdFontFamily* const family = findFontFamily(fontId);")
src = src.replace("fontIt == fontMap.end()", "!family")
src = src.replace("fontIt->second.", "family->")         # member calls
src = src.replace("= fontIt->second;", "= *family;")      # const auto& font = ...
src = src.replace("(fontIt->second, ", "(*family, ")     # passed as the first argument
for leftover in ("fontIt", "fontMap.find"):
    if leftover in src:
        sys.exit(f"'{leftover}' still present: a site did not match the expected shape; convert it by hand")
path.write_text(src, encoding="utf-8")
print("converted", sites, "sites")
```

```bash
cd /c/_development/witchhunt-reader
python /c/tmp/stage1_fontlookup.py
grep -n "fontMap.find\|fontIt" lib/GfxRenderer/GfxRenderer.cpp
git diff --stat lib/GfxRenderer/GfxRenderer.cpp
```

Expected: `converted 13 sites`, no grep output, and a diff of about 26-39 changed lines.

- [x] **Step 6: Build both firmware boards**

```bash
export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8
"$HOME/.platformio/penv/Scripts/pio.exe" run -e default > /c/tmp/stage1-task4-default.log 2>&1; tail -3 /c/tmp/stage1-task4-default.log
"$HOME/.platformio/penv/Scripts/pio.exe" run -e x4pro > /c/tmp/stage1-task4-x4pro.log 2>&1; tail -3 /c/tmp/stage1-task4-x4pro.log
```

Expected: `[SUCCESS]` for both.

- [x] **Step 7: Commit**

```bash
git add lib/GfxRenderer/FontLookup.h lib/GfxRenderer/GfxRenderer.h lib/GfxRenderer/GfxRenderer.cpp test/font_lookup test/CMakeLists.txt
git commit -m "GfxRenderer: one font lookup with an optional resolver for unknown IDs"
```

---

### Task 5: Verification and pull request

**Files:**
- No source changes, unless a check fails.

- [x] **Step 1: Whole host suite**

```bash
cmake --build build/test -j 8 -- -k 0
ctest --test-dir build/test -j 8 --output-on-failure 2>&1 | tail -5
```

Expected: 100% of built tests pass (the `epub_build_inventory`-only `NOT_BUILT` entries are the known
Windows exception). Note the total for the PR description.

- [x] **Step 2: Flash and size**

Record the `Flash:` and `RAM:` lines from `/c/tmp/stage1-task4-default.log`. Compare them with the
same lines from a `default` build of `master`, or with the 2026-10-10 master baseline (6,271,873 B
flash, 54,616 B static RAM). Expected: within a few hundred bytes. The change removes one pointer from
every `EpdFontData` and adds vtables for two small classes. Put both numbers in the PR description.

- [x] **Step 3: Glyph-path micro-benchmark (needs the X3 on COM6)**

USER ACTION: connect the X3. The bench replaces the reader firmware until Step 5 reflashes it.

```bash
export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8
"$HOME/.platformio/penv/Scripts/pio.exe" run -e bench_font -t upload
```

Capture the serial output with this script. Write it with the Write tool as `/c/tmp/capture.py`.
It opens COM6 with DTR and RTS held low, because toggling them after `open()` resets the C3. It saves
everything until `=== done ===` or 600 s.

```python
import sys, time
import serial  # pyserial, from the PlatformIO penv

port, out_path = sys.argv[1], sys.argv[2]
ser = serial.Serial()
ser.port, ser.baudrate, ser.timeout = port, 115200, 1
ser.dtr = False
ser.rts = False
ser.open()
deadline = time.time() + 600
with open(out_path, "w", encoding="utf-8", errors="replace") as out:
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace")
        if line:
            out.write(line)
            out.flush()
            if "=== done ===" in line:
                break
ser.close()
```

```bash
/c/pio/penv/Scripts/python.exe /c/tmp/capture.py COM6 /c/tmp/bench_font_branch.txt
```

Press the device's reset button, or replug it, right after the script starts, so the run is captured
from the beginning. Repeat on `master`:

```bash
git switch master
"$HOME/.platformio/penv/Scripts/pio.exe" run -e bench_font -t upload
/c/pio/penv/Scripts/python.exe /c/tmp/capture.py COM6 /c/tmp/bench_font_master.txt
git switch refactor/glyph-provider
```

Expected: every `glyph_lookup` and kerning line within ±3% of master. A built-in face has kern
tables, so it never reaches the provider. If a kerning line regresses by more than 3%, the
`hasKernTables` computation in `getKerning` is the suspect; revert it to the two original early
returns with the provider call added to each.

- [x] **Step 4: Device smoke test on the reader firmware (X3)**

```bash
"$HOME/.platformio/penv/Scripts/pio.exe" run -e default -t upload
```

USER ACTION, on the device:

1. Open a book in a **built-in** reader font. Turn 10 pages, then open the reader menu and change the
   font size once.
2. Switch to an **SD-card font family**, reopen the book, and turn 10 pages.
3. Open **Settings → Font** and scroll through the previews of at least two SD families. The previews
   draw glyphs from the miss ring.
4. Look up a word in the **dictionary**.
5. Check that the UI menus and icons render as before.

Expected: no blank, boxed or garbled glyphs, and no reboot. In the serial log, `[SDCF] Overflow:` lines
may appear during previews as before, and there are no new `LOG_ERR` lines.

- [x] **Step 5: Format, push, open the PR**

CI formats with clang-format 21.1.8. Install it once, if `/c/tmp/cf21/bin/clang-format-21.exe` does
not exist yet:

```bash
python -m pip install --target /c/tmp/cf21/pkg clang-format==21.1.8
mkdir -p /c/tmp/cf21/bin
cp /c/tmp/cf21/pkg/clang_format/data/bin/clang-format.exe /c/tmp/cf21/bin/clang-format-21.exe
```

```bash
PATH="/c/tmp/cf21/bin:$PATH" ./bin/clang-format-fix -g
git status --short
```

Commit any formatting changes (`git commit -am "style: clang-format"`). Then:

```bash
git push -u origin refactor/glyph-provider
gh pr create --title "EpdFont: GlyphProvider replaces the glyph-miss callback; one font lookup in GfxRenderer" --body "$(cat <<'EOF'
Stage 1 of on-device TTF fonts (design: docs/superpowers/specs/2026-10-13-ttf-fonts-design.md on docs/ttf-fonts-design). No behaviour change.

- `GlyphProvider` replaces `EpdFontData::glyphMissHandler` + `glyphMissCtx`. `SdCardFont` implements it per style; `GfxRenderer::getGlyphBitmap` asks `ownsBitmap()` instead of casting the context to `SdCardFont`.
- `EpdFont::getKerning` asks the provider only when a font has no kern tables.
- `GfxRenderer`'s 13 font lookups go through one helper that can ask an optional resolver for an unknown ID (unused until TTF ladder rungs land).

Tests: new GlyphProviderTest (6), FontLookupTest (5), two SdFontArena cases; full host suite <N>/<N>. Builds: default, x4pro. bench_font glyph_lookup/kerning within <x>% of master. Device smoke on X3: built-in and SD fonts, previews, dictionary, UI.
EOF
)"
```

Before running it, replace `<N>` with the test total from Step 1 and `<x>` with the largest
`bench_font` deviation from Step 3.
