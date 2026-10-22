# Synthetic italic for outline families — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Italic and bold italic text in a TrueType or CFF family without an italic file of its own draws slanted, as FreeType's `FT_GlyphSlot_Oblique` and KOReader draw it. This covers regular-only Latin families on the card, the CJK families, fonts books embed, and the two italic styles of a Regular-and-Bold family.

**Architecture:** `ttf::Rasterizer` gains `slant()`, FreeType 2.13.3's `FT_GlyphSlot_Oblique` rebuilt in integers: each point's x gains `mulFix(y, kSlant)`, measured from the glyph's origin. It runs on the outline that `load()` (and `embolden()`) leaves, then boxes the glyph again; the advance stays. `planScratch` leaves room for the shear. `Face::italic` reads head.macStyle bit 1 and OS/2 fsSelection bits 0 and 9. `TtfFont::chooseSynthetic()` applies spec 7.4's rule after 7.1's, so a drawn style is emboldened, slanted or both, always from a file's style block (`source_`), and keyed under its own style bits. `TtfFont::load()` drops the slant, then the emboldening, for a glyph they make too large. `ttf::kRenderVersion` goes to 3, because a regular-only family's bold-italic keys now draw slanted. Advances do not change, so neither the section version nor the Markdown cache version moves.

**Tech Stack:** C++20 firmware (PlatformIO, ESP32-C3), GoogleTest host tests (`test/ttf_rasterizer`, `test/ttf_font`) against FreeType 2.13.3 (the SDK's copy, with `src/base/ftsynth.c` added to the oracle), the device bench `bench/ttf_raster_main.cpp`, `tools/devctl.py`.

**Spec:** `docs/superpowers/specs/2026-10-20-cjk-ttf-fonts-design.md`, section 7.4, with 7.1–7.3 (synthetic bold), which it mirrors. Synthetic bold shipped in #548 (plan `docs/superpowers/plans/2026-10-20-synthetic-bold.md`). Where this plan points at bold's code, it means master as merged. That code departs from bold's plan in three rulings: a zero advance stays zero (cc62bafff), a bold glyph too large to embolden is drawn unemboldened (62afe787a), and the Markdown cache version moved as well (bdcfc6368). This plan was checked against master 0eaac6878 on 2026-10-22.

## Global Constraints

- **Base.** `feat/synthetic-italic` (worktree `C:/tmp/wt-italic`) at 2acf93509 or later: master 0eaac6878 (#548) plus the spec's 7.4 commits.
  - Run once, before Task 1: `git -C /c/tmp/wt-italic submodule update --init freeink-sdk`. The worktree has no SDK checked out, and both the firmware and the FreeType oracle read it.
- **Integer only.** The engine links no floating point; `scripts/check_no_float.py` runs after every firmware link.
- **Output that must not change** (spec 7.4: "A style the family has a file for is never synthesized"). A style with a file of its own, and a synthetic bold style that is not an italic, draw exactly as on master. These pass unchanged:
  - `TtfVsFreeType.EveryGlyphMatchesFreeType` and `.EveryGlyphEmboldenedMatchesFreeType`, and their `TtfVsFreeTypeCff` siblings;
  - `TtfVsFreeTypeCffSynthetic.*`, `TtfGolden.TheDeviceBenchExpectsThisOutput` and `TtfCffGolden.TrueTypeOutputIsUnchanged`;
  - every synthetic bold test in `TtfFontTest.cpp` except the ones Task 2 changes.
- **The rule** (spec 7.4, verbatim): "An outline family gets synthetic italic only when none of its files is italic. A file counts as italic when: its `head.macStyle` says italic (bit 1); or its OS/2 `fsSelection` says italic or oblique (bits 0 and 9); or it sits in the italic or bold-italic slot."
  - Then: "italic is drawn from the regular file, slanted; bold italic is drawn from the bold file, slanted, when the family has one; otherwise bold italic is drawn from the regular, emboldened (7.1) and then slanted."
  - "A style the family has a file for is never synthesized."
  - Not covered: `.cpfont` families, built-in fonts and the UI faces.
  - "There is no setting": no setting and no new strings.
- **The table** (spec 7.4). Task 2 has one test per row:

  | Family | Italic | Bold italic |
  |---|---|---|
  | Regular only | the regular, slanted | the regular, emboldened, then slanted |
  | Regular and Bold | the regular, slanted | the Bold, slanted |
  | Regular and Italic | the Italic | the Italic, emboldened |
  | Regular, Bold and Italic | the Italic | the upright Bold |
  | An Italic moved into the regular slot | not slanted again | as in 7.1 |

- **The shear** (spec 7.4): "each point's x gains `FT_MulFix(y, 0x366A)`; `FT_MulFix(x, 0x10000)` is x exactly; y stays."
  - `ttf::mulFix` (`TtfFixed.h`) is `FT_MulFix` bit for bit. The inline `FT_MulFix_x86_64` that the oracle's `ftoutln.c` compiles to rounds the same way.
  - "Emboldening comes first, then the shear: crengine's order. The oracle uses the same order."
  - CJK glyphs are slanted too.
- **Metrics** (spec 7.4):
  - the advance stays;
  - kerning uses the source file's pairs;
  - the ink box is the slanted outline's: "The left side bearing can turn negative."
- **Glyphs too large to slant** (spec 7.4). A glyph is drawn upright when its slanted bitmap exceeds the output cap, or its offsets no longer fit `EpdGlyphRef`'s 8-bit `left` and `top`. For bold italic, the slant is dropped first, then the emboldening.
- **Storage.**
  - Glyphs are keyed by `glyphstore::makeKey(codepoint, style, faceIndex)` under the drawn style's bits.
  - `glyphstore::kMaxStyles` (8) and the store's `kFormatVersion` (4) stay as they are.
  - `ttf::kRenderVersion` goes from 2 to 3.
  - `SECTION_FILE_VERSION` stays 97, and the Markdown `CACHE_VERSION` stays 11.
- **Comments and formatting.** Comments say what the code does and why; no history narration. Before each commit, format the task's `.cpp` and `.h` files with clang-format 21:
  `CF=/c/Users/jens/AppData/Local/Temp/claude/c---development-witchhunt-reader/fc578506-1ee1-4ff8-934c-5cbb87f79026/scratchpad/cf21/clang_format/data/bin/clang-format.exe; "$CF" -i <files>`.
- **Host tests** (Git Bash). Configure once:
  ```bash
  export PATH=/c/msys64/ucrt64/bin:$PATH
  cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/cc.exe -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/c++.exe \
    -DFETCHCONTENT_SOURCE_DIR_SDFAT=C:/tmp/SdFat-2.3.1 \
    -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=C:/_development/witchhunt-reader/build/test/_deps/googletest-src \
    -DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON=C:/tmp/wt-serial-api/build/test/_deps/arduinojson-src
  ```
  Then build with `cmake --build build/test --target <T> -j 8`, and run with `./build/test/<dir>/<T>.exe --gtest_filter=...`.
- **The oracle on LP64**, as CI's Linux runs it. From PowerShell: `C:\msys64\usr\bin\bash.exe -lc "bash /tmp/ftprobe/build_tests.sh /c/tmp/wt-italic '<filter>'"`. Its FreeType objects (`/tmp/ftprobe/obj`) lack `ftsynth.c`; Task 1 adds it once.
- **Firmware.** `PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 pio run -e default`, one `pio run` at a time, in the background.
  - `x4pro` and `lilygo_t5s3` are left to CI.
  - `bench_ttf_raster` is built in Task 3.
- **Device: the X4 only** (the X3 is out of scope).
  - It is driven over the serial test API (`tools/devctl.py`, `docs/serial-test-api.md`).
  - `cmd:GET` each setting before changing it, and restore it afterwards.
  - The X4 runs German.

## Review Focus

1. **Book fonts (Publisher Fonts).** `pickFaces` (`BookFonts.cpp:359`) fills the style slots from the CSS, and moves a book's lone face to the regular slot.
   - A regular-only embedded family now draws `<em>` slanted.
   - A lone CSS-italic face whose file says italic must not be slanted again (table row 5). Pinned by Task 2's `AnItalicFileServingAsTheRegularIsNotSlantedAgain` and `AFaceWhoseFsSelectionSaysItalicOrObliqueIsNotSlanted`.
   - Task 4 reads such a book.
   - A file that is drawn italic but says upright is slanted again. That is the spec's rule; Task 3 documents it as a known limit.
2. **Italic headings and drop caps at the largest native size.** At 120 px, most slanted glyphs exceed the 4 KB output region.
   - Such a glyph is drawn upright, never missing. Bold italic falls back to emboldened upright, then plain.
   - Pinned by Task 2's `SyntheticItalicAtTheLargestNativeSizeIsSlantedWhereItFitsAndUprightWhereItDoesNot` (TrueType and CFF), and Task 1's `TtfSlant.ABitmapThatNoLongerFitsTheOutputIsUnsupported`.
   - The shear margin in `planScratch` (`TtfScratch.ASlantedPlanHasRoomForTheShear`) is insurance below the cap. The reviewer judges it by reading `planScratch`.
3. **A descender far left of the pen.** The slant moves a glyph's foot to the left. Past `EpdGlyphRef`'s 8-bit `left`, the glyph would be missing.
   - The check is `placeable()`, which runs after the rasterizer. #548's bold fallback did not retry on it; `TtfFont::load()` now must.
   - Pinned by Task 2's `AnItalicGlyphReachingTooFarLeftOnceSlantedIsDrawnUpright`.
4. **Layout stays where it was.** In these families, italic was laid out with the regular's advances and pairs, and bold italic with those of the Bold or the emboldened regular. The section version does not move, so a changed advance or pair would leave cached chapters wrong.
   - Pinned by Task 2's `SyntheticItalicKernsAsItsFileAndKeepsItsAdvance`, and by the `EpdFontFamily` advances in `ARegularAndBoldFamilyDrawsItalicAndBoldItalicSlanted`.
   - Two cached values do depend on the ink box:
     - a drop cap's width (`ChapterHtmlSlimParser.cpp:1674`, `getTextWidthScaled` → `getTextBounds`);
     - a narrowed CJK mark's ink shift (`ParsedText.cpp:1530-1546`, `getGlyphInk`).
   - Chapters laid out in an outline body font are laid out again anyway, because the render version moves its font ID (spec 7.4). The reviewer checks what that leaves out: an accent family's italic drop cap or CJK mark.
5. **A store written by #548.** Its regular-only families hold upright emboldened glyphs under the bold-italic bits.
   - Pinned by Task 3's `TtfFamily.TheIdentityChangesWithTheRenderVersion`, together with `AnotherFamilyIsInstalledOverTheStore`, which installs again over a store of another identity.
   - The reviewer checks against `TtfFont::storeKey` that no other key changes what it draws:
     - no style-2 keys ever existed in a family without an italic file (`EpdFontFamily::getFont` fell back to the regular's font);
     - no style-3 keys existed in a Regular-and-Bold family (it fell back to the Bold's).

---

### Task 1: The slant, bit for bit with FreeType, and room for it

**Files:**
- Modify: `lib/EpdFont/ttf/TtfRasterizer.h`, `lib/EpdFont/ttf/TtfRasterizer.cpp`
- Modify: `test/ttf_rasterizer/FreeTypeReference.h`, `test/ttf_rasterizer/FreeTypeReference.cpp`, `test/ttf_rasterizer/CMakeLists.txt`
- Modify: `test/ttf_rasterizer/TtfVsFreeTypeTest.cpp`, `test/ttf_rasterizer/TtfCorruptFontTest.cpp`, `test/ttf_rasterizer/TtfRasterizerTest.cpp`
- Create: `test/ttf_rasterizer/TtfSlantTest.cpp`, listed in `CMakeLists.txt` after `TtfEmboldenTest.cpp`

**Interfaces:**
- Produces, in `TtfRasterizer.h` after `emboldenStrength`: `constexpr int32_t kSlant = 0x366A;`, commented "Synthetic italic's shear (spec 7.4), 16.16: FT_GlyphSlot_Oblique's, about 12 degrees."
- Produces: `ttf::Status ttf::Rasterizer::slant(EpdGlyph* metrics)`, public, after `embolden()`.
- Produces: `ttf::ScratchPlan ttf::planScratch(const Face& face, uint32_t maxPpem26_6, uint32_t poolBytes, bool emboldened = false, bool slanted = false)`.
- Produces (tests): `ReferenceGlyph ttf_test::FreeTypeReference::glyph(uint32_t index, bool oversampleOverlaps = false, long xStrength = 0, long yStrength = 0, bool slant = false) const`, and `compareWithFreeType(name, data, bytes, face, points, Comparison* out, const Strength& strength = {}, bool slant = false)`.

- [ ] **Step 1: Let the oracle slant.**
  - **CMake.** In `test/ttf_rasterizer/CMakeLists.txt`, `${FREEINK_FONT}/third_party/freetype/src/base/ftsynth.c` joins `ftcid.c`, both in `freetype_host` and in its `set_source_files_properties(... COMPILE_DEFINITIONS FT2_BUILD_LIBRARY)`. The comment says that `ftsynth.c` (`FT_GlyphSlot_Oblique`) is not among the SDK's wrappers either.
  - **`FreeTypeReference.cpp`.** Add `#include FT_SYNTHESIS_H`. After the emboldening, add `if (slant) FT_GlyphSlot_Oblique(slot);`. The header's comment on `glyph` gains: "`slant` then shears it as FT_GlyphSlot_Oblique does (synthetic italic)."
  - **The LP64 probe's FreeType.** Run once, from PowerShell:
    ```
    C:\msys64\usr\bin\bash.exe -lc "cd /tmp/ftprobe && gcc -c -O1 -w -DFREEINK_FONT_ENABLE_CFF=1 -DFT2_BUILD_LIBRARY -I/c/tmp/wt-italic/freeink-sdk/libs/font/FreeInkFont/third_party/freetype/include /c/tmp/wt-italic/freeink-sdk/libs/font/FreeInkFont/third_party/freetype/src/base/ftsynth.c -o obj/ftsynth.o"
    ```

- [ ] **Step 2: Write the failing tests.**
  - **`compareWithFreeType`** takes `const bool slant = false` after `strength`. With `slant`:
    - it asks for `reference.glyph(g, false, strength.x, strength.y, slant)`;
    - the engine runs `slant(&metrics)` after `embolden()`, before `rasterize()`;
    - the plan's `outputBytes` is `kMaxOutputBytes` when the glyph is emboldened or slanted;
    - the expected advance is left as it is, since the slant does not change it;
    - the label gains `" slanted"`.
  - **`TEST_P(TtfVsFreeType, EveryGlyphSlantedMatchesFreeType)`** goes after `EveryGlyphEmboldenedMatchesFreeType`, and **`TEST_P(TtfVsFreeTypeCff, EveryGlyphSlantedMatchesFreeType)`** after its sibling. Each loops:
    ```cpp
    for (const uint32_t points : {8u, kPoints, 40u}) {
      // Slanted; and emboldened at the engine's strength, then slanted (spec 7.4).
      for (const Strength& strength : {Strength{}, emboldenStrength(ppemFromPoints(points))}) {
        SCOPED_TRACE(testing::Message() << points << " pt, " << strength.x << "," << strength.y << "/64, slanted");
        Comparison c;
        compareWithFreeType(GetParam(), data, bytes, face, points, &c, strength, true);
        if (HasFatalFailure()) return;
        // ...the emboldened sibling's expectations, unchanged
      }
    }
    ```
  - **`TtfSlantTest.cpp`.**
    - Its fixture, `TtfSlant`, is set up as `TtfEmbolden` is: a `TtfBuilder` font at 1000 units per em, drawn at `ppemFromPoints(40)`, with `planScratch(face, ppem, kPoolBytesFlash, true, true)`. Its glyphs:
      - 0: `rect(0, 0, 64, 64)`;
      - 1: `rect(100, 0, 500, 700)`, from the baseline up;
      - 2: `rect(100, -200, 500, 500)`, reaching below the baseline;
      - 3: no contours.
    - `Drawn draw(glyph, bold, slant)` runs `load()`, then `embolden(emboldenStrength(ppem))` when `bold`, then `slant()` when `slant`, then `rasterize()`, and expects `Ok`.
    - Its tests (the figures were worked out with the engine's `mulFix` and FreeType's box rounding):
    ```cpp
    TEST_F(TtfSlant, AGlyphLeansRightAboveTheBaselineAndLeftBelowIt) {
      // Glyph 1 upright: 34 x 59 at left 8, top 59. Its top edge, 3733/64 px up, moves 793/64 px right.
      EXPECT_EQ(47, slanted1.width);  EXPECT_EQ(59, slanted1.height);
      EXPECT_EQ(8, slanted1.left);    EXPECT_EQ(59, slanted1.top);
      // Glyph 2 upright: 34 x 59 at left 8, top 42. Its bottom edge, 1067/64 px down, moves 227/64 px left.
      EXPECT_EQ(47, slanted2.width);  EXPECT_EQ(59, slanted2.height);
      EXPECT_EQ(4, slanted2.left);    EXPECT_EQ(42, slanted2.top);
      // Both: the advance as upright.
    }
    TEST_F(TtfSlant, AnEmboldenedGlyphKeepsItsAdvanceAndLeansAsWell) {
      // Glyph 1 emboldened, then emboldened and slanted: the same advance, left, top and height
      // (nothing lies below the baseline); a wider box.
    }
    TEST_F(TtfSlant, AGlyphWithoutAnOutlineIsUnchanged) {      // glyph 3: Ok, 0 x 0, the same advance
    }
    TEST_F(TtfSlant, AfterAFailedLoadItReturnsThatStatus) {
      // load() of glyph face.numGlyphs is Malformed, and so is slant() after it.
    }
    TEST_F(TtfSlant, ABitmapThatNoLongerFitsTheOutputIsUnsupported) {
      // A rasterizer planned planScratch(face, ppem, kPoolBytesFlash) with outputBytes = glyphDataBytes(34, 59, true):
      // glyph 1 loads, and slant() is Unsupported (47 x 59 does not fit).
    }
    ```
  - **`TEST(TtfScratch, ASlantedPlanHasRoomForTheShear)`**, in `TtfRasterizerTest.cpp` after `TheEmboldenedGrowthIsClampedWithTheBox`. It uses the face of `AnEmboldenedPlanHasRoomForTheGrowthAndKeepsTheCffCaps`: CFF outlines, 1000 units per em, a box of 1000 by 1000.
    ```cpp
    EXPECT_EQ(99u, planScratch(face, 1000, kPoolBytesFlash, false, true).outputBytes);  // 22 x 18: 18 rows x kSlant,
                                                                                         // rounded up, is 4 px across
    const ScratchPlan both = planScratch(face, 1000, kPoolBytesFlash, true, true);
    EXPECT_EQ(108u, both.outputBytes);  // 24 x 18: and emboldening's 2
    EXPECT_EQ(kMaxScratchPoints, both.points);
    EXPECT_EQ(kMaxScratchContours, both.contours);
    EXPECT_EQ(kCffDecoderBytes, both.decoderBytes);
    EXPECT_EQ(kMaxOutputBytes, planScratch(face, kMaxPpem26_6, kPoolBytesFlash, true, true).outputBytes);
    ```
  - **`TtfCorruptFontTest.cpp`.** `exercise()` draws each glyph a third time: emboldened, then slanted. It runs `load()`, `embolden({s.x, s.x})`, `slant()`, then `rasterize()`, and asserts `Status::Ok || isPermanent(status)`.

- [ ] **Step 3: Run the tests to see them fail.**
Run: `cmake --build build/test --target TtfRasterizerTest -j 8`
Expected: the build fails. `kSlant` and `Rasterizer::slant` are not declared, and `planScratch` takes no fifth argument.

- [ ] **Step 4: Declare the interface.**
  - **`slant()`'s comment:** "Synthetic italic (spec 7.4): shears the outline of the last load() (and embolden()) exactly as FreeType's FT_GlyphSlot_Oblique does, each point's x gaining FT_MulFix(y, kSlant) measured from the glyph's origin, and boxes it again; the advance stays. After a failed load() or embolden() it returns that status; a bitmap that no longer fits reports what load() would."
  - **The private part** gains `void unbox();`, commented "Moves the outline back from its bitmap box's corner to the glyph's origin, where load() had it and where FreeType's synthesis reads it."
  - **`planScratch`'s comment** gains: "`slanted`: with room for synthetic italic's shear too, the planned height times kSlant, across."

- [ ] **Step 5: Implement the shear.**
  - **`unbox()`** is `embolden()`'s loop that adds `left_ * 64` and `bottom_ * 64`, moved out. `embolden()` calls it.
  - **`slant()`:**
    - after a failed load, it returns `loaded_`;
    - with no points, it returns `Ok`;
    - otherwise it calls `unbox()`, adds `mulFix(static_cast<int32_t>(points[i].y), kSlant)` to each point's x, and returns `loaded_ = placeBox(metrics)`.

    Like `embolden()`, it touches only the points.
  - **`planScratch`**, when `slanted`, sets `width = std::min<int32_t>(255, width + ((height * kSlant + 0xFFFF) >> 16))`, after the em and 255 clamps of both sides. Its comment: "Room below the output cap for synthetic italic's shear: the planned height times kSlant, rounded up, across. After the clamps: the shear grows with whatever height a glyph has. At the cap itself there is none; such a glyph is drawn upright (TtfFont::load)."

- [ ] **Step 6: Run the rasterizer tests, here and on LP64.**
Run: `cmake --build build/test --target TtfRasterizerTest -j 8 && ./build/test/ttf_rasterizer/TtfRasterizerTest.exe`
Expected:
- PASS, every test.
- Each slanted line prints `0 load failures, 0 advance diffs, 0 box diffs, 0 inexact`. That holds for the 12 TrueType fonts and the 10 CFF fonts, at 8, 14 and 40 pt, in both runs.
- A differing pixel is a port bug until shown otherwise. `kCffPixelTolerance` stays empty.

Then: `C:\msys64\usr\bin\bash.exe -lc "bash /tmp/ftprobe/build_tests.sh /c/tmp/wt-italic '*EveryGlyph*'"`
Expected: `[  PASSED  ]`, with no failure.

- [ ] **Step 7: Format and commit.**
```bash
"$CF" -i lib/EpdFont/ttf/TtfRasterizer.h lib/EpdFont/ttf/TtfRasterizer.cpp test/ttf_rasterizer/FreeTypeReference.h \
  test/ttf_rasterizer/FreeTypeReference.cpp test/ttf_rasterizer/TtfVsFreeTypeTest.cpp \
  test/ttf_rasterizer/TtfCorruptFontTest.cpp test/ttf_rasterizer/TtfRasterizerTest.cpp test/ttf_rasterizer/TtfSlantTest.cpp
git add lib/EpdFont/ttf/TtfRasterizer.h lib/EpdFont/ttf/TtfRasterizer.cpp test/ttf_rasterizer/
git commit -m "feat(ttf): synthetic italic, FreeType's oblique shear in integers"
```

### Task 2: Which styles are slanted, and how TtfFont draws them

**Files:**
- Modify: `lib/EpdFont/ttf/TtfFace.h`, `lib/EpdFont/ttf/TtfFace.cpp` (`Face::italic`), `test/ttf_rasterizer/TtfBuilder.h` (`Font::os2Bytes`); Test: `test/ttf_rasterizer/TtfFaceTest.cpp`
- Modify: `lib/EpdFont/ttf/TtfFont.h`, `lib/EpdFont/ttf/TtfFont.cpp`; Test: `test/ttf_font/TtfFontTest.cpp`

**Interfaces:**
- Consumes: Task 1's `Rasterizer::slant(EpdGlyph*)` and `planScratch(face, ppem, pool, emboldened, slanted)`.
- Produces: `bool ttf::Face::italic`: head.macStyle bit 1, or OS/2 fsSelection bit 0 or 9, read from either outline kind.
- Produces: `bool TtfFont::emboldened(uint8_t style) const` and `bool TtfFont::slanted(uint8_t style) const`. `bool TtfFont::synthetic(uint8_t style) const` now means either of the two.
- Produces (test builder): `uint16_t Font::os2Bytes = 78;`, the OS/2 table's length.

- [ ] **Step 1: Write the failing face test**, in `TtfFaceTest.cpp` after `BoldIsMacStyleOrAWeightOf600InEitherOutlineFormat`. It is built as that test is: a `threeGlyphs()` font, and the same font with CFF outlines.
  ```cpp
  TEST(TtfFace, ItalicIsMacStyleOrFsSelectionInEitherOutlineFormat) {
    // Spec 7.4. OS/2 is read as FreeType reads it: a table too short for its version counts as missing.
    // italic(macStyle, os2, fsSelection, os2Bytes = 78) builds the font and returns face.italic.
    EXPECT_FALSE(italic(0, false, 0)) << cff;
    EXPECT_TRUE(italic(2, false, 0)) << cff;
    EXPECT_FALSE(italic(1, false, 0)) << cff;            // bold
    EXPECT_TRUE(italic(0, true, 1)) << cff;              // ITALIC
    EXPECT_TRUE(italic(0, true, 1u << 9)) << cff;        // OBLIQUE
    EXPECT_FALSE(italic(0, true, 0x20 | 0x40 | 0x80)) << cff;  // BOLD, REGULAR, USE_TYPO_METRICS
    EXPECT_FALSE(italic(0, true, 1, 64)) << cff;         // a 64-byte OS/2: missing
    EXPECT_TRUE(italic(2, true, 0, 64)) << cff;          // macStyle still counts
  }
  ```

- [ ] **Step 2: Run it to see it fail.**
Run: `cmake --build build/test --target TtfRasterizerTest -j 8`
Expected: the build fails. `Face` has no member `italic`, and `Font` has no member `os2Bytes`.

- [ ] **Step 3: Add `Face::italic`, and the builder's `os2Bytes`.**
  - **`TtfFace.h`**, after `bold`: `/// An italic face: head.macStyle says italic, or OS/2's fsSelection says italic or oblique. Never slanted (spec 7.4).` `bool italic = false;`.
  - **`parseFace`**, beside the macStyle bold read: `face->italic = (bytes.u16(head.offset + 44) & 2) != 0;  // macStyle`.
  - **`parseFace`**, after the usWeightClass line: `if (haveOs2 && (bytes.u16(os2.offset + 62) & 0x0201) != 0) face->italic = true;  // fsSelection: ITALIC, OBLIQUE`. It sits under the same `haveOs2` as every OS/2 read, which is FreeType's rule.
  - **`TtfBuilder.h`.** `Font` gains `uint16_t os2Bytes = 78;  ///< the OS/2 table's length; shorter than its version's, FreeType ignores it`. `buildFont` resizes the OS/2 bytes to it after setting the fields.

- [ ] **Step 4: Run it.** `./build/test/ttf_rasterizer/TtfRasterizerTest.exe --gtest_filter='TtfFace.*'`. Expected: PASS.

- [ ] **Step 5: Write the failing font tests**, in `TtfFontTest.cpp`.
  - **Helpers**, beside `emboldened()`:
    - `Reference slantedAt(const std::vector<uint8_t>& file, uint32_t cp, uint32_t ppem, bool bold = false)` works as `emboldenedAt`. It plans `planScratch(face, kMaxNativePx << 6, kPoolBytesFlash, true, true)`, which is the plan a regular-only family merges. It runs `load()`, then `embolden()` when `bold`, then `slant()`, then `rasterize()`, each expected `Ok`.
    - `Reference slanted(const std::vector<uint8_t>& file, uint32_t cp, uint8_t points, bool bold = false)`.
    - `Reference drawnAt(const std::vector<uint8_t>& file, uint32_t cp, uint32_t ppem, bool bold, bool* leaning)`, with the same plan. It returns the first of three that the rasterizer draws `Ok` with `left` and `top` inside int8:
      - slanted (emboldened first when `bold`);
      - the same without the slant;
      - plain.

      `*leaning` says whether the slant stayed.
  - **Tests that change:**
    - **`ARegularOnlyFamilyDrawsBoldAndBoldItalicEmboldened`** becomes `ARegularOnlyFamilyDrawsBoldEmboldenedItalicSlantedAndBoldItalicBoth` (table row 1):
      - `emboldened()` is false, true, false, true for styles 0–3, and `slanted()` false, false, true, true;
      - `font(kItalic)` is non-null and not `font(kRegular)`;
      - 'W' is drawn as `emboldened(regular, 'W', 14)` in `kBold`, as `slanted(regular, 'W', 14)` in `kItalic`, as `slanted(regular, 'W', 14, true)` in `kBoldItalic`, and as `reference()` in `kRegular`.
    - **`BoldItalicIsEmboldenedFromTheItalicWhenThereIsOne`** (row 3) gains `for (uint8_t s = 0; s < 4; ++s) EXPECT_FALSE(font->slanted(s)) << int{s};`.
    - **`AFamilyWithABoldFileIsNeverEmboldened`** (row 4) keeps its code. Its comment says that `synthetic()` covers the slant now, so its loop pins row 4 too.
    - **`ABoldFileServingAsTheRegularIsNotEmboldenedAgain`** gains `EXPECT_TRUE(font->slanted(kItalic))` and `EXPECT_EQ(nullptr, font->font(kBoldItalic))`. The comment on the second: "bold only by its face: bold italic falls back to the slanted face (EpdFontFamily::getFont)".
    - **`ABrokenGlyphIsMarkedMissingAndNotTriedAgain`**: its last line becomes `EXPECT_TRUE(font->slanted(kItalic));  // a one-style family: its bold and italic are synthetic (spec 7.1, 7.4)`.
    - **`AFamilyHoldsMemoryOnlyForTheStylesItHas`**: `built.macStyle = 3;  // declared bold italic, so no style is synthetic: ...`.
    - **`ASyntheticStyleCostsItsFontsAndNoFile`:**
      - the two families are `macStyle` 3, then 0;
      - `fonts[kBold]` and `fonts[kItalic]` are non-null only for the second;
      - both bounds become `< 768u`, for three fonts;
      - the print says "without and with synthetic styles".
    - **`TtfFontSetTest.TheFamiliesAreInstalledTogetherAndTheNextLoadReusesThem`**: the Inter line becomes `EXPECT_TRUE(set->font(1)->slanted(kItalic));  // no Inter italic file: the regular, slanted (spec 7.4)`.
  - **New tests**, in a new section at the end of the file, `// --- Synthetic italic (spec 7.4) ---`. Each family is opened under its own identity.

    | Test | Family | Assertions |
    |---|---|---|
    | `ARegularAndBoldFamilyDrawsItalicAndBoldItalicSlanted` | Bookerly Regular and Bold (row 2) | No style emboldened; `slanted()` false, false, true, true. 'W' in `kItalic` = `slanted(regular)`, in `kBoldItalic` = `slanted(bold)`. Through `EpdFontFamily(font(0..3))`, ITALIC 'W' has REGULAR's `advanceX`, and BOLD_ITALIC 'W' has `reference(files[kBold], 'W', 14)`'s: the advances the fallback gave |
    | `AnItalicFileServingAsTheRegularIsNotSlantedAgain` | `Bookerly/Bookerly-Italic.ttf`, then `NotoSans/NotoSans-Italic.ttf`, each alone in the regular slot (row 5) | `!slanted(kItalic)` and `font(kItalic) == nullptr`. `emboldened(kBold)` and `emboldened(kBoldItalic)`, and 'W' in `kBoldItalic` = `emboldened(file, 'W', 14)` |
    | `AFaceWhoseFsSelectionSaysItalicOrObliqueIsNotSlanted` | `kernedBoxes()` with `os2 = true` and `os2Selection` 1, then `1u << 9`, each alone | `!slanted(kItalic)`, `font(kItalic) == nullptr`, `emboldened(kBold)` |
    | `SyntheticItalicIsStoredUnderTheItalicStyleAndServesTheNextBoot` | Bookerly Regular alone | First boot: `glyph(kRegular/kItalic/kBoldItalic, 'W')` all non-null, and `renders() == 3`. Next boot: drawn as `reference`, `slanted` and `slanted(..., true)`, with `renders() == 0` and `storeHits() == 3` |
    | `SyntheticItalicKernsAsItsFileAndKeepsItsAdvance` | `kernedBoxes()` | `slanted(kItalic)`. `kerning(kRegular, 'A', 'B') == -47` and `kerning(kItalic, 'A', 'B') == -47`, with `kernLookups() == 1`. Italic 'A' has regular 'A''s `advanceX`, `left`, `top` and `height`, and a greater `width`. Bold-italic 'A' has bold 'A''s `advanceX` |
    | `SyntheticItalicAtTheLargestNativeSizeIsSlantedWhereItFitsAndUprightWhereItDoesNot` | Bookerly Regular alone (U+0021–U+024F, more than 300 codepoints), then `NotoSansJP-CffFlat.otf` alone (`mappedCodepoints()`, more than 250), at `nativeFonts(maxNativePx())` | For every codepoint the regular draws there, `kItalic` = `drawnAt(file, cp, ppem, false, &leaning)` and `kBoldItalic` = `drawnAt(file, cp, ppem, true, &leaning)`. For Bookerly, the `kItalic` glyphs include both leaning and upright ones (each count above 0): the 4 KB cap decides |
    | `AnItalicGlyphReachingTooFarLeftOnceSlantedIsDrawnUpright` | A built font, regular only: 'A' `rect(50, 0, 450, 700)`, 'J' `rect(-2200, -2000, -2100, 0)`; opened at `TtfFont::kMaxPointSize` (26 pt, 54 px) | 'J' in `kItalic` = `reference(file, 'J', 26)`: upright it reaches 120 px left of the pen; slanted, its foot reaches 143 px, past int8. 'A' in `kItalic` = `slanted(file, 'A', 26)` |
    | `TtfFontSetTest.SyntheticItalicInSdModeIsDrawnFromTheRegularFile` | as `SyntheticBoldInSdModeIsDrawnFromTheRegularFile` | `readsCard()` and `slanted(kItalic)`. `prefetch(kItalic, <the ten kana>)` gives `renders() == 10`, and more than 0 but at most 11 new `sdPageReads()`. Each kana in `kItalic` = `slanted(file, cp, 14)`, with `renders()` still 10. 'あ' in `kRegular` = `reference(...)`. `expectWrittenOnce(chip)` |

- [ ] **Step 6: Run them to see them fail.**
Run: `cmake --build build/test --target TtfFontTest -j 8`
Expected: the build fails. `TtfFont` has no members `emboldened` and `slanted`.

- [ ] **Step 7: Choose and draw the slanted styles.**
  - **`TtfFont.h`.**
    - `synthetic_` gives way to `uint8_t emboldened_ = 0;` and `uint8_t slanted_ = 0;`, one bit per style.
    - Public:
      - `emboldened(style)`: "Whether `style` is drawn emboldened from another style's file (spec 7.1)."
      - `slanted(style)`: "Whether `style` is drawn slanted from another style's file (spec 7.4)."
      - `synthetic(style)`: `emboldened(style) || slanted(style)`.
    - `font()`'s comment gains: "A family none of whose files is italic draws italic, and bold italic without a file of its own, slanted (spec 7.4)."
    - `source_`'s comment: "...or for a synthetic style the regular, the italic or the bold".
  - **`chooseSynthetic()`.** Both rules read the files before either changes `source_`:
    ```cpp
    bool bold = fileSlot_[1] != kNoSlot || fileSlot_[3] != kNoSlot;
    bool italic = fileSlot_[2] != kNoSlot || fileSlot_[3] != kNoSlot;
    for (uint8_t s = 0; s < 4; ++s) {
      if (!draws(s)) continue;
      bold = bold || styleAt(s)->face.bold;
      italic = italic || styleAt(s)->face.italic;
    }
    if (!bold) { /* as today: source_[1] = 0, source_[3] = draws(2) ? 2 : 0, emboldened_ bits 1 and 3 */ }
    if (!italic) {
      // Italic from the regular. Bold italic: the emboldened regular above, else the Bold file. A family that is bold
      // only by its faces (a bold or heavy file serving as the regular) has neither: its bold italic falls back to
      // the italic.
      source_[2] = 0;
      if (source_[3] == kNoSlot && fileSlot_[1] != kNoSlot && draws(1)) source_[3] = 1;
      slanted_ = static_cast<uint8_t>((1u << 2) | (source_[3] != kNoSlot ? 1u << 3 : 0u));
    }
    // then slot_ and fontCount_, as today
    ```
  - **`openStyles`.** The loop that merges synthetic plans runs `if (!synthetic(s) || !draws(s)) continue;` and merges `planScratch(source.face, largest, poolFor(source), emboldened(s), slanted(s))`. Its comment: "A synthetic glyph outgrows its file's plan: wider emboldened, wider again slanted."
  - **`load()`.**
    - Its comment: "The rasterizer's load(), then embolden() and slant() as the style asks (spec 7.2, 7.4), for a glyph EpdGlyphRef can place. A glyph that the synthesis makes too large (the output region's cap, the box or advance limits, EpdGlyphRef's 8-bit offsets), though its file's glyph fits, is drawn without the slant, then without the emboldening too, rather than missing. A Transient stays so."
    - The body:
    ```cpp
    bool bold = emboldened(style);
    bool slant = slanted(style);
    for (;;) {
      ttf::Status loaded = rasterizer.load(bytes, face, index, size.ppem, metrics);
      if (loaded != ttf::Status::Ok) return loaded;
      if (bold) loaded = rasterizer.embolden(ttf::emboldenStrength(size.ppem), metrics);
      if (slant && loaded == ttf::Status::Ok) loaded = rasterizer.slant(metrics);
      loaded = placeable(loaded, *metrics);
      if (loaded != ttf::Status::Unsupported || (!bold && !slant)) return loaded;
      if (slant) {
        slant = false;  // upright first
      } else {
        bold = false;   // then the file's own weight
      }
    }
    ```
    A bold glyph whose offsets no longer fit `EpdGlyphRef` is now drawn unemboldened too. Emboldening keeps the left edge and the top, so in practice that does not happen.
  - **Unchanged:** `render`, `prefetch`, `kerning` (`source_[drawn]` already names the file's style), `configure`, `rebase`, `nativeSize`, and `TtfFontSet`.

- [ ] **Step 8: Run the font tests.**
Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe`
Expected:
- PASS, every test.
- The heap line shows about 720 B more for the family with synthetic styles, and the same for a native size: three fonts of about 240 B on the 64-bit host.

Also run: `cmake --build build/test --target TtfRasterizerTest -j 8 && ./build/test/ttf_rasterizer/TtfRasterizerTest.exe --gtest_filter='TtfFace.*'`
Expected: PASS.

- [ ] **Step 9: Format and commit.**
```bash
"$CF" -i lib/EpdFont/ttf/TtfFace.h lib/EpdFont/ttf/TtfFace.cpp lib/EpdFont/ttf/TtfFont.h lib/EpdFont/ttf/TtfFont.cpp \
  test/ttf_rasterizer/TtfBuilder.h test/ttf_rasterizer/TtfFaceTest.cpp test/ttf_font/TtfFontTest.cpp
git add lib/EpdFont/ttf/TtfFace.h lib/EpdFont/ttf/TtfFace.cpp lib/EpdFont/ttf/TtfFont.h lib/EpdFont/ttf/TtfFont.cpp \
  test/ttf_rasterizer/TtfBuilder.h test/ttf_rasterizer/TtfFaceTest.cpp test/ttf_font/TtfFontTest.cpp
git commit -m "feat(ttf): a family without an italic file draws italic and bold italic slanted"
```

### Task 3: Render version 3, the bench, the docs, the firmware

**Files:**
- Modify: `lib/EpdFont/ttf/TtfFamily.h`; Test: `test/ttf_font/TtfFamilyTest.cpp`
- Modify: `bench/ttf_raster_main.cpp`
- Modify: `docs/file-formats.md`, `USER_GUIDE.md`, `docs/superpowers/specs/2026-10-20-cjk-ttf-fonts-design.md` (7.4)

**Interfaces:**
- Consumes: Task 1's `Rasterizer::slant` and `planScratch(..., emboldened, slanted)`.
- Produces: `ttf::kRenderVersion == 3`.

- [ ] **Step 1: Write the failing identity test**, in `TtfFamilyTest.cpp` after `TheIdentityChangesWithAnyFileDetail`.
  - A test helper, `identityAt(const ttf::FileStamp (&styles)[4], uint8_t version)`, rebuilds the identity as `docs/file-formats.md` describes it:
    - four lanes, each FNV-1a seeded `2166136261u ^ (lane * 0x9E3779B9u)`;
    - for each style, a marker byte (the style, or `0x80 | style` without a file), then the name with its NUL, then the size (4 bytes), date (2) and time (2), little-endian;
    - then the version byte;
    - each lane stored little-endian.
  - The test:
  ```cpp
  TEST(TtfFamily, TheIdentityChangesWithTheRenderVersion) {
    // Spec 7.4: a store #548 wrote holds a regular-only family's bold italic upright; at version 3 it is installed again.
    const ttf::FileStamp styles[4] = {{"Lit-Regular.ttf", 450048, 0x5A21, 0x6400}};
    EXPECT_EQ(3, ttf::kRenderVersion);
    EXPECT_EQ(identityAt(styles, 3), identityOf(styles));
    EXPECT_NE(identityAt(styles, 2), identityOf(styles));
  }
  ```

- [ ] **Step 2: Run it to see it fail.**
Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_filter='TtfFamily.TheIdentity*'` (`TtfFamilyTest.cpp` is part of `TtfFontTest`)
Expected: FAIL. `kRenderVersion` is 2, and the identity is `identityAt(styles, 2)`'s.

- [ ] **Step 3: Bump the render version.** `constexpr uint8_t kRenderVersion = 3;`, with a history line under version 2's: "3: synthetic italic (spec 7.4): a family with neither a bold nor an italic file stored its bold italic emboldened and upright under the bold-italic bits, which now draw slanted."

- [ ] **Step 4: Run it.** Run the same command. Expected: PASS. Then run `./build/test/ttf_font/TtfFontTest.exe` whole. Expected: PASS.

- [ ] **Step 5: The bench times a slanted miss** (spec 7.4: "The device bench confirms it, as it did for bold").
  - **`renderGlyphs`** gains `const bool slant = false` after `bold`. It runs `slant(&metrics)` after the emboldening, timed with the rest as one miss. `renderAll` passes it on.
  - **Bookerly's rasterizer** is planned `planScratch(face, heading, kPoolBytesFlash, true, true)`. After `bold_40pt_pool4k`, the bench reports `italic_14pt_pool4k` (slanted), `italic_40pt_pool4k`, and `bolditalic_14pt_pool4k` (emboldened, then slanted).
  - **`benchJapanese`.** The 8 KB-pool rasterizer is planned `planScratch(face, ppem, kPoolBytesSd, true, true)`. After `<name>_kanji_12pt_pool8k_bold`, it reports `<name>_kanji_12pt_pool8k_italic`.
  - **The file's header comment** names the new runs.
  - Bookerly's 14 pt hash must still match the host's (`TtfGolden`).

- [ ] **Step 6: Docs.**
  - **`docs/file-formats.md`, the glyph store's face bullet.** "A synthetic bold style (spec 7.1), drawn from another style's file, keys its glyphs ..." becomes "A synthetic style (spec 7.1, 7.4), drawn emboldened or slanted from another style's file, keys its glyphs ...".
  - **`docs/file-formats.md`, the identity bullet:** "and the engine's render version (`ttf::kRenderVersion`, 3)".
  - **`USER_GUIDE.md`, Font Family:** "A TrueType or OpenType font without a bold file draws bold text emboldened, and without an italic file draws italic text slanted."
  - **`USER_GUIDE.md`, Publisher Fonts:** "A book font without a bold face draws bold text emboldened, and without an italic face draws italic text slanted."
  - **The spec, section 7.4**, after the **Scope** bullets: "**Known limit.** A book face declared italic in CSS whose file says upright (`macStyle` bit 1 and `fsSelection` bits 0 and 9 clear) is slanted again: `pickFaces` moves a lone face to the regular slot and its CSS style is not kept. OpenDyslexic's `OpenDyslexic-Italic.otf` is such a file."

- [ ] **Step 7: Build the firmware and the bench**, one at a time, in the background:
  - `PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 pio run -e default`
  - `PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 pio run -e bench_ttf_raster`

Expected: `SUCCESS` for each, and `check_no_float: <n> TTF engine objects, no floating point` on the firmware build. Note the flash size against master's.

- [ ] **Step 8: Run the whole host suite, then format and commit.**
Run: `cmake --build build/test -j 8 -- -k 0 && (cd build/test && ctest -j 8 --output-on-failure)`
Expected: every test that passes on master passes here. Compare with the same suite run on master 0eaac6878.
```bash
"$CF" -i lib/EpdFont/ttf/TtfFamily.h test/ttf_font/TtfFamilyTest.cpp bench/ttf_raster_main.cpp
git add lib/EpdFont/ttf/TtfFamily.h test/ttf_font/TtfFamilyTest.cpp bench/ttf_raster_main.cpp docs/file-formats.md \
  USER_GUIDE.md docs/superpowers/specs/2026-10-20-cjk-ttf-fonts-design.md
git commit -m "feat(ttf): render version 3 for synthetic italic; the bench times a slanted miss"
```

### Task 4: On the X4

**Files:** none changed. The figures and the screenshots go into the PR description.

- [ ] **Step 1: The bench.** Run `PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 pio run -e bench_ttf_raster -t upload --upload-port <port>`, and read the serial output up to `=== done ===`. Stop the logger before flashing again.
Expected: `output hash ... MATCHES`. Record each pair's `avg=`:
- `render_14pt_pool4k` against `italic_14pt_pool4k`;
- `render_40pt_pool4k` against `italic_40pt_pool4k`;
- `bold_14pt_pool4k` against `bolditalic_14pt_pool4k`;
- `<name>_kanji_12pt_pool8k` against `<name>_kanji_12pt_pool8k_italic`, for `truetype`, `cff_flat` and `cff_subrs`.

Spec 7.4 expects the slant not to show next to the rasterizer. The figures are recorded, not a pass mark.

- [ ] **Step 2: Flash the reader.** `PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 pio run -e default -t upload --upload-port <port>`.

- [ ] **Step 3: Fonts and books on the card.**
  - Run `python tools/devctl.py "cmd:FS:LS:/.crosspoint/fonts" "cmd:FS:LS:/test"`.
  - Expected fonts: `NotoSansRegularOnly/` (regular-only TrueType), `OpenDyslexicOtf/` (regular-only CFF) and `NotoSansTCOtf/` (CJK, SD mode). A Japanese family serves the CJK step better, if the card has one.
  - Expected books: `test_text_rendering.epub` (its spine item 6 has `<i>` list items), `test_cjk_line_breaking.epub` (spine item 1 has `<i>斜体の文字</i>` and `<i>混ざって</i>`) and `bookfont_cff.epub`.
  - Copy a missing book from `test/epubs/` with `tools/serial_transfer.py` (USB Transfer screen, `MSYS_NO_PATHCONV=1`).

- [ ] **Step 4: Latin, regular-only.**
  - First, `cmd:GET:fontFamily cmd:GET:fontSize cmd:GET:publisherFonts`.
  - Then `cmd:SET:fontFamily=NotoSansRegularOnly` and `cmd:SET:fontSize=14pt` (a size is set by its label).
  - Then `"cmd:ACT:READER:/test/test_text_rendering.epub@6:1" idle:120000:SCREEN shot:/c/tmp/italic-x4-noto.png`.
  - The same with `fontFamily=OpenDyslexicOtf`, shot to `/c/tmp/italic-x4-dyslexic.png`.

Expected: the italic words lean right by about 12°, and the upright words around them are unchanged. No glyph is missing, and no slanted glyph is clipped on its right. Claude reviews the screenshots.

- [ ] **Step 5: Publisher Fonts.**
  - Set `publisherFonts` to the value that uses the book's own fonts (its label, as `GET:*` lists it).
  - Open `/test/bookfont_cff.epub`, page with `cmd:BTN:RIGHT` to text in `<em>` or `<i>`, and shot it.
  - If the book has no italic text:
    - write a one-chapter EPUB to `/c/tmp/bookfont_italic.epub` with Python's `zipfile` (`mimetype` stored first, then `container.xml`, the OPF and the chapter). Its only `@font-face` is `lib/EpdFont/builtinFonts/source/OpenDyslexic/OpenDyslexic-Regular.otf`, for a family the body uses, and its paragraph mixes plain, `<em>`, `<b>` and `<b><em>` words;
    - upload it to `/test`, and read it instead.

Expected: `<em>` is slanted in the embedded face, and `<b><em>` is heavier and slanted.

- [ ] **Step 6: CJK.**
  - `cmd:SET:fontFamily=<the CJK family>` and `cmd:SET:fontSize=12pt`.
  - `"cmd:ACT:READER:/test/test_cjk_line_breaking.epub@1:1" idle:120000:SCREEN cmd:TTF`.
  - Page to the inline paragraph (漢字の…斜体の文字…混ざって), and `shot:/c/tmp/italic-x4-cjk.png`.

Expected:
- `TTF:... sd=1`.
- The `<i>` runs lean right, kanji and kana included, and the line ends sit where the upright text's would.
- The log has no `unsupported` lines.
- With the TC family, kana it lacks fall back as before.

- [ ] **Step 7: Restore.** `cmd:SET` every changed setting back to the value `GET` gave. Remove anything this run added to the card, such as `/test/bookfont_italic.epub` (`cmd:FS:RM:<path>`). Record the bench figures and the screenshots for the PR.
