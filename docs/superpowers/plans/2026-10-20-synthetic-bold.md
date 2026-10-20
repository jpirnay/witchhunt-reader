# Synthetic bold for TrueType families — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bold and bold italic text in a TrueType family that has no bold file of its own draws emboldened, as FreeType's synthetic bold does, so headings and bold spans look bold in the CJK families (Regular only), in regular-only Latin families and in fonts books embed.

**Architecture:** `ttf::Rasterizer` gains `embolden()`: FreeType 2.13.3's `FT_Outline_EmboldenXY` rebuilt in integers, bit for bit, run between `load()` and `rasterize()`, with FreeType's strength across and none down (`ttf::emboldenStrength`). `TtfFont` separates the fonts it serves (one per drawn style, in every size) from the files that draw them, decides once per load which styles are synthetic (only when no file of the family is bold), keys their glyphs under their own style bits with the face index of their size, and kerns them with their file's pairs. The section version moves, because bold text in such a family gets wider.

**Tech Stack:** C++17 firmware (PlatformIO, ESP32-C3/S3), host tests with GoogleTest/CMake (`test/`) against FreeType 2.13.3 (the SDK's copy), Python only to cut a test font.

**Spec:** `docs/superpowers/specs/2026-10-20-cjk-ttf-fonts-design.md`, section 7 (synthetic bold), with sections 9 and 10. The plan was cut from a prototype (`proto/synthetic-bold`, worktree `C:/tmp/wt-bold-proto`): every code block below is taken from it, and it passes every test this plan names. CFF outlines (a later plan) are emboldened by the same code: it works on any outline the loader leaves in the scratch block.

## Global Constraints

- Base: master at 67bb1e428 or later.
- Integer only: the engine links no floating point (`scripts/check_no_float.py` runs after every firmware link).
- Regular glyphs, and every glyph of a family with a real bold file, stay byte-identical: `TtfVsFreeType.EveryGlyphMatchesFreeType` and `TtfGolden.TheDeviceBenchExpectsThisOutput` must pass unchanged.
- The strength is one function, `ttf::emboldenStrength(ppem26_6)`: across, FreeType's `x_ppem * 0x0AAA / 1024` (about em/24); down, 0 (spec 7.2 allows a smaller vertical strength; Task 2 shows why it is 0).
- `ttf::kRenderVersion` stays 2: until now a regular-only family's bold fell back to the regular's or italic's font, whose glyphs carry those styles' bits, so no store holds a glyph under a synthetic style's key. `SECTION_FILE_VERSION` goes up by one (96 to 97 on the prototype's base; one more than master's when this lands).
- No setting and no new strings (spec 7.1: no switch to turn it off).
- Comments say what the code does and why; no history narration.
- clang-format 21 (`bin/clang-format-fix` with clang-format 21.1.8 on the PATH) before pushing; version 22 differs.
- Host tests from Git Bash (`-DFETCHCONTENT_SOURCE_DIR_SDFAT=/c/tmp/SdFat-2.3.1` at configure; `cmake --build build/test -j 8 -- -k 0`).
- Firmware: `default`, `x4pro` and `lilygo_t5s3` must build, one `pio run` at a time, `PLATFORMIO_CORE_DIR=C:/pio`.
- Devices: never pulse RTS or run esptool to reset an X3; GET each setting before changing it and restore it afterwards (the X4 runs in German).

## Review Focus

1. **A book that embeds a regular-only font** (Publisher Fonts, an accent family): its bold spans are now synthetic, which spec 7.1 wants, and its layout widens. The reviewer checks that accent families go through the same `TtfFont::openStyles`; Task 5 reads such a book.
2. **A family of Regular and Italic only**: bold italic is drawn from the italic, bold from the regular. Pinned in Task 3: `TtfFontTest.BoldItalicIsEmboldenedFromTheItalicWhenThereIsOne`.
3. **The largest native size** (120 px headings, drop caps): an emboldened glyph is wider than the scratch block was planned for. `planScratch(..., emboldened = true)` adds twice the strength across. Pinned in Task 3: `TtfFontTest.SyntheticBoldDrawsEveryGlyphAtTheLargestNativeSize`. That test does not fail without the margin on Bookerly (its head box leaves room) or on CJK faces (head box under the em): the margin is insurance for a glyph that fills its box, and the reviewer judges it by reading `planScratch`.
4. **Small caps, superscript and subscript in bold**: the style bits above 1 never reach `TtfFont` (`EpdFontFamily::getFont` masks them), so a synthetic bold small-caps run uses the bold font. The reviewer confirms nothing in `TtfFont` reads those bits.
5. **A store written by an older build**: no key a synthetic style uses can be in it (see the constraint on `kRenderVersion`), and a family with a real Bold draws as before. The reviewer checks the reasoning against `TtfFont::storeKey`.

---

### Task 1: The emboldening, bit for bit with FreeType

**Files:**
- Modify: `lib/EpdFont/ttf/TtfRasterizer.h`, `lib/EpdFont/ttf/TtfRasterizer.cpp`
- Modify: `test/ttf_rasterizer/FreeTypeReference.h`, `test/ttf_rasterizer/FreeTypeReference.cpp`
- Test: `test/ttf_rasterizer/TtfVsFreeTypeTest.cpp`

**Interfaces:**
- Produces: `struct ttf::Strength { int32_t x = 0; int32_t y = 0; };` (26.6 pixels: the outline's growth on each axis) and `ttf::Strength ttf::emboldenStrength(uint32_t ppem26_6)`.
- Produces: `ttf::Status ttf::Rasterizer::embolden(const Strength& strength, EpdGlyph* metrics)`.
- Produces: `ReferenceGlyph ttf_test::FreeTypeReference::glyph(uint32_t index, bool oversampleOverlaps = false, long xStrength = 0, long yStrength = 0) const`.

- [ ] **Step 1: Let the oracle embolden.** `FreeTypeReference::glyph` takes the two strengths; in the `.cpp`, right after `out.linearAdvance = ...`:

```cpp
  if (xStrength != 0 || yStrength != 0) FT_Outline_EmboldenXY(&slot->outline, xStrength, yStrength);
```

The header's comment on `glyph` gains: "A non-zero strength (26.6) emboldens the outline first, with FT_Outline_EmboldenXY."

- [ ] **Step 2: Write the failing test.** In `TtfVsFreeTypeTest.cpp`, declare `void compareEmboldened(uint32_t points, bool bothAxes);` in the fixture beside `compareEveryGlyph`, and add after `TtfVsFreeType::compareEveryGlyph`:

```cpp
TEST_P(TtfVsFreeType, EveryGlyphEmboldenedMatchesFreeType) {
  // Synthetic bold (spec 7.3): FreeType's FT_Outline_EmboldenXY is the oracle, every glyph at the three sizes, box,
  // advance and bitmap byte for byte: at the engine's strength, and at FreeType's own (both axes), so the port's
  // vertical half is held to it too.
  for (const uint32_t points : {8u, kPoints, 40u}) {
    for (const bool both : {false, true}) {
      SCOPED_TRACE(testing::Message() << points << " pt" << (both ? ", both axes" : ""));
      compareEmboldened(points, both);
    }
  }
}

void TtfVsFreeType::compareEmboldened(const uint32_t points, const bool bothAxes) {
  const FreeTypeReference reference(data, points);
  ASSERT_TRUE(reference.ok());
  const uint32_t ppem = ppemFromPoints(points);
  Strength strength = emboldenStrength(ppem);
  ASSERT_GT(strength.x, 0);
  if (bothAxes) strength.y = strength.x;
  Rasterizer rasterizer;
  ASSERT_TRUE(rasterizer.allocate(planScratch(face, ppem, kPoolBytesFlash)));
  uint32_t glyphs = 0, tooLarge = 0, failures = 0, advanceDiffs = 0, boxDiffs = 0, exactDiffs = 0;
  for (uint32_t g = 0; g < face.numGlyphs; ++g) {
    const ReferenceGlyph expected = reference.glyph(g, false, strength.x, strength.y);
    if (!expected.loaded) continue;
    ++glyphs;
    EpdGlyph metrics{};
    Status status = rasterizer.load(bytes, face, static_cast<uint16_t>(g), ppem, &metrics);
    if (status == Status::Ok) status = rasterizer.embolden(strength, &metrics);
    if (status == Status::Ok) status = rasterizer.rasterize(rasterizer.output());
    if (expected.width > 255 || expected.height > 255 ||
        glyphDataBytes(expected.width, expected.height, true) > rasterizer.plan().outputBytes) {
      EXPECT_EQ(Status::Unsupported, status) << "glyph " << g;
      ++tooLarge;
      continue;
    }
    if (status != Status::Ok) {
      ++failures;
      ADD_FAILURE() << "glyph " << g << ": " << statusName(status);
      continue;
    }
    const uint16_t advance =
        static_cast<uint16_t>(((expected.linearAdvance + 2048) >> 12) + (static_cast<uint32_t>(strength.x) + 2) / 4);
    advanceDiffs += metrics.advanceX != advance;
    if (metrics.width != expected.width || metrics.height != expected.height || metrics.left != expected.left ||
        metrics.top != expected.top) {
      if (++boxDiffs <= 3) {
        ADD_FAILURE() << "glyph " << g << " box " << int(metrics.width) << "x" << int(metrics.height) << " at "
                      << metrics.left << "," << metrics.top << ", FreeType's " << expected.width << "x"
                      << expected.height << " at " << expected.left << "," << expected.top;
      }
      continue;
    }
    if (!expected.bitmap.empty() &&
        std::memcmp(rasterizer.output(), expected.bitmap.data(), expected.bitmap.size()) != 0 &&
        ++exactDiffs <= 3) {
      ADD_FAILURE() << "glyph " << g << ", ours then FreeType's:\n"
                    << picture(metrics, rasterizer.output()) << "---\n"
                    << picture(metrics, expected.bitmap.data());
    }
  }
  std::printf(
      "%s @%u pt emboldened by %d,%d/64: %u glyphs (%u too large), %u failures, %u advance, %u box, %u inexact\n",
      GetParam().c_str(), points, strength.x, strength.y, glyphs, tooLarge, failures, advanceDiffs, boxDiffs,
      exactDiffs);
  EXPECT_GT(glyphs, 1000u);
  EXPECT_EQ(0u, failures);
  EXPECT_EQ(0u, advanceDiffs);
  EXPECT_EQ(0u, boxDiffs);
  EXPECT_EQ(0u, exactDiffs);
}
```

- [ ] **Step 3: Run it to see it fail.**

Run: `cmake --build build/test --target TtfRasterizerTest -j 8`
Expected: the build fails: `Strength`, `emboldenStrength` and `Rasterizer::embolden` are not declared.

- [ ] **Step 4: Declare the interface** in `TtfRasterizer.h`, after `mergePlans`:

```cpp
/// How much synthetic bold grows a glyph (spec 7.2): the outline's growth on each axis, 26.6 pixels.
struct Strength {
  int32_t x = 0;
  int32_t y = 0;
};
/// Synthetic bold's strength at `ppem26_6`.
Strength emboldenStrength(uint32_t ppem26_6);
```

and in `class Rasterizer`, after `load()`:

```cpp
  /// Synthetic bold (spec 7.2): emboldens the outline of the last load() exactly as FreeType's
  /// FT_Outline_EmboldenXY does with `strength`, and updates its `metrics` the way FT_GlyphSlot_AdjustWeight
  /// does: the bitmap box from the new outline (the left edge and the baseline stay where they were), the
  /// advance strength.x wider (12.4, rounded). An outline FreeType cannot orient stays as it is, the advance
  /// growing all the same. After a failed load() it returns that status; a bitmap that no longer fits reports
  /// what load() would.
  Status embolden(const Strength& strength, EpdGlyph* metrics);
```

The private part keeps where the last `load()` moved the outline, and the box step becomes a member both use:

```cpp
  // The last load(): its status, outline size, size and bitmap box, whose bottom left corner the outline was moved to.
  Status loaded_ = Status::Malformed;
  uint16_t numPoints_ = 0;
  uint16_t numContours_ = 0;
  uint16_t width_ = 0;
  uint16_t height_ = 0;
  int32_t left_ = 0;
  int32_t bottom_ = 0;
  uint32_t ppem_ = 0;

  Status placeBox(EpdGlyph* metrics);
```

- [ ] **Step 5: Port FreeType's emboldening** into `TtfRasterizer.cpp`'s anonymous namespace, after `inRange()`. These three functions are FreeType 2.13.3's (`ftcalc.c` `FT_Vector_NormLen`, `ftoutln.c` `FT_Outline_Get_Orientation` and `FT_Outline_EmboldenXY`), statement for statement, with `TtfFixed.h`'s `mulFix`/`mulDiv` for `FT_MulFix`/`FT_MulDiv` and `int64_t` for the orientation's area (FreeType's `FT_Long` on a 64-bit build). Copy them exactly:

```cpp
// --- Synthetic bold: FreeType 2.13.3's outline emboldening (ftoutln.c, ftcalc.c), bit for bit ---

// FT_Vector_NormLen: `v` scaled to unit length (16.16), and its length returned. FreeType's own integer Newton
// iteration, in its own 32-bit types: the wrap-arounds it relies on are the same on every target.
int msb(const uint32_t v) { return 31 - __builtin_clz(v); }  // FT_MSB; v is never 0

uint32_t normLen(FT_Vector* v) {
  int32_t x_ = static_cast<int32_t>(v->x);
  int32_t y_ = static_cast<int32_t>(v->y);
  int sx = 1;
  int sy = 1;
  uint32_t x = x_ < 0 ? 0u - static_cast<uint32_t>(x_) : static_cast<uint32_t>(x_);
  uint32_t y = y_ < 0 ? 0u - static_cast<uint32_t>(y_) : static_cast<uint32_t>(y_);
  if (x_ < 0) sx = -1;
  if (y_ < 0) sy = -1;
  if (x == 0) {
    if (y > 0) v->y = sy * 0x10000;
    return y;
  }
  if (y == 0) {
    if (x > 0) v->x = sx * 0x10000;
    return x;
  }
  // Prenormalised by shifting, so the estimated length lies between 2/3 and 4/3.
  uint32_t l = x > y ? x + (y >> 1) : y + (x >> 1);
  int shift = 31 - msb(l);
  shift -= 15 + (l >= (0xAAAAAAAAu >> shift) ? 1 : 0);
  if (shift > 0) {
    x <<= shift;
    y <<= shift;
    l = x > y ? x + (y >> 1) : y + (x >> 1);
  } else {
    x >>= -shift;
    y >>= -shift;
    l >>= -shift;
  }
  int32_t b = 0x10000 - static_cast<int32_t>(l);
  x_ = static_cast<int32_t>(x);
  y_ = static_cast<int32_t>(y);
  uint32_t u = 0;
  uint32_t w = 0;
  int32_t z = 0;
  do {  // Newton's iterations
    u = static_cast<uint32_t>(x_ + (x_ * b >> 16));
    w = static_cast<uint32_t>(y_ + (y_ * b >> 16));
    z = -static_cast<int32_t>(u * u + w * w) / 0x200;
    z = z * ((0x10000 + b) >> 8) / 0x10000;
    b += z;
  } while (z > 0);
  v->x = sx < 0 ? -static_cast<FT_Pos>(u) : static_cast<FT_Pos>(u);
  v->y = sy < 0 ? -static_cast<FT_Pos>(w) : static_cast<FT_Pos>(w);
  l = static_cast<uint32_t>(0x10000 + static_cast<int32_t>(u * x + w * y) / 0x10000);
  if (shift > 0) {
    l = (l + (1u << (shift - 1))) >> shift;
  } else {
    l <<= -shift;
  }
  return l;
}

enum class Orientation : uint8_t { TrueType, PostScript, None };

// FT_Outline_Get_Orientation: the sign of the control polygon's area, by the nonzero winding rule.
Orientation orientation(const FT_Vector* points, const unsigned short* contours, const uint16_t numContours,
                        const uint16_t numPoints) {
  if (numPoints == 0) return Orientation::TrueType;
  FT_Pos xMin = points[0].x;
  FT_Pos yMin = points[0].y;
  FT_Pos xMax = xMin;
  FT_Pos yMax = yMin;
  for (uint16_t i = 1; i < numPoints; ++i) {
    xMin = points[i].x < xMin ? points[i].x : xMin;
    yMin = points[i].y < yMin ? points[i].y : yMin;
    xMax = points[i].x > xMax ? points[i].x : xMax;
    yMax = points[i].y > yMax ? points[i].y : yMax;
  }
  if (xMin == xMax || yMin == yMax) return Orientation::None;
  if (xMin < -0x1000000 || yMin < -0x1000000 || xMax > 0x1000000 || yMax > 0x1000000) return Orientation::None;
  const auto absPos = [](const FT_Pos p) { return static_cast<uint32_t>(p < 0 ? -p : p); };
  int xshift = msb(absPos(xMax) | absPos(xMin)) - 14;
  xshift = xshift > 0 ? xshift : 0;
  int yshift = msb(static_cast<uint32_t>(yMax - yMin)) - 14;
  yshift = yshift > 0 ? yshift : 0;
  int64_t area = 0;
  int last = -1;
  for (uint16_t c = 0; c < numContours; ++c) {
    const int first = last + 1;
    last = contours[c];
    int64_t prevX = points[last].x >> xshift;
    int64_t prevY = points[last].y >> yshift;
    for (int n = first; n <= last; ++n) {
      const int64_t curX = points[n].x >> xshift;
      const int64_t curY = points[n].y >> yshift;
      area += (curY - prevY) * (curX + prevX);
      prevX = curX;
      prevY = curY;
    }
  }
  return area > 0 ? Orientation::PostScript : area < 0 ? Orientation::TrueType : Orientation::None;
}

// FT_Outline_EmboldenXY: every point moves outwards along the bisector of its two edges, by half the strength on each
// axis, less where the edges are short or the turn is sharp, and the whole outline by half the strength up and to the
// right, so the left edge and the baseline stay. False, the outline unchanged, where FreeType returns an error.
bool emboldenXY(FT_Vector* points, const unsigned short* contours, const uint16_t numContours,
                const uint16_t numPoints, int32_t xstrength, int32_t ystrength) {
  xstrength /= 2;
  ystrength /= 2;
  if (xstrength == 0 && ystrength == 0) return true;
  const Orientation orient = orientation(points, contours, numContours, numPoints);
  if (orient == Orientation::None) return numContours == 0;
  int last = -1;
  for (uint16_t c = 0; c < numContours; ++c) {
    const int first = last + 1;
    last = contours[c];
    FT_Vector in{0, 0};
    FT_Vector out{0, 0};
    FT_Vector anchor{0, 0};
    FT_Vector shift{0, 0};
    int32_t lIn = 0;
    int32_t lOut = 0;
    int32_t lAnchor = 0;
    // j cycles through the points; i advances only when points are moved; the anchor k marks the first moved point.
    for (int i = last, j = first, k = -1; j != i && i != k; j = j < last ? j + 1 : first) {
      if (j != k) {
        out.x = points[j].x - points[i].x;
        out.y = points[j].y - points[i].y;
        lOut = static_cast<int32_t>(normLen(&out));
        if (lOut == 0) continue;
      } else {
        out = anchor;
        lOut = lAnchor;
      }
      if (lIn != 0) {
        if (k < 0) {
          k = i;
          anchor = in;
          lAnchor = lIn;
        }
        const auto ix = static_cast<int32_t>(in.x);
        const auto iy = static_cast<int32_t>(in.y);
        const auto ox = static_cast<int32_t>(out.x);
        const auto oy = static_cast<int32_t>(out.y);
        int32_t d = mulFix(ix, ox) + mulFix(iy, oy);
        if (d > -0xF000) {  // a turn of less than about 160 degrees
          d += 0x10000;
          // Along the lateral bisector, in the outline's orientation.
          int32_t sx = iy + oy;
          int32_t sy = ix + ox;
          if (orient == Orientation::TrueType) {
            sx = -sx;
          } else {
            sy = -sy;
          }
          // Limited, so collapsing segments are handled.
          int32_t q = mulFix(ox, iy) - mulFix(oy, ix);
          if (orient == Orientation::TrueType) q = -q;
          const int32_t l = lIn < lOut ? lIn : lOut;
          shift.x = mulFix(xstrength, q) <= mulFix(l, d) ? mulDiv(sx, xstrength, d) : mulDiv(sx, l, q);
          shift.y = mulFix(ystrength, q) <= mulFix(l, d) ? mulDiv(sy, ystrength, d) : mulDiv(sy, l, q);
        } else {
          shift.x = shift.y = 0;
        }
        for (; i != j; i = i < last ? i + 1 : first) {
          points[i].x += xstrength + shift.x;
          points[i].y += ystrength + shift.y;
        }
      } else {
        i = j;
      }
      in = out;
      lIn = lOut;
    }
  }
  return true;
}
```

- [ ] **Step 6: Split `load()`'s box step out and add `embolden()`.** `load()` keeps everything up to and including the advance check; it then stores the outline's size and calls the box step:

```cpp
  metrics->advanceX = static_cast<uint16_t>(advance);
  numPoints_ = loader.numPoints;
  numContours_ = loader.numContours;
  left_ = bottom_ = 0;
  ppem_ = ppem26_6;
  loaded_ = Status::Ok;
  return loaded_ = placeBox(metrics);
}
```

`Rasterizer::placeBox(EpdGlyph* metrics)` is `load()`'s former box code moved as it was, reading `numPoints_` and `ppem_` instead of the loader's and the parameter, returning its two failures without setting `loaded_` (`Status::Unsupported`; `ppem_ > plan_.maxPpem26_6 ? Status::Transient : Status::Unsupported`), translating the points by `-left * 64, -bottom * 64` in a loop over `numPoints_` (the loader is gone by then), remembering `left_ = left; bottom_ = bottom;`, and filling `width`, `height`, `left`, `top`, `dataLength`, `width_`, `height_` as before (`advanceX` stays `load()`'s). Then:

```cpp
Status Rasterizer::embolden(const Strength& strength, EpdGlyph* metrics) {
  if (loaded_ != Status::Ok) return loaded_;
  // FT_GlyphSlot_AdjustWeight: the advance grows by the horizontal strength, whatever became of the outline.
  const uint32_t advance = metrics->advanceX + (static_cast<uint32_t>(strength.x) + 2) / 4;
  if (advance > 0xFFFF) return loaded_ = Status::Unsupported;
  metrics->advanceX = static_cast<uint16_t>(advance);
  if (numPoints_ == 0) return Status::Ok;
  // Back where FreeType has the outline (its orientation reads absolute coordinates), emboldened, boxed again.
  auto* points = reinterpret_cast<FT_Vector*>(block_ + pointsOffset_);
  for (uint16_t i = 0; i < numPoints_; ++i) {
    points[i].x += left_ * 64;
    points[i].y += bottom_ * 64;
  }
  emboldenXY(points, reinterpret_cast<const unsigned short*>(block_ + contoursOffset_), numContours_, numPoints_,
             strength.x, strength.y);
  return loaded_ = placeBox(metrics);
}
```

and, for now, FreeType's own strength on both axes (Task 2 settles it):

```cpp
Strength emboldenStrength(const uint32_t ppem26_6) {
  // FreeType's synthetic bold strength (FT_GlyphSlot_Embolden): whole pixels per em times 0x0AAA / 1024, about em/24.
  const int32_t strength = static_cast<int32_t>((ppem26_6 + 32) >> 6) * 0x0AAA / 1024;
  return {strength, strength};
}
```

- [ ] **Step 7: Run the rasterizer tests.**

Run: `cmake --build build/test --target TtfRasterizerTest -j 8 && ./build/test/ttf_rasterizer/TtfRasterizerTest.exe`
Expected: PASS, every test, `EveryGlyphEmboldenedMatchesFreeType` printing `0 failures, 0 advance, 0 box, 0 inexact` for all 12 bundled fonts at 8, 14 and 40 pt in both runs (Ubuntu's wordmark is 1 too large at 40 pt, as it is unemboldened); `EveryGlyphMatchesFreeType` and `TtfGolden.TheDeviceBenchExpectsThisOutput` unchanged.

- [ ] **Step 8: Commit.**

```bash
git add lib/EpdFont/ttf/TtfRasterizer.h lib/EpdFont/ttf/TtfRasterizer.cpp test/ttf_rasterizer/FreeTypeReference.h \
  test/ttf_rasterizer/FreeTypeReference.cpp test/ttf_rasterizer/TtfVsFreeTypeTest.cpp
git commit -m "feat(ttf): synthetic bold, FreeType's outline emboldening in integers"
```

### Task 2: Across only, so dense characters keep their counters

**Files:**
- Create: `test/fixtures/fonts/NotoSansJP-Dense.ttf`, `test/fixtures/fonts/NotoSerifJP-Dense.ttf`, `test/fixtures/fonts/README.md`, `test/fixtures/fonts/OFL.txt`
- Create: `test/ttf_rasterizer/TtfEmboldenTest.cpp`
- Modify: `test/ttf_rasterizer/CMakeLists.txt`
- Modify: `lib/EpdFont/ttf/TtfRasterizer.h`, `lib/EpdFont/ttf/TtfRasterizer.cpp` (`emboldenStrength`, `planScratch`)

**Interfaces:**
- Produces: `ttf::emboldenStrength` returns `{x_ppem * 0x0AAA / 1024, 0}`.
- Produces: `ttf::ScratchPlan ttf::planScratch(const Face& face, uint32_t maxPpem26_6, uint32_t poolBytes, bool emboldened = false)`.

- [ ] **Step 1: The test fonts.** Two small TrueType files with 鬱 書 慶 議 驚, space and `A`, cut from Noto Sans CJK JP and Noto Serif CJK JP (OFL). Make them with fontTools from the region OTFs (`https://github.com/notofonts/noto-cjk/raw/Sans2.004/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf`, sha256 `68a3fc98800b2a27b371f2fb79991daf3633bd89309d4ffaa6946fd587f375b5`; `.../Serif2.003/Serif/OTF/Japanese/NotoSerifCJKjp-Regular.otf`, sha256 `d9854c7a8ef170b5a7932558856fd64eb8de0b007cd823fed6f9f514ad2803d3`): subset to `U+0020,U+0041,U+9B31,U+66F8,U+6176,U+8B70,U+9A5A` with no layout features and no hinting, then convert CFF to TrueType with `fontTools.pens.cu2quPen.Cu2QuPen(pen, 1.0, reverse_direction=True)` into `glyf`, maxp version 1, post format 3 (the recipe of fontTools' `otf2ttf`), and save without recalculating the timestamp. The prototype's files (3,412 and 4,328 B, `C:/tmp/wt-bold-proto/test/fixtures/fonts/`) may be copied instead. `README.md` there says what the files hold, which test reads them, how they were made (the steps above), and the licence: "Noto Sans CJK and Noto Serif CJK: Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'. Licensed under the SIL Open Font License, Version 1.1 (`OFL.txt`)." `OFL.txt` is the noto-cjk repository's `LICENSE` at tag Sans2.004.

- [ ] **Step 2: Write the failing test** `test/ttf_rasterizer/TtfEmboldenTest.cpp`, and add it to `TtfRasterizerTest`'s sources after `TtfVsFreeTypeTest.cpp`, with `TTF_FIXTURE_DIR="${REPO_ROOT}/test/fixtures/fonts"` beside `TTF_FONT_DIR` in its compile definitions:

```cpp
// Synthetic bold on dense CJK characters (spec 7.2): at reading sizes the strength must leave their counters open.
// FreeType's own strength on both axes closed up to 42% of the open space of these characters; the engine's keeps
// at least three quarters of it.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "TtfRasterizer.h"

using namespace ttf;

namespace {

// The blank pixels of a glyph's bitmap: the open space inside its box.
uint32_t blankPixels(const EpdGlyph& glyph, const uint8_t* bits) {
  uint32_t blank = 0;
  for (int p = 0; p < glyph.width * glyph.height; ++p) blank += ((bits[p >> 2] >> (6 - 2 * (p & 3))) & 3) == 0;
  return blank;
}

// test/fixtures/fonts: the five characters cut from the Noto Sans and Serif JP subsets (README.md there).
std::vector<uint8_t> readFixture(const std::string& name) {
  std::ifstream in(std::string(TTF_FIXTURE_DIR) + "/" + name, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(TtfEmbolden, DenseCharactersKeepThreeQuartersOfTheirOpenSpace) {
  // 鬱 書 慶 議 驚: stacks of horizontal strokes a pixel or two apart at 10-14 pt.
  const uint32_t dense[] = {0x9B31, 0x66F8, 0x6176, 0x8B70, 0x9A5A};
  for (const char* name : {"NotoSansJP-Dense.ttf", "NotoSerifJP-Dense.ttf"}) {
    const std::vector<uint8_t> data = readFixture(name);
    ASSERT_FALSE(data.empty()) << name;
    FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
    Face face;
    ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
    for (const uint32_t points : {10u, 12u, 14u}) {
      const uint32_t ppem = ppemFromPoints(points);
      Rasterizer rasterizer;
      ASSERT_TRUE(rasterizer.allocate(planScratch(face, ppem, kPoolBytesFlash, true)));
      for (const uint32_t cp : dense) {
        const uint16_t glyph = glyphIndex(bytes, face, cp);
        ASSERT_NE(0, glyph);
        EpdGlyph regular{};
        ASSERT_EQ(Status::Ok, rasterizer.render(bytes, face, glyph, ppem, &regular));
        const uint32_t open = blankPixels(regular, rasterizer.output());
        EpdGlyph bold{};
        ASSERT_EQ(Status::Ok, rasterizer.load(bytes, face, glyph, ppem, &bold));
        ASSERT_EQ(Status::Ok, rasterizer.embolden(emboldenStrength(ppem), &bold));
        ASSERT_EQ(Status::Ok, rasterizer.rasterize(rasterizer.output()));
        const uint32_t kept = blankPixels(bold, rasterizer.output());
        std::printf("%s %u pt U+%04X: %u of %u blank pixels kept\n", name, points, cp, kept, open);
        EXPECT_GE(kept * 4, open * 3) << name << " " << points << " pt U+" << std::hex << cp;
      }
    }
  }
}
```

- [ ] **Step 3: Run it to see it fail.**

Run: `cmake --build build/test --target TtfRasterizerTest -j 8`
Expected: the build fails on `planScratch`'s fourth argument. Add it (Step 4's `planScratch` alone), build again and run `./build/test/ttf_rasterizer/TtfRasterizerTest.exe --gtest_filter='TtfEmbolden*'`.
Expected: FAIL, 19 of the 30 cases under three quarters with FreeType's strength on both axes.

- [ ] **Step 4: Room for the growth, and the strength across only.** `planScratch` takes `bool emboldened` and, when set, widens the planned box by twice the strength on each axis before the em and 255 clamps:

```cpp
  if (emboldened) {
    const Strength grow = emboldenStrength(plan.maxPpem26_6);
    width += 2 * ((grow.x + 63) >> 6);
    height += 2 * ((grow.y + 63) >> 6);
  }
```

Its comment in the header gains: "`emboldened`: with room for synthetic bold's growth too, twice its strength across and down (a sharp corner reaches past the strength)." And `emboldenStrength` becomes:

```cpp
Strength emboldenStrength(const uint32_t ppem26_6) {
  // FreeType's synthetic bold strength (FT_GlyphSlot_Embolden: whole pixels per em times 0x0AAA / 1024, about
  // em/24), across only. At reading sizes a dense CJK character stacks its horizontal strokes a pixel or two apart, and
  // any vertical growth closed them: 10-42% of the open space of 鬱書慶議驚 at 10-14 pt went, against at most 22%
  // across only. The stems carry the weight, in Latin text too.
  return {static_cast<int32_t>((ppem26_6 + 32) >> 6) * 0x0AAA / 1024, 0};
}
```

- [ ] **Step 5: Run the rasterizer tests.**

Run: `cmake --build build/test --target TtfRasterizerTest -j 8 && ./build/test/ttf_rasterizer/TtfRasterizerTest.exe`
Expected: PASS, every test; `TtfEmbolden` prints its 30 lines, the lowest 鬱 in sans at 12 pt (198 of 252 blank pixels kept); `EveryGlyphEmboldenedMatchesFreeType` passes at the new strength and at both axes.

- [ ] **Step 6: Commit.**

```bash
git add test/fixtures/fonts test/ttf_rasterizer/TtfEmboldenTest.cpp test/ttf_rasterizer/CMakeLists.txt \
  lib/EpdFont/ttf/TtfRasterizer.h lib/EpdFont/ttf/TtfRasterizer.cpp
git commit -m "feat(ttf): synthetic bold grows across only, so dense CJK characters stay open"
```

### Task 3: Which styles are synthetic, and how TtfFont draws them

**Files:**
- Modify: `lib/EpdFont/ttf/TtfFace.h`, `lib/EpdFont/ttf/TtfFace.cpp` (`Face::bold`)
- Modify: `lib/EpdFont/ttf/TtfFont.h`, `lib/EpdFont/ttf/TtfFont.cpp`
- Test: `test/ttf_font/TtfFontTest.cpp`

**Interfaces:**
- Consumes: Task 1's `Rasterizer::embolden`, Task 2's `emboldenStrength` and `planScratch(..., true)`.
- Produces: `bool ttf::Face::bold` (head.macStyle bit 0, or OS/2 usWeightClass of 600 or more).
- Produces: `bool TtfFont::synthetic(uint8_t style) const`; `const EpdFont* TtfFont::font(uint8_t style) const` and `nativeFonts()` return a font for a synthetic style.

The rule (spec 7.1): a family gets synthetic bold only when none of its files is bold. A file in the bold or bold italic slot is bold (`assignStyles` puts a bold-declared file there, or a heavy file serving bold too); a face read as bold (`Face::bold`) is one too, which covers a Bold moved into the regular slot and a single heavy face. Then bold is drawn from the regular's file, and bold italic from the italic's when the family has one, else from the regular's. A style with a file of its own is never synthesized.

- [ ] **Step 1: Write the failing tests.** In `TtfFontTest.cpp`: add `constexpr uint8_t kBoldItalic = 3;` beside `kItalic`; after `reference()`, the reference for a synthetic glyph:

```cpp
/// What the rasterizer draws for `cp` at a pixels-per-em (26.6) as synthetic bold: what a synthetic style's glyph must
/// equal.
Reference emboldenedAt(const std::vector<uint8_t>& file, const uint32_t cp, const uint32_t ppem) {
  FontBytes bytes(file.data(), static_cast<uint32_t>(file.size()));
  ttf::Face face;
  EXPECT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  ttf::Rasterizer rasterizer;
  EXPECT_TRUE(rasterizer.allocate(
      ttf::planScratch(face, static_cast<uint32_t>(TtfFont::kMaxNativePx) << 6, ttf::kPoolBytesFlash, true)));
  Reference ref;
  EXPECT_EQ(ttf::Status::Ok, rasterizer.load(bytes, face, ttf::glyphIndex(bytes, face, cp), ppem, &ref.metrics));
  EXPECT_EQ(ttf::Status::Ok, rasterizer.embolden(ttf::emboldenStrength(ppem), &ref.metrics));
  EXPECT_EQ(ttf::Status::Ok, rasterizer.rasterize(rasterizer.output()));
  ref.bitmap.assign(rasterizer.output(), rasterizer.output() + ref.metrics.dataLength);
  return ref;
}

Reference emboldened(const std::vector<uint8_t>& file, const uint32_t cp, const uint8_t points) {
  return emboldenedAt(file, cp, ttf::ppemFromPoints(points));
}
```

In `ABrokenGlyphIsMarkedMissingAndNotTriedAgain`, the last line becomes `EXPECT_EQ(nullptr, font->font(kItalic));  // a one-style family; its bold is synthetic (spec 7.1)`. In `AFamilyHoldsMemoryOnlyForTheStylesItHas`, after `built.extraTables.clear();` add `built.macStyle = 1;  // bold-declared: no style is synthetic, each one is a file (see ASyntheticStyleCostsItsFontsAndNoFile)`. Then, before `TEST_F(TtfFontTest, TheFiLigatureDrawsTheGlyphGsubNames)` (after `kernedBoxes()`, which they use):

```cpp
// Spec 7.1: synthetic bold, only for a family none of whose files is bold.

TEST_F(TtfFontTest, ARegularOnlyFamilyDrawsBoldAndBoldItalicEmboldened) {
  for (int s = 1; s < 4; ++s) files[s].bytes.clear();
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_FALSE(font->synthetic(kRegular));
  EXPECT_TRUE(font->synthetic(kBold));
  EXPECT_FALSE(font->synthetic(kItalic));
  EXPECT_TRUE(font->synthetic(kBoldItalic));
  ASSERT_NE(nullptr, font->font(kBold));
  ASSERT_NE(nullptr, font->font(kBoldItalic));
  EXPECT_NE(font->font(kRegular), font->font(kBold));
  EXPECT_EQ(nullptr, font->font(kItalic));  // nothing draws italic: EpdFontFamily falls back to the regular
  const Reference bold = emboldened(files[kRegular].bytes, 'W', 14);
  expectDrawnAs(bold, *font, kBold, font->glyph(kBold, 'W'));
  expectDrawnAs(bold, *font, kBoldItalic, font->glyph(kBoldItalic, 'W'));
  // The regular glyphs stay as they were.
  expectDrawnAs(reference(files[kRegular].bytes, 'W', 14), *font, kRegular, font->glyph(kRegular, 'W'));
}

TEST_F(TtfFontTest, BoldItalicIsEmboldenedFromTheItalicWhenThereIsOne) {
  files[kBold].bytes.clear();
  files[kBoldItalic].bytes.clear();
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_TRUE(font->synthetic(kBold));
  EXPECT_TRUE(font->synthetic(kBoldItalic));
  expectDrawnAs(emboldened(files[kRegular].bytes, 'W', 14), *font, kBold, font->glyph(kBold, 'W'));
  expectDrawnAs(emboldened(files[kItalic].bytes, 'W', 14), *font, kBoldItalic, font->glyph(kBoldItalic, 'W'));
  expectDrawnAs(reference(files[kItalic].bytes, 'W', 14), *font, kItalic, font->glyph(kItalic, 'W'));
}

TEST_F(TtfFontTest, AFamilyWithABoldFileIsNeverEmboldened) {
  // Regular, Bold and Italic without a BoldItalic: bold italic keeps falling back to the real Bold.
  files[kBoldItalic].bytes.clear();
  const auto font = open();
  ASSERT_NE(nullptr, font);
  for (uint8_t s = 0; s < 4; ++s) EXPECT_FALSE(font->synthetic(s)) << int(s);
  EXPECT_EQ(nullptr, font->font(kBoldItalic));
  const EpdFontFamily family(font->font(0), font->font(1), font->font(2), font->font(3));
  EXPECT_EQ(reference(files[kBold].bytes, 'W', 14).metrics.advanceX,
            family.getGlyph('W', EpdFontFamily::BOLD_ITALIC).advanceX);
}

TEST_F(TtfFontTest, ABoldServingAsTheRegularIsNotEmboldenedAgain) {
  // A family of a Bold alone: assignStyles moves it into the regular slot.
  files[kRegular].bytes = files[kBold].bytes;
  for (int s = 1; s < 4; ++s) files[s].bytes.clear();
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_FALSE(font->synthetic(kBold));
  EXPECT_EQ(nullptr, font->font(kBold));
}

TEST_F(TtfFontTest, ASingleHeavyFaceIsNotEmboldened) {
  // A weight of 600 or more declared regular: a regular with no bold, heavy already.
  Font heavy = kernedBoxes();
  heavy.os2 = true;
  heavy.weightClass = 700;
  useBuiltFont(heavy);
  const auto font = open();
  ASSERT_NE(nullptr, font);
  EXPECT_FALSE(font->synthetic(kBold));
  EXPECT_EQ(nullptr, font->font(kBold));
}

TEST_F(TtfFontTest, SyntheticBoldIsStoredUnderTheBoldStyleAndServesTheNextBoot) {
  for (int s = 1; s < 4; ++s) files[s].bytes.clear();
  {
    const auto font = open();
    ASSERT_NE(nullptr, font);
    ASSERT_NE(nullptr, font->glyph(kBold, 'W'));
    ASSERT_NE(nullptr, font->glyph(kRegular, 'W'));  // another glyph: the regular's key is not the bold's
    EXPECT_EQ(2u, font->renders());
  }
  const auto font = open();  // the next boot
  ASSERT_NE(nullptr, font);
  expectDrawnAs(emboldened(files[kRegular].bytes, 'W', 14), *font, kBold, font->glyph(kBold, 'W'));
  expectDrawnAs(reference(files[kRegular].bytes, 'W', 14), *font, kRegular, font->glyph(kRegular, 'W'));
  EXPECT_EQ(0u, font->renders());
  EXPECT_EQ(2u, font->storeHits());
}

TEST_F(TtfFontTest, SyntheticBoldKernsAsItsFileAndIsWiderByItsStrength) {
  useBuiltFont(kernedBoxes());
  const auto font = open();
  ASSERT_NE(nullptr, font);
  ASSERT_TRUE(font->synthetic(kBold));
  EXPECT_EQ(-47, font->kerning(kRegular, 'A', 'B'));
  EXPECT_EQ(-47, font->kerning(kBold, 'A', 'B'));
  EXPECT_EQ(1u, font->kernLookups());  // the bold pair is the regular's, from the store
  const EpdGlyph* regular = font->glyph(kRegular, 'A');
  const EpdGlyph* bold = font->glyph(kBold, 'A');
  ASSERT_NE(nullptr, regular);
  ASSERT_NE(nullptr, bold);
  const int32_t strength = ttf::emboldenStrength(ttf::ppemFromPoints(14)).x;
  EXPECT_EQ(regular->advanceX + (strength + 2) / 4, bold->advanceX);
  EXPECT_EQ(regular->left, bold->left);  // the left side bearing stays
  EXPECT_EQ(regular->top, bold->top);    // and the baseline: across only (emboldenStrength)
}

TEST_F(TtfFontTest, SyntheticBoldDrawsEveryGlyphAtTheLargestNativeSize) {
  // The scratch block is planned with room for the growth: at the largest size, every glyph drawn in the regular
  // style is drawn emboldened too (a glyph too large for the output region in both stays missing in both).
  for (int s = 1; s < 4; ++s) files[s].bytes.clear();
  const auto font = open();
  ASSERT_NE(nullptr, font);
  const EpdFont* fonts[4] = {};
  ASSERT_TRUE(font->nativeFonts(font->maxNativePx(), fonts));
  const uint32_t ppem = px(font->maxNativePx());
  int drawn = 0;
  for (uint32_t cp = 0x21; cp < 0x250; ++cp) {
    if (nativeGlyph(fonts[kRegular], cp) == nullptr) continue;
    const EpdGlyph* glyph = nativeGlyph(fonts[kBold], cp);
    ASSERT_NE(nullptr, glyph) << std::hex << cp;
    expectDrawnAs(emboldenedAt(files[kRegular].bytes, cp, ppem), *font, kBold, glyph);
    ++drawn;
  }
  EXPECT_GT(drawn, 300);
}

TEST_F(TtfFontTest, ASyntheticStyleCostsItsFontsAndNoFile) {
  // Two families of one file each, one of them bold-declared: the other's synthetic bold and bold italic cost a font
  // in each size, and nothing for a file.
  Font regular = kernedBoxes();
  regular.extraTables.clear();
  Font bold = regular;
  bold.macStyle = 1;
  size_t family[2] = {};
  size_t native[2] = {};
  const uint8_t* const identities[] = {kIdentityA, kIdentityB};
  for (int i = 0; i < 2; ++i) {
    useBuiltFont(i == 0 ? bold : regular);
    ASSERT_NE(nullptr, open(identities[i]));  // installed: what an install or the first mapping keeps is not counted
    const size_t before = heapBytesHeld;
    const auto font = open(identities[i]);
    ASSERT_NE(nullptr, font);
    family[i] = heapBytesHeld - before;
    const EpdFont* fonts[4] = {};
    const size_t beforeNative = heapBytesHeld;
    ASSERT_TRUE(font->nativeFonts(40, fonts));
    native[i] = heapBytesHeld - beforeNative;
    EXPECT_EQ(i == 1, fonts[kBold] != nullptr);
  }
  std::printf("[  heap    ] a one-file family without and with synthetic bold: %zu/%zu B; a native size: %zu/%zu B\n",
              family[0], family[1], native[0], native[1]);
  // Two fonts (about 130 B each) in the body size and in the native one; a file's style would add its tables' records.
  EXPECT_GT(family[1], family[0]);
  EXPECT_LT(family[1] - family[0], 512u);
  EXPECT_GT(native[1], native[0]);
  EXPECT_LT(native[1] - native[0], 512u);
}
```

- [ ] **Step 2: Run them to see them fail.**

Run: `cmake --build build/test --target TtfFontTest -j 8`
Expected: the build fails: `TtfFont` has no member `synthetic`.

- [ ] **Step 3: `Face::bold`.** In `TtfFace.h`, after `longLoca`:

```cpp
  /// A bold face: head.macStyle says bold, or OS/2's weight class is 600 or more. Never emboldened (spec 7.1).
  bool bold = false;
```

In `parseFace`, after `face->longLoca = ...`: `face->bold = (bytes.u16(head.offset + 44) & 1) != 0;  // macStyle`; and after `haveOs2` is settled: `if (haveOs2 && bytes.u16(os2.offset + 4) >= 600) face->bold = true;  // usWeightClass`.

- [ ] **Step 4: Fonts apart from files in `TtfFont.h`.** Public, after `font()` (whose comment gains "A family none of whose files is bold draws bold, and bold italic without a file of its own, emboldened (spec 7.1)."):

```cpp
  /// `style` is drawn emboldened from another style's file.
  bool synthetic(const uint8_t style) const { return style < 4 && (synthetic_ & (1u << style)) != 0; }
```

Private: `styleAt` and `draws` follow the drawing file, and a loader helper and the decision are declared:

```cpp
  // The file that draws `style`: its own, or the one a synthetic style is emboldened from; nullptr when none does.
  Style* styleAt(const uint8_t style) const {
    return style < 4 && source_[style] != kNoSlot ? &styles_[fileSlot_[source_[style]]] : nullptr;
  }
  // Whether the family draws `style`: a file the engine reads draws it.
  bool draws(const uint8_t style) const {
    const Style* const found = styleAt(style);
    return found && found->present;
  }
  // Which styles to embolden (spec 7.1), once the files' faces are read.
  void chooseSynthetic();
  ...
  // Loads `index` of the file drawing `style` into the scratch block, emboldened when the style is synthetic.
  ttf::Status load(FontBytes& bytes, const ttf::Face& face, uint16_t index, const Size& size, uint8_t style,
                   EpdGlyph* metrics);
```

and the members:

```cpp
  // Only the styles the family has a file for, a book's accent family often one or two (each ~260 B): fileSlot_ gives a
  // file's place in styles_. Every style the family draws has a font in every size (~130 B each), where slot_ says,
  // and source_ names the style whose file draws it: its own, or for a synthetic style the regular's or the italic's.
  // Made at load.
  static constexpr uint8_t kNoSlot = 0xFF;
  std::unique_ptr<Style[]> styles_;
  uint8_t fileSlot_[4] = {kNoSlot, kNoSlot, kNoSlot, kNoSlot};
  uint8_t slot_[4] = {kNoSlot, kNoSlot, kNoSlot, kNoSlot};
  uint8_t source_[4] = {kNoSlot, kNoSlot, kNoSlot, kNoSlot};
  uint8_t styleCount_ = 0;
  uint8_t fontCount_ = 0;
  uint8_t synthetic_ = 0;  // bit s: style s is emboldened
```

The comment on `Size::styles` becomes "One for each style the family draws, where slot_ says: the body's made at load, a native size's with it."

- [ ] **Step 5: The decision and the drawing in `TtfFont.cpp`.**
  - `openStyles`: the first-load block sets `fileSlot_[s]` and `source_[s] = s` for each style with a file, allocates `styles_` only (no longer `body_.styles`); the loop takes `styles_[fileSlot_[s]]`. After `if (!draws(0)) return StylesResult::Failed;`:

```cpp
  if (!body_.styles) {
    // The body size's fonts, one for every style the family draws, synthetic ones too.
    chooseSynthetic();
    body_.styles = makeUniqueNoThrow<Size::StyleFont[]>(fontCount_);
    if (!body_.styles) {
      LOG_ERR("TTF", "OOM: family %u's %u fonts", family_, fontCount_);
      return StylesResult::Failed;
    }
  }
  for (uint8_t s = 0; s < 4; ++s) {
    if (!synthetic(s) || !draws(s)) continue;
    // A synthetic style's glyphs are wider than its file's: the block has room for them at the largest size.
    const Style& style = *styleAt(s);
    const uint32_t largest = std::max(ttf::ppemFromPoints(kMaxPointSize), static_cast<uint32_t>(kMaxNativePx) << 6);
    const uint32_t pool = style.onCard && family_ == 0 ? ttf::kPoolBytesSd : ttf::kPoolBytesFlash;
    *plan = ttf::mergePlans(*plan, ttf::planScratch(style.face, largest, pool, true));
  }
  return StylesResult::Ok;
}

void TtfFont::chooseSynthetic() {
  // Spec 7.1: only a family none of whose files is bold gets synthetic bold. A file in the bold or bold italic slot is
  // bold (assignStyles puts a bold-declared file there, or a heavy one serving bold too); a face read as bold
  // (macStyle, or a weight of 600 or more) is one too, as a Bold moved into the regular slot or a single heavy face.
  // Anything else would embolden a bold face again, or draw bold italic from the italic beside a real Bold.
  bool boldFile = fileSlot_[1] != kNoSlot || fileSlot_[3] != kNoSlot;
  for (uint8_t s = 0; s < 4; ++s) boldFile = boldFile || (draws(s) && styleAt(s)->face.bold);
  synthetic_ = 0;
  if (!boldFile) {
    source_[1] = 0;
    source_[3] = draws(2) ? 2 : 0;
    synthetic_ = static_cast<uint8_t>(1u << 1 | 1u << 3);
  }
  fontCount_ = 0;
  for (uint8_t s = 0; s < 4; ++s) slot_[s] = source_[s] != kNoSlot ? fontCount_++ : kNoSlot;
}
```

  - `rebase`: `for (uint8_t s = 0; s < 4; ++s) { if (draws(s)) size.styles[slot_[s]].data.bitmap = set_.store_.logBase(); }`.
  - `nativeSize`: the new size's fonts are `makeUniqueNoThrow<Size::StyleFont[]>(fontCount_)`.
  - `render` and `prefetch`: `placeable(set.rasterizer_.load(...), metrics)` becomes `load(bytes, face, index, size, style, &metrics)` (prefetch: `p.glyph`), with:

```cpp
ttf::Status TtfFont::load(FontBytes& bytes, const ttf::Face& face, const uint16_t index, const Size& size,
                          const uint8_t style, EpdGlyph* metrics) {
  ttf::Rasterizer& rasterizer = set_.rasterizer_;
  ttf::Status loaded = rasterizer.load(bytes, face, index, size.ppem, metrics);
  if (loaded == ttf::Status::Ok && synthetic(style)) {
    loaded = rasterizer.embolden(ttf::emboldenStrength(size.ppem), metrics);
  }
  return placeable(loaded, *metrics);
}
```

  - `kerning`: its style parameter is renamed `drawn`; `styleAt(drawn)` finds the file, and the pair store uses `const uint8_t style = source_[drawn];` with the comment "A synthetic style kerns as its file does, with the file's style's pairs: every glyph is the same amount wider."

- [ ] **Step 6: Run the font tests.**

Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe`
Expected: PASS, every test (150 on the prototype's base), with the heap line `a one-file family without and with synthetic bold: 1872/2352 B; a native size: 272/752 B` or close to it.

- [ ] **Step 7: Commit.**

```bash
git add lib/EpdFont/ttf/TtfFace.h lib/EpdFont/ttf/TtfFace.cpp lib/EpdFont/ttf/TtfFont.h lib/EpdFont/ttf/TtfFont.cpp \
  test/ttf_font/TtfFontTest.cpp
git commit -m "feat(ttf): a family without a bold file draws bold and bold italic emboldened"
```

### Task 4: Section version, bench, docs, firmware

**Files:**
- Modify: `lib/Epub/Epub/Section.cpp` (`SECTION_FILE_VERSION`)
- Modify: `bench/ttf_raster_main.cpp`, `platformio.ini` (`[env:bench_ttf_raster]`)
- Modify: `docs/file-formats.md`, `USER_GUIDE.md`

- [ ] **Step 1: The section version.** One more than master's, with the history line on top:

```cpp
constexpr uint8_t SECTION_FILE_VERSION = 97;  // v97: bold in a TrueType family without a bold
                                              // file is synthetic, wider than the regular
                                              // v96: ...
```

- [ ] **Step 2: The bench measures a bold miss** (spec 7.3). In `bench/ttf_raster_main.cpp`: `renderAll` gains `const bool bold = false` and does `load()`, then `embolden(emboldenStrength(ppem26_6), &metrics)` when `bold`, then `rasterize(rasterizer.output())`, timing all three as a miss would; the scratch block is planned with `planScratch(face, heading, kPoolBytesFlash, true)`; after `render_40pt_pool4k` it reports `bold_14pt_pool4k` and `bold_40pt_pool4k`; it embeds `test/fixtures/fonts/NotoSansJP-Dense.ttf` too (`board_build.embed_files` lists both files, one per line; the symbols are `_binary_test_fixtures_fonts_NotoSansJP_Dense_ttf_start`/`_end`) and reports `cjk_12pt_pool8k` and `cjk_bold_12pt_pool8k` from a second rasterizer planned with `kPoolBytesSd` and `true`. The file's header comment names the new runs. The 14 pt regular hash must still match the host's.

- [ ] **Step 3: Docs.**
  - `docs/file-formats.md`, the glyph store's face bullet, after the key layout: "A synthetic bold style (spec 7.1) keys its glyphs under its own style bits and the face index of its size, like a style with a file; its kerning pairs are its file's style's."
  - `USER_GUIDE.md`, **EPUB Font** → **Font Family**: add "A TrueType font without a bold file draws bold text emboldened."

- [ ] **Step 4: Build the firmware and the bench.** One at a time, in the background: `pio run -e default`, `-e x4pro`, `-e lilygo_t5s3`, `-e bench_ttf_raster`.
Expected: `SUCCESS` each; `check_no_float: ... TTF engine objects, no floating point` on the firmware builds.

- [ ] **Step 5: Run the whole host suite, format, commit.**

Run: `cmake --build build/test -j 8 -- -k 0 && (cd build/test && ctest -j 8 --output-on-failure)`
Expected: all pass, except tests that fail on master too on Windows (compare with master's run).

```bash
PATH="<clang-format 21 dir>:$PATH" bash bin/clang-format-fix
git add lib/Epub/Epub/Section.cpp bench/ttf_raster_main.cpp platformio.ini docs/file-formats.md USER_GUIDE.md
git commit -m "feat(ttf): section version 97 for synthetic bold; the bench times a bold miss"
```

### Task 5: On the devices

**Files:** none changed; figures and screenshots go into the PR description.

- [ ] **Step 1: The bench on an ESP32-C3** (the X4, or the X3 when the maintainer flashes it): `pio run -e bench_ttf_raster -t upload --upload-port <port>`, then read the serial output to `=== done ===`.
Expected: `output hash ... MATCHES`; record `avg=` of `render_14pt_pool4k` against `bold_14pt_pool4k`, `render_40pt_pool4k` against `bold_40pt_pool4k`, and `cjk_12pt_pool8k` against `cjk_bold_12pt_pool8k`. The prototype on the X4 (2026-10-20): 480 against 819 µs, 1,229 against 1,610 µs, 670 against 1,321 µs (a dense CJK glyph costs about twice a regular one, once, before the store holds it). A result far above these is worth a look before the PR; it is not a pass mark.

- [ ] **Step 2: Flash the reader** (`pio run -e default -t upload --upload-port <port>` on the X4; the maintainer flashes the X3).

- [ ] **Step 3: CJK bold.** With a CJK TrueType family installed (the CJK fonts plan's, or a `.ttf` copied to `/.crosspoint/fonts/<Family>/`) and selected, open `/test/test_cjk_ja_soseki.epub` and `/test/test_cjk_zh_honglou.epub` at 12 pt (`cmd:GET:fontFamily`, `cmd:GET:fontSize` first; `cmd:SET:...` the family's index and 12; `cmd:ACT:READER:<path>@1:1`, `idle:120000:SCREEN`, `shot:/c/tmp/bold-<board>-<book>.png`). Their chapter headings are bold.
Expected: the headings are visibly heavier than the text, their counters (書, 慶 and the like) open; Claude reviews the screenshots for stroke weight and counters (the maintainer does not read CJK).

- [ ] **Step 4: Latin.** A regular-only Latin TrueType family (copy any regular-only `.ttf`, such as `lib/EpdFont/builtinFonts/source/NotoSans/NotoSans-Regular.ttf` alone, into `/.crosspoint/fonts/NotoSansRegularOnly/`) on a book with bold spans (`/test/test_font_families.epub` or any with `<b>`), and a book that embeds a regular-only font with Publisher Fonts on.
Expected: bold spans are heavier and wider than the text; a family with a real Bold (Bookerly) draws exactly as before.

- [ ] **Step 5: Restore** every setting changed, and record the bench figures and screenshots for the PR.
