# Synthetic bold for outline families — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bold and bold italic text in a TrueType or CFF family that has no bold file of its own draws emboldened, as FreeType's synthetic bold does: the CJK families (Regular only), regular-only Latin families on the card, and fonts books embed.

**Architecture:** `ttf::Rasterizer` gains `embolden()`: FreeType 2.13.3's `FT_Outline_EmboldenXY`, with the orientation and vector-length helpers it calls, rebuilt in integers, bit for bit. It runs between `load()` and `rasterize()` on the scaled 26.6 outline `load()` leaves, whichever loader filled it (the TrueType `Loader`, or `loadCff` and `scaleOutline`), and boxes the glyph again. The strength is FreeType's across and none down (`ttf::emboldenStrength`). `Face::bold` reads head.macStyle and the OS/2 weight class from either outline kind. `TtfFont` separates the fonts it serves (one per drawn style, in every size) from the files that draw them. It decides once per load which styles are synthetic, keys their glyphs under their own style bits, and draws them from the source file's style block (in SD mode, from that file's outlines on the card). Their kerning pairs are the source style's. The section version moves, because bold text in such a family gets wider.

**Tech Stack:** C++17 firmware (PlatformIO, ESP32-C3/S3), GoogleTest host tests (`test/ttf_rasterizer`, `test/ttf_font`) against FreeType 2.13.3 (the SDK's copy, CFF driver included), the device bench `bench/ttf_raster_main.cpp`, `tools/devctl.py`.

**Spec:** `docs/design/cjk-ttf-fonts.md`, section 7, with 9.3 (the oracle's bar for CFF) and section 10's "Bold" and "Latin" bullets. The prototype is `proto/synthetic-bold` (worktree `C:/tmp/wt-bold-proto`, 0da58ed81 on master 2d09575f3). It predates #532, #542 and #544. This plan was checked against master 5ae036f79 on 2026-10-21. Where a step points at the prototype's code, that code is right as it stands; everything else follows master.

## Global Constraints

- Base: master at 5ae036f79 (#544) or later.
- Integer only: the engine links no floating point (`scripts/check_no_float.py` runs after every firmware link).
- Regular output stays byte-identical. These pass unchanged: `TtfVsFreeType.EveryGlyphMatchesFreeType`, `TtfVsFreeTypeCff.EveryGlyphMatchesFreeType`, `TtfVsFreeTypeCffSynthetic.*`, `TtfGolden.TheDeviceBenchExpectsThisOutput` and `TtfCffGolden.TrueTypeOutputIsUnchanged`. The spec's anti-goal: "any regular glyph, any glyph of a family with a real Bold, or any TrueType glyph renders differently".
- The rule (spec 7.1): "An outline family (TrueType or CFF) gets synthetic bold only when none of its files is bold. A file counts as bold when its `macStyle` says bold or its OS/2 weight class is 600 or more, and so does any file in the bold or bold-italic slot." Bold is drawn from the file serving the regular style. Bold italic is drawn from the italic file when the family has one, otherwise from the regular. "A style the family has a file for is never synthesized." `.cpfont` families, built-in fonts and the UI faces are not covered.
- Strength (spec 7.2): "FreeType's strength across (`x_ppem * 0x0AAA / 1024`, about em/24) and none down". It is one function, `ttf::emboldenStrength(ppem26_6)`.
- Metrics (spec 7.2): "The ink box grows across; the left side bearing, the baseline and the line height stay." "The advance grows by the strength, rounded as FreeType rounds it." That is `advanceX + (strength.x + 2) / 4`, from 26.6 to 12.4.
- The oracle (spec 7.3, 9.3) covers every glyph of the 12 bundled TrueType fonts and of the 10 CFF test fonts (`cffTestFonts()`), at 8, 14 and 40 pt, emboldened at the engine's strength and at FreeType's own (both axes).
  - Advances and boxes: exact.
  - Bitmaps: "byte-identical is the target". For a CFF font where rounding makes that impossible, "at most 3% differing pixels per font and size in all", recorded in `kCffPixelTolerance`.
- Counters: "A test pins two thirds and no filled counter on Noto Sans and Serif JP samples" (a regular blank pixel counts as kept when the bold bitmap is blank at the same absolute position; an enclosed blank region of 5 or more pixels, 4-connected, fills when none of its pixels stays blank). The anti-goal is "synthetic bold fills the counters of dense characters at reading sizes".
- Storage (spec 7.2):
  - glyphs are keyed by `glyphstore::makeKey(codepoint, style, faceIndex)` under the bold style's bits, with the face index of the size;
  - "`glyphstore::kMaxStyles` is untouched" (8), and the store's format (`kFormatVersion` 4) stays;
  - "`ttf::kRenderVersion` stays" (2);
  - `SECTION_FILE_VERSION` goes from 96 to 97.
- "There is no setting to turn it off": no setting and no new strings.
- Comments say what the code does and why; no history narration. Format with clang-format 21: `bin/clang-format-fix` with 21.1.8 first on the PATH.
- Host tests (Git Bash):
  - configure with `-DFETCHCONTENT_SOURCE_DIR_SDFAT=C:/tmp/SdFat-2.3.1`, plus the GOOGLETEST and ARDUINOJSON source dirs as for every host build;
  - run with `cmake --build build/test --target <T> -j 8`, then `./build/test/<dir>/<T>.exe --gtest_filter=...`.
- The oracle on LP64, as CI's Linux runs it, from PowerShell: `C:\msys64\usr\bin\bash.exe -lc "bash /tmp/ftprobe/build_tests.sh /c/tmp/<worktree> '<filter>'"`. A synthetic CFF font in an oracle test writes negative DICT integers compactly (`putCffCompactInt`), never as operator 29 (FreeType misreads those on LP64).
- Firmware: `PLATFORMIO_CORE_DIR=C:/pio pio run -e default`, one `pio run` at a time, in the background. `x4pro` and `lilygo_t5s3` are left to CI. `bench_ttf_raster` is built for Task 5.
- Devices:
  - the X4 is driven over the serial test API (`tools/devctl.py`);
  - the X3 is flashed by the maintainer only, never RTS or esptool;
  - `CMD:GET` each setting before changing it and restore it afterwards;
  - the X4 runs German.

## Review Focus

1. **Book fonts** (Publisher Fonts, CFF faces included). `BookFonts.cpp` (`pickFaces`) fills the style slots from the CSS `@font-face` weights, not from the files.
   - A regular-only embedded family now draws its bold emboldened, and its layout widens.
   - A book that declares its regular file bold as well puts it in the bold slot. The rule counts that file as bold, so that bold stays unemboldened.

   Pinned by Task 3's `AFamilyWithABoldFileIsNeverEmboldened` and `ARegularOnlyFamilyDrawsBoldAndBoldItalicEmboldened`; Task 5 reads such a book.
2. **The PostScript orientation, and an outline without one.** FreeType gives every glyph of the 12 bundled TrueType fonts the TrueType orientation, so the prototype's zero differences never reached `emboldenXY`'s PostScript branch, nor an outline FreeType cannot orient. The CFF fonts have it: the Noto subsets and OpenDyslexic are PostScript-oriented but for a few glyphs, and `CffOperators.otf` mixes both and has one glyph with no orientation. Pinned by Task 1's `TtfVsFreeTypeCff.EveryGlyphEmboldenedMatchesFreeType` and `TtfEmbolden.AnOutlineOfEitherDirectionGrowsOutwards`. The reviewer reads the port's two sign flips against `ftoutln.c:917`.
3. **The largest native size** (120 px headings, drop caps). An emboldened glyph is wider than its file's plan allows for. `planScratch(..., emboldened = true)` adds twice the strength across and keeps the CFF caps. Pinned by Task 2's `TtfScratch.AnEmboldenedPlanHasRoomForTheGrowthAndKeepsTheCffCaps` and Task 3's `SyntheticBoldDrawsEveryGlyphAtTheLargestNativeSize` (TrueType and CFF). Neither family fails without the margin, which is insurance; the reviewer judges it by reading `planScratch`. At the 4 KB output cap itself the margin is inert: a glyph that fits as the regular but not emboldened is drawn unemboldened (`SyntheticBoldDrawsAGlyphTooLargeToEmboldenUnemboldened`).
4. **SD mode.** A synthetic style has no style block of its own. It maps the source file's block and reads that file's outlines from the card. Pinned by Task 3's `TtfFontSetTest.SyntheticBoldInSdModeIsDrawnFromTheRegularFile`.
5. **A store written by an older build, and hostile fonts.**
   - Until now a regular-only family's bold fell back to the regular's or the italic's font, whose glyphs carry those styles' bits. No store can hold a key a synthetic style uses, so `kRenderVersion` stays. The reviewer checks this against `TtfFont::storeKey` and the pair style in `kerning`.
   - A damaged outline is emboldened without a crash or an overflow: Task 1's `TtfCorruptFont.*` run emboldened, under the sanitizers.

---

### Task 1: The emboldening, bit for bit with FreeType, TrueType and CFF

**Files:**
- Modify: `lib/EpdFont/ttf/TtfRasterizer.h`, `lib/EpdFont/ttf/TtfRasterizer.cpp`
- Modify: `test/ttf_rasterizer/FreeTypeReference.h`, `test/ttf_rasterizer/FreeTypeReference.cpp`
- Modify: `test/ttf_rasterizer/TtfVsFreeTypeTest.cpp`, `test/ttf_rasterizer/TtfCorruptFontTest.cpp`
- Create: `test/ttf_rasterizer/TtfEmboldenTest.cpp`. In `test/ttf_rasterizer/CMakeLists.txt` it goes after `TtfVsFreeTypeTest.cpp`; `TTF_FIXTURE_DIR` is defined there already.

**Interfaces:**
- Produces, in `TtfRasterizer.h` after `mergePlans`:
  ```cpp
  /// How much synthetic bold grows a glyph (spec 7.2): the outline's growth on each axis, 26.6 pixels.
  struct Strength {
    int32_t x = 0;
    int32_t y = 0;
  };
  /// Synthetic bold's strength at `ppem26_6`.
  Strength emboldenStrength(uint32_t ppem26_6);
  ```
- Produces: `ttf::Status ttf::Rasterizer::embolden(const Strength& strength, EpdGlyph* metrics)`, public, after `load()`.
- Produces: `ReferenceGlyph ttf_test::FreeTypeReference::glyph(uint32_t index, bool oversampleOverlaps = false, long xStrength = 0, long yStrength = 0) const`.
- Produces (test file only): `compareWithFreeType(name, data, bytes, face, points, Comparison* out, const Strength& strength = {})`.

- [ ] **Step 1: Let the oracle embolden.** In `FreeTypeReference::glyph`, right after `out.linearAdvance = ...`:
  ```cpp
  if (xStrength != 0 || yStrength != 0) FT_Outline_EmboldenXY(&slot->outline, xStrength, yStrength);
  ```
  The header's comment on `glyph` gains: "A non-zero strength (26.6) emboldens the outline first, with FT_Outline_EmboldenXY." The test adds the strength to the expected advance itself; `FT_GlyphSlot_AdjustWeight` would move `horiAdvance`, which the converter's advance does not read.

- [ ] **Step 2: Write the failing tests.**
  - **`compareWithFreeType`** takes `const Strength& strength = {}`.
    - With a zero strength it runs exactly as today.
    - With a non-zero strength:
      - it asks `reference.glyph(g, false, strength.x, strength.y)`;
      - the engine runs `load()`, then `embolden(strength, &metrics)`, then `rasterize(rasterizer.output())`;
      - the plan's `outputBytes` is set to `kMaxOutputBytes`, so every emboldened glyph under 4 KB is compared, not only those the unemboldened plan has room for;
      - the expected advance is `((expected.linearAdvance + 2048) >> 12) + (strength.x + 2) / 4`;
      - the `printf` line names the strength (`emboldened by %d,%d/64`).
  - **`TEST_P(TtfVsFreeType, EveryGlyphEmboldenedMatchesFreeType)`**, after `EveryGlyphMatchesFreeType`, and **`TEST_P(TtfVsFreeTypeCff, EveryGlyphEmboldenedMatchesFreeType)`**, after its sibling. Each loops:
    ```cpp
    for (const uint32_t points : {8u, kPoints, 40u}) {
      const Strength engine = emboldenStrength(ppemFromPoints(points));
      for (const Strength& strength : {engine, Strength{engine.x, engine.x}}) {  // the engine's; FreeType's own
        SCOPED_TRACE(testing::Message() << points << " pt, " << strength.x << "," << strength.y << "/64");
        Comparison c;
        compareWithFreeType(GetParam(), data, bytes, face, points, &c, strength);
        if (HasFatalFailure()) return;
        // ...the sibling test's expectations, unchanged
      }
    }
    ```
    The expectations are the sibling's:
    - TrueType: `c.glyphs > 1000`, and 0 for `loadFailures`, `advanceDiffs`, `boxDiffs` and `inexact`;
    - CFF: `c.glyphs > 0`, `c.refusedAsFreeType == c.freeTypeFails`, 0 for `advanceDiffs` and `boxDiffs`, and `inexact` 0 or within `cffPixelTolerance()`.
  - **`TtfEmbolden.AnOutlineOfEitherDirectionGrowsOutwards`**, in `TtfEmboldenTest.cpp` (spec 7.2: "the outline's orientation is computed, not assumed: book fonts come in either direction"). The font is a `TtfBuilder` font, 1000 units per em, drawn at 40 pt with `emboldenStrength(ppemFromPoints(40))` and planned with `planScratch(face, ppem, kPoolBytesFlash)`. It has three glyphs:
    - glyph 1: a ring as TrueType draws it, the outer contour clockwise (`rect(100, 0, 500, 700)` reversed) and the inner one `rect(200, 100, 400, 600)`;
    - glyph 2: the same ring as CFF draws it, the outer `rect(100, 0, 500, 700)` and the inner reversed;
    - glyph 3: a contour of the two points (100, 0) and (500, 700), whose area is 0, so FreeType cannot orient it.

    The assertions, with FreeType's figures:
    ```cpp
    // glyphs 1 and 2, each: unemboldened 34 x 59 at left 8, top 59
    EXPECT_EQ(34 + 4, bold.width);           // grown across by the strength (221/64 px)
    EXPECT_EQ(regular.left, bold.left);      // the left side bearing stays
    EXPECT_GT(ink(bold), ink(regular));      // ink = sum of the 2-bit values
    // the two rings: the same box (width, height, left, top) once emboldened
    // glyph 3: the box unchanged, the advance grown all the same
    EXPECT_EQ(flat.width, flatBold.width);  // and height, left and top
    EXPECT_EQ(flat.advanceX + (strength.x + 2) / 4, flatBold.advanceX);
    ```
  - **`TtfCorruptFontTest.cpp`**: `exercise()` draws each glyph a second time, emboldened. It calls `load()`, then `embolden({s.x, s.x})` with `s = emboldenStrength(ppem)`, then `rasterize()`, and asserts `Status::Ok || isPermanent(status)`.

- [ ] **Step 3: Run them to see them fail.**
Run: `cmake --build build/test --target TtfRasterizerTest -j 8`
Expected: the build fails: `Strength`, `emboldenStrength` and `Rasterizer::embolden` are not declared.

- [ ] **Step 4: Declare the interface.**
  - In `TtfRasterizer.h`: the `Strength` block above.
  - `embolden`'s comment: "Synthetic bold (spec 7.2): emboldens the outline of the last load() exactly as FreeType's FT_Outline_EmboldenXY does with `strength`, and updates its `metrics` the way FT_GlyphSlot_AdjustWeight does: the bitmap box from the new outline, the advance strength.x wider (12.4, rounded). An outline FreeType cannot orient stays as it is, the advance growing all the same. After a failed load() it returns that status; a bitmap that no longer fits reports what load() would."
  - The private part gains `int32_t left_ = 0; int32_t bottom_ = 0; uint32_t ppem_ = 0;` ("the last load()'s size and bitmap box, whose bottom-left corner the outline was moved to") and `Status placeBox(EpdGlyph* metrics);`.

- [ ] **Step 5: Port FreeType's emboldening** into `TtfRasterizer.cpp`'s anonymous namespace, after `inRange()`: `msb`, `normLen`, `enum class Orientation { TrueType, PostScript, None }`, `orientation` and `emboldenXY`.
  - These are FreeType 2.13.3's functions statement for statement. The SDK copy is under `freeink-sdk/libs/font/FreeInkFont/third_party/freetype/src/base/`: `FT_Vector_NormLen` at `ftcalc.c:800`, `FT_Outline_Get_Orientation` at `ftoutln.c:1051` and `FT_Outline_EmboldenXY` at `ftoutln.c:917`.
  - The prototype has the port, and it is correct for both orientations: copy `git show proto/synthetic-bold:lib/EpdFont/ttf/TtfRasterizer.cpp`, lines 64–239. The copy keeps these choices:
    - `normLen` stays in FreeType's own 32-bit types: its wrap-arounds are part of the result.
    - `mulFix` and `mulDiv` from `TtfFixed.h` stand for `FT_MulFix` and `FT_MulDiv`.
    - The orientation's area is an `int64_t`. That is FreeType's `FT_Long` on CI's LP64 build. With a 32-bit long, `ADD_LONG` wraps, but no outline that passes `load()`'s box check comes near that.
    - `emboldenXY` returns `false`, with the outline untouched, where FreeType returns an error: an outline with contours that it cannot orient.

- [ ] **Step 6: Split `load()`'s box step out, and add `embolden()` and `emboldenStrength()`.**
  - **`load()`** keeps everything through the advance check (both loaders, the `-pp1x` shift, `advance12_4`). It then sets `metrics->advanceX`, `numPoints_`, `numContours_`, `ppem_ = ppem26_6` and `left_ = bottom_ = 0`, and returns `loaded_ = placeBox(metrics)`.
  - **`placeBox()`** is `load()`'s former box code, moved (prototype lines 587–633):
    - it reads `numPoints_` and `ppem_`;
    - it returns its two failures without touching `loaded_`: `Unsupported`, and `ppem_ > plan_.maxPpem26_6 ? Transient : Unsupported`;
    - it translates the points in a loop over `numPoints_`, because the `Loader` is gone by then;
    - it records `left_` and `bottom_`, and fills `width`, `height`, `left`, `top`, `dataLength`, `width_` and `height_`.
  - **`embolden()`** (prototype lines 635–651):
    - after a failed load it returns `loaded_`;
    - the advance grows by `(strength.x + 2) / 4`, past `0xFFFF` it is `Unsupported`;
    - with no points it returns `Ok`;
    - otherwise it moves the points back by `(left_ * 64, bottom_ * 64)`, because the orientation reads absolute coordinates;
    - it runs `emboldenXY`, ignoring its result as `FT_GlyphSlot_AdjustWeight` does;
    - it returns `loaded_ = placeBox(metrics)`.

    It touches only the points and the contour ends. The CFF interpreter's state in the cell pool is dead once `load()` returns, and the pool is ftgrays' only inside `rasterize()`.
  - **`emboldenStrength()`**, for now FreeType's own on both axes (Task 2 settles it): `s = static_cast<int32_t>((ppem26_6 + 32) >> 6) * 0x0AAA / 1024; return {s, s};`. Here `x_ppem` is rounded as `FT_Request_Metrics` rounds it.

- [ ] **Step 7: Run the rasterizer tests**, here and as CI's LP64 build runs them.
Run: `cmake --build build/test --target TtfRasterizerTest -j 8 && ./build/test/ttf_rasterizer/TtfRasterizerTest.exe`
Expected:
- PASS, every test.
- Each emboldened line prints `0 load failures, 0 advance diffs, 0 box diffs, 0 inexact`, for the 12 TrueType fonts and the 10 CFF fonts at 8, 14 and 40 pt, in both runs.
- Ubuntu's wordmark is still too large at 40 pt, as it is unemboldened.
- A differing pixel is a port bug until shown otherwise: the port is FreeType's code. `kCffPixelTolerance` takes an entry only for a rounding limit that has been proven.

Then: `C:\msys64\usr\bin\bash.exe -lc "bash /tmp/ftprobe/build_tests.sh /c/tmp/<worktree> '*EveryGlyph*'"`
Expected: `[  PASSED  ]`, no failure.

- [ ] **Step 8: Commit.**
```bash
git add lib/EpdFont/ttf/TtfRasterizer.h lib/EpdFont/ttf/TtfRasterizer.cpp test/ttf_rasterizer/FreeTypeReference.h \
  test/ttf_rasterizer/FreeTypeReference.cpp test/ttf_rasterizer/TtfVsFreeTypeTest.cpp \
  test/ttf_rasterizer/TtfCorruptFontTest.cpp test/ttf_rasterizer/TtfEmboldenTest.cpp test/ttf_rasterizer/CMakeLists.txt
git commit -m "feat(ttf): synthetic bold, FreeType's outline emboldening in integers"
```

### Task 2: Across only, with room for the growth

**Files:**
- Modify: `lib/EpdFont/ttf/TtfRasterizer.h`, `lib/EpdFont/ttf/TtfRasterizer.cpp` (`emboldenStrength`, `planScratch`)
- Modify: `test/ttf_rasterizer/TtfEmboldenTest.cpp`, `test/ttf_rasterizer/TtfRasterizerTest.cpp`
- Modify: `test/fixtures/fonts/README.md` (one sentence). No new fixture: the subsets already hold 鬱 書 慶 議 驚.

**Interfaces:**
- Consumes: Task 1's `Strength`, `emboldenStrength`, `Rasterizer::embolden`.
- Produces: `emboldenStrength()` returns `{x_ppem * 0x0AAA / 1024, 0}`.
- Produces: `ttf::ScratchPlan ttf::planScratch(const Face& face, uint32_t maxPpem26_6, uint32_t poolBytes, bool emboldened = false)`.

- [ ] **Step 1: Write the failing tests.**
  - **`TEST_F(TtfEmbolden, DenseCharactersKeepTheirCountersOpen)`**, in `TtfEmboldenTest.cpp`. Its comment: "Synthetic bold on dense CJK characters (spec 7.2): at reading sizes the strength must leave their counters open."
    ```cpp
    const uint32_t dense[] = {0x9B31, 0x66F8, 0x6176, 0x8B70, 0x9A5A};  // 鬱 書 慶 議 驚
    for (const char* name : {"NotoSansJP-TrueType.ttf", "NotoSansJP-Cff.otf", "NotoSerifJP-Cff.otf"}) {  // readFixture
      for (const uint32_t points : {10u, 12u, 14u}) {
        // planScratch(face, ppem, kPoolBytesFlash, true); per character: render() the regular,
        // then load(), embolden(emboldenStrength(ppem)), rasterize()
        // kept: a regular blank pixel (2-bit value 0, inside the box) that is blank in the bold bitmap at the same
        // absolute position (index by regular.left - bold.left and bold.top - regular.top)
        // counters: 4-connected blank regions of the regular glyph, 5+ pixels, not touching the box edge
        std::printf("%s %u pt U+%04X: %u of %u blank pixels kept, %u of %u counters open\n", name, points, cp, kept, open,
                    countersOpen, counters);
        EXPECT_GE(kept * 3, open * 2);
        EXPECT_EQ(counters, countersOpen);        // no enclosed counter fills
        EXPECT_GT(ink(bold), ink(regular));       // grown outwards: TrueType and CFF outlines run opposite ways
        EXPECT_EQ(regular.top, bold.top);         // across only: the baseline...
        EXPECT_EQ(regular.height, bold.height);   // ...and the height stay
      }
    }
    ```
  - **`TEST(TtfScratch, AnEmboldenedPlanHasRoomForTheGrowthAndKeepsTheCffCaps)`**, in `TtfRasterizerTest.cpp`, set up as `ACffFaceIsPlannedAtTheCapsWithTheInterpretersState` (a CFF `Face`, 1000 units per em, a box of 1000 by 1000):
    ```cpp
    const ScratchPlan plain = planScratch(face, 1000, kPoolBytesFlash);
    const ScratchPlan bold = planScratch(face, 1000, kPoolBytesFlash, true);
    EXPECT_EQ(81u, plain.outputBytes);
    EXPECT_EQ(90u, bold.outputBytes);  // 20 x 18: two pixels across for emboldenStrength(1000).x = 42/64, none down
    EXPECT_EQ(kMaxScratchPoints, bold.points);
    EXPECT_EQ(kMaxScratchContours, bold.contours);
    EXPECT_EQ(kCffDecoderBytes, bold.decoderBytes);
    EXPECT_EQ(kMaxOutputBytes, planScratch(face, kMaxPpem26_6, kPoolBytesFlash, true).outputBytes);
    ```

- [ ] **Step 2: Run them to see them fail.**
Run: `cmake --build build/test --target TtfRasterizerTest -j 8`
Expected: the build fails on `planScratch`'s fourth argument.

Add the parameter (Step 3's `planScratch` part only), build again, and run `./build/test/ttf_rasterizer/TtfRasterizerTest.exe --gtest_filter='TtfEmbolden.*:TtfScratch.*'`.
Expected: FAIL.
- With FreeType's strength on both axes (`emboldenStrength` temporarily returning {s, s}) the dense test fails on all 45 cases (kept under two thirds) and on counters that fill; tops and heights differ.
- `AnEmboldenedPlan...` reports 100 output bytes against 90.

- [ ] **Step 3: Room for the growth, and the strength across only.**
  - **`planScratch`**, when `emboldened`, widens the planned box by `2 * ((grow.x + 63) >> 6)` across and `2 * ((grow.y + 63) >> 6)` down, with `grow = emboldenStrength(plan.maxPpem26_6)`. This comes before the em and 255 clamps. It applies to either outline kind: the CFF branch keeps its caps and `decoderBytes`. Its header comment gains: "`emboldened`: with room for synthetic bold's growth too, twice its strength each way (a sharp corner reaches past the strength)."
  - **`emboldenStrength`** returns `{static_cast<int32_t>((ppem26_6 + 32) >> 6) * 0x0AAA / 1024, 0}`. Its comment gives the reason and the measurement: "With FreeType's vertical strength too, 鬱 書 慶 議 驚 stays blank in 35–57% of cases at 10–14 pt and 66 enclosed counters fill; across only, in 69–91% and none fills. The stems carry the weight, in Latin text too."
  - **The fixtures' `README.md`** gains: "`TtfEmboldenTest` reads the dense kanji 鬱書慶議驚 of `NotoSansJP-TrueType.ttf`, `NotoSansJP-Cff.otf` and `NotoSerifJP-Cff.otf` (synthetic bold's counters, spec 7.2)."

- [ ] **Step 4: Run the rasterizer tests.**
Run: `cmake --build build/test --target TtfRasterizerTest -j 8 && ./build/test/ttf_rasterizer/TtfRasterizerTest.exe`
Expected:
- PASS, every test.
- `TtfEmbolden` prints its 45 lines. The lowest is 鬱 in `NotoSansJP-TrueType.ttf` at 12 pt, 173 of 252 blank pixels kept (69%); every line has all its counters open.
- Both emboldened oracle runs pass, now at two different strengths.

- [ ] **Step 5: Commit.**
```bash
git add lib/EpdFont/ttf/TtfRasterizer.h lib/EpdFont/ttf/TtfRasterizer.cpp test/ttf_rasterizer/TtfEmboldenTest.cpp \
  test/ttf_rasterizer/TtfRasterizerTest.cpp test/fixtures/fonts/README.md
git commit -m "feat(ttf): synthetic bold grows across only, so dense CJK characters stay open"
```

### Task 3: Which styles are synthetic, and how TtfFont draws them

**Files:**
- Modify: `lib/EpdFont/ttf/TtfFace.h`, `lib/EpdFont/ttf/TtfFace.cpp` (`Face::bold`); Test: `test/ttf_rasterizer/TtfFaceTest.cpp`
- Modify: `lib/EpdFont/ttf/TtfFont.h`, `lib/EpdFont/ttf/TtfFont.cpp`; Test: `test/ttf_font/TtfFontTest.cpp`

**Interfaces:**
- Consumes: Task 1's `Rasterizer::embolden`, and Task 2's `emboldenStrength` and `planScratch(..., true)`.
- Produces: `bool ttf::Face::bold`: head.macStyle bit 0, or an OS/2 usWeightClass of 600 or more, read from either outline kind.
- Produces: `bool TtfFont::synthetic(uint8_t style) const`. `font()`, `nativeFonts()`, `glyph()`, `kerning()` and `prefetch()` serve a synthetic style as they serve one with a file.

- [ ] **Step 1: The failing face test**, in `TtfFaceTest.cpp`:
  ```cpp
  TEST(TtfFace, BoldIsMacStyleOrAWeightOf600InEitherOutlineFormat) {
    // Spec 7.1: read alike from a TrueType face and a CFF one.
    for (const bool cff : {false, true}) {
      const auto bold = [cff](const uint16_t macStyle, const bool os2, const uint16_t weight) {
        Font font = threeGlyphs();
        if (cff) {
          font.sfntVersion = tagOf("OTTO");
          font.extraTables = {{"CFF ", buildCff(CffSpec{})}};
        }
        font.macStyle = macStyle;
        font.os2 = os2;
        font.weightClass = weight;
        Face face;
        EXPECT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
        return face.bold;
      };
      EXPECT_FALSE(bold(0, false, 400)) << cff;
      EXPECT_TRUE(bold(1, false, 400)) << cff;
      EXPECT_FALSE(bold(2, false, 400)) << cff;  // italic
      EXPECT_FALSE(bold(0, true, 599)) << cff;
      EXPECT_TRUE(bold(0, true, 600)) << cff;
    }
  }
  ```

- [ ] **Step 2: Run it to see it fail.**
Run: `cmake --build build/test --target TtfRasterizerTest -j 8`
Expected: the build fails: `Face` has no member `bold`.

- [ ] **Step 3: `Face::bold`.**
  - In `TtfFace.h`, after `longLoca`: `/// A bold face: head.macStyle says bold, or OS/2's weight class is 600 or more. Never emboldened (spec 7.1).` `bool bold = false;`.
  - In `parseFace`, `face->bold = (bytes.u16(head.offset + 44) & 1) != 0;  // macStyle` goes beside the head box reads (`face->xMin = ...`), which both outline kinds reach. It must not go beside `face->longLoca`, which on master is inside the TrueType branch (where the prototype has it).
  - After `haveOs2` is settled: `if (haveOs2 && bytes.u16(os2.offset + 4) >= 600) face->bold = true;  // usWeightClass`.

- [ ] **Step 4: Run it.** `./build/test/ttf_rasterizer/TtfRasterizerTest.exe --gtest_filter='TtfFace.*'`. Expected: PASS.

- [ ] **Step 5: The failing font tests**, in `TtfFontTest.cpp`.
  - **Helpers**, beside `referenceAt()` and `reference()`:
    - `constexpr uint8_t kBoldItalic = 3;`;
    - `Reference emboldenedAt(const std::vector<uint8_t>& file, uint32_t cp, uint32_t ppem)`: `referenceAt` with `planScratch(..., true)`, and `load()`, then `embolden(ttf::emboldenStrength(ppem))`, then `rasterize()`;
    - `Reference emboldened(const std::vector<uint8_t>& file, uint32_t cp, uint8_t points)`.
  - **Two existing tests change:**
    - in `ABrokenGlyphIsMarkedMissingAndNotTriedAgain` the last line becomes `EXPECT_EQ(nullptr, font->font(kItalic));  // a one-style family; its bold is synthetic (spec 7.1)`;
    - in `AFamilyHoldsMemoryOnlyForTheStylesItHas`, `built.macStyle = 1;` follows `built.extraTables.clear();`. The face is bold-declared, so no style is synthetic and each style is a file; `ASyntheticStyleCostsItsFontsAndNoFile` counts the synthetic ones.
  - **New tests**, in a new section at the end of the file, `// --- Synthetic bold (spec 7.1) ---`, after the helpers they use (`kernedBoxes`, `nativeGlyph`, `mappedCodepoints`, `tooLargeForFlashMode`, `openFamilies`, `kSdOnlyBytes`). Each family is opened under its own identity:

    | Test | Family | Assertions |
    |---|---|---|
    | `ARegularOnlyFamilyDrawsBoldAndBoldItalicEmboldened` | Bookerly Regular alone | `synthetic()` is false, true, false, true for styles 0–3; `font(kBold)` and `font(kBoldItalic)` are non-null and not `font(kRegular)`; `font(kItalic) == nullptr`; 'W' in `kBold` and `kBoldItalic` `expectDrawnAs(emboldened(regular, 'W', 14))`; `kRegular` as `reference(...)` |
    | `BoldItalicIsEmboldenedFromTheItalicWhenThereIsOne` | Bookerly Regular and Italic | both synthetic; 'W' in `kBold` = `emboldened(regular)`, in `kBoldItalic` = `emboldened(italic)`, in `kItalic` = `reference(italic)` |
    | `AFamilyWithABoldFileIsNeverEmboldened` | Bookerly Regular, Bold, Italic (spec 7.1 table, row 1) | no style synthetic; `font(kBoldItalic) == nullptr`; through `EpdFontFamily(font(0..3))`, BOLD_ITALIC 'W' has `reference(files[kBold], 'W', 14)`'s advance: today's fallback, the real upright Bold |
    | `ABoldFileServingAsTheRegularIsNotEmboldenedAgain` | `Bookerly/Bookerly-Bold.ttf`, then `OpenDyslexic/OpenDyslexic-Bold.otf`, each alone in the regular slot as `assignStyles` moves it (row 2) | `!synthetic(kBold)`, `font(kBold) == nullptr` |
    | `ASingleHeavyFaceIsNotEmboldened` | `Inter/Inter-Bold.ttf` alone: macStyle regular, weight 700 (row 3) | `!synthetic(kBold)`, `font(kBold) == nullptr` |
    | `AnOtfFamilyIsEmboldenedAsATrueTypeOne` | `OpenDyslexic-Regular.otf` and `OpenDyslexic-Italic.otf` | bold and bold italic synthetic; 'W' = `emboldened` of each source; `kerning(kBold, '!', ' ') == kerning(kRegular, '!', ' ')`, and both > 0 |
    | `SyntheticBoldIsStoredUnderTheBoldStyleAndServesTheNextBoot` | Bookerly Regular alone | first boot: `glyph(kBold, 'W')` and `glyph(kRegular, 'W')` non-null, `renders() == 2` (the regular's key is not the bold's); next boot: both drawn as their references, `renders() == 0`, `storeHits() == 2` |
    | `SyntheticBoldKernsAsItsFileAndIsWiderByItsStrength` | `kernedBoxes()` | `synthetic(kBold)`; `kerning(kRegular, 'A', 'B') == -47` and `kerning(kBold, 'A', 'B') == -47` with `kernLookups() == 1`; bold 'A' `advanceX == regular + (emboldenStrength(ppemFromPoints(14)).x + 2) / 4`; `left` and `top` equal |
    | `SyntheticBoldDrawsEveryGlyphAtTheLargestNativeSize` | Bookerly Regular alone, then `NotoSansJP-CffFlat.otf` alone | at `nativeFonts(maxNativePx())`, every codepoint the regular draws (Bookerly U+0021–U+024F, more than 300; the CFF subset's `mappedCodepoints()`, more than 250) has a bold glyph equal to `emboldenedAt(file, cp, px(maxNativePx()))` |
    | `ASyntheticStyleCostsItsFontsAndNoFile` | two one-file families of `kernedBoxes()` without kerning, one with `macStyle = 1` | measured as `AFamilyHoldsMemoryOnlyForTheStylesItHas` measures (opened once to install, then counted): the regular one's family and its native size at 40 px each hold more, by under 512 B; `fonts[kBold] != nullptr` only for the regular one; prints `[  heap    ] a one-file family without and with synthetic bold: %zu/%zu B; a native size: %zu/%zu B` |
    | `TtfFontSetTest.SyntheticBoldInSdModeIsDrawnFromTheRegularFile` | `tooLargeForFlashMode("NotoSerifJP-CffFlat.otf")` on `SimNorFlash chip{kSdOnlyBytes}`, through `openFamilies` | `readsCard()`; `prefetch(kBold, <the ten kana あいうえおかきくけこ of ACffFamilyDrawsInSdMode>)` gives `renders() == 10` and more than 0 but at most 11 new `sdPageReads()`; each kana in `kBold` = `emboldened(file, cp, 14)` with `renders()` still 10 (stored by the batch); 'あ' in `kRegular` = `reference(...)`; `expectWrittenOnce(chip)` |

- [ ] **Step 6: Run them to see them fail.**
Run: `cmake --build build/test --target TtfFontTest -j 8`
Expected: the build fails: `TtfFont` has no member `synthetic`.

- [ ] **Step 7: Fonts apart from files.**
  - **`TtfFont.h`**:
    - public, after `font()`: `bool synthetic(const uint8_t style) const { return style < 4 && (synthetic_ & (1u << style)) != 0; }`. `font()`'s comment gains "A family none of whose files is bold draws bold, and bold italic without a file of its own, emboldened (spec 7.1).";
    - private members: `fileSlot_[4]` (a file's place in `styles_`), `slot_[4]` (a drawn style's place in each `Size`'s fonts), `source_[4]` (the style whose file draws it: its own, or for a synthetic style the regular or the italic), `uint8_t fontCount_`, and `uint8_t synthetic_` (one bit per style). All start at `kNoSlot` or 0;
    - `styleAt(style)` follows the drawing file: `source_[style] != kNoSlot ? &styles_[fileSlot_[source_[style]]] : nullptr`. `draws()` is unchanged;
    - new: `void chooseSynthetic();` and `ttf::Status load(FontBytes& bytes, const ttf::Face& face, uint16_t index, const Size& size, uint8_t style, EpdGlyph* metrics);`;
    - `Size::styles`' comment: "one for each style the family draws".
  - **`openStyles`**:
    - the first-load block sets `fileSlot_[s]` and `source_[s] = s` for each style with a file, and allocates `styles_` only. The loop takes `styles_[fileSlot_[s]]`;
    - after `if (!draws(0)) return StylesResult::Failed;`, when `body_.styles` is still null: `chooseSynthetic()`, then `body_.styles = makeUniqueNoThrow<Size::StyleFont[]>(fontCount_)`. On OOM it logs, sets `set_.failedForNow_ = true` as master's OOM paths do, and returns `Failed`;
    - then, for each synthetic style the family draws, it merges `planScratch(styleAt(s)->face, largest, pool, true)` into `*plan`. `largest` and `pool` follow the file loop's rule (`kPoolBytesSd` for family 0 on the card).
  - **`chooseSynthetic()`** applies the rule:
    - a file in slot 1 or 3, or any drawn style whose `face.bold` is set, makes the family bold;
    - otherwise `source_[1] = 0`, `source_[3] = draws(2) ? 2 : 0`, and `synthetic_` gets bits 1 and 3;
    - then `slot_` numbers every style that has a source, which gives `fontCount_`.
  - **`rebase`** sets `size.styles[slot_[s]].data.bitmap` for each `s` with `draws(s)`. **`nativeSize`** allocates `fontCount_` fonts. `configure`, `font()` and `nativeFonts()` are unchanged: through `slot_` and `styleAt()`, a synthetic style takes its source's line metrics and ligature table.
  - **`render` and `prefetch`** call `load(bytes, face, index, size, style, &metrics)` instead of `placeable(set.rasterizer_.load(...), metrics)`; prefetch passes `p.glyph`. `load()` is the rasterizer's `load()`, then `embolden(ttf::emboldenStrength(size.ppem), metrics)` when `synthetic(style)`, then `placeable()`.
  - **`kerning`**'s style parameter is renamed `drawn`. `styleAt(drawn)` finds the file. `fontBytes`, `findPair` and `storePair` use `const uint8_t style = source_[drawn];`, with the comment: "A synthetic style kerns as its file does, with the file's style's pairs: every glyph is the same amount wider."
  - **Nothing in `TtfFontSet`** changes: `install()` and the style blocks count files, and a synthetic style has none.

- [ ] **Step 8: Run the font tests.**
Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe`
Expected:
- PASS, 188 tests.
- The heap line shows about 480 B more with synthetic bold for the family and for a native size (two fonts of about 240 B on the 64-bit host). On the prototype it read `1872/2352 B; 272/752 B`. The family's absolute figure is larger on master because `Face` holds the CFF fields.

- [ ] **Step 9: Commit.**
```bash
git add lib/EpdFont/ttf/TtfFace.h lib/EpdFont/ttf/TtfFace.cpp test/ttf_rasterizer/TtfFaceTest.cpp \
  lib/EpdFont/ttf/TtfFont.h lib/EpdFont/ttf/TtfFont.cpp test/ttf_font/TtfFontTest.cpp
git commit -m "feat(ttf): a family without a bold file draws bold and bold italic emboldened"
```

### Task 4: Section version, bench, docs, firmware

**Files:**
- Modify: `lib/Epub/Epub/Section.cpp`, `bench/ttf_raster_main.cpp`, `docs/file-formats.md`, `USER_GUIDE.md`

- [ ] **Step 1: The section version**, with its history line on top:
  ```cpp
  constexpr uint8_t SECTION_FILE_VERSION = 97;  // v97: bold in an outline family without a bold
                                                // file is synthetic, wider than the regular
                                                // v96: CJK punctuation is narrowed by the book's
  ```

- [ ] **Step 2: The bench times a bold miss** (spec 7.3; spec 9.6: "the CJK samples as CFF against the same characters as TrueType, at 12 pt, regular and bold").
  - `renderGlyphs` gains `const bool bold = false`. It runs `load()`, then `embolden(emboldenStrength(ppem26_6), &metrics)` when `bold`, then `rasterize(rasterizer.output())`, and times the three together as a miss. `renderAll` passes `bold` on.
  - Bookerly's rasterizer is planned with `planScratch(face, heading, kPoolBytesFlash, true)`. After `render_40pt_pool4k` the bench reports `bold_14pt_pool4k` and `bold_40pt_pool4k`.
  - In `benchJapanese`, the 8 KB-pool rasterizer is planned with `true`. After `<name>_kanji_12pt_pool8k` it reports `<name>_kanji_12pt_pool8k_bold`, for `truetype`, `cff_flat` and `cff_subrs`.
  - The file's header comment names the bold runs.
  - `platformio.ini` stays: the Japanese fixtures are embedded already.
  - Bookerly's 14 pt hash must still match the host's.

- [ ] **Step 3: Docs.**
  - `docs/file-formats.md`, the glyph store's face bullet, after the key layout: "A synthetic bold style (spec 7.1), drawn from another style's file, keys its glyphs under its own style bits and the face index of its size, like a style with a file; its kerning pairs are those of the style it is drawn from."
  - `USER_GUIDE.md`, **EPUB Font** → **Font Family**: "A TrueType or OpenType font without a bold file draws bold text emboldened." **Publisher Fonts**: "A book font without a bold face draws bold text emboldened."

- [ ] **Step 4: Build the firmware and the bench**, one at a time, in the background:
  - `PLATFORMIO_CORE_DIR=C:/pio pio run -e default`
  - `PLATFORMIO_CORE_DIR=C:/pio pio run -e bench_ttf_raster`

Expected: `SUCCESS` each, and `check_no_float: <n> TTF engine objects, no floating point` on the firmware build. Note the flash size against master's.

- [ ] **Step 5: Run the whole host suite, format, commit.**
Run: `cmake --build build/test -j 8 -- -k 0 && (cd build/test && ctest -j 8 --output-on-failure)`
Expected: all pass but master's known Windows-only failures: `LongChapterPageTables` ×2, `FirstChapter` ×2 and `KeptReadingState` (each a teardown "file in use"), and `epub_build_inventory`, which does not build (`dlfcn.h`). Compare with master's run.
```bash
PATH="<clang-format 21 dir>:$PATH" bash bin/clang-format-fix
git add lib/Epub/Epub/Section.cpp bench/ttf_raster_main.cpp docs/file-formats.md USER_GUIDE.md
git commit -m "feat(ttf): section version 97 for synthetic bold; the bench times a bold miss"
```

### Task 5: On the devices

**Files:** none changed. The figures and the screenshots go into the PR description.

- [ ] **Step 1: The bench on the X4.** Run `PLATFORMIO_CORE_DIR=C:/pio pio run -e bench_ttf_raster -t upload --upload-port <port>` and read the serial output to `=== done ===`; stop the logger before flashing again.
Expected: `output hash ... MATCHES`. Record each pair's `avg=`:
- `render_14pt_pool4k` against `bold_14pt_pool4k`;
- `render_40pt_pool4k` against `bold_40pt_pool4k`;
- `<name>_kanji_12pt_pool8k` against `<name>_kanji_12pt_pool8k_bold`, for TrueType and the two CFF fonts.

The prototype on the X4 (2026-10-20) took 480 against 819 µs and 1,229 against 1,610 µs. Its CJK figure (670 against 1,321 µs) came from a seven-glyph TrueType subset and does not compare with the kanji runs. The figures are recorded, not a pass mark.

- [ ] **Step 2: Flash the reader.** On the X4: `PLATFORMIO_CORE_DIR=C:/pio pio run -e default -t upload --upload-port <port>`. The maintainer flashes the X3.

- [ ] **Step 3: Fonts on the card.** Run `python tools/devctl.py cmd:FS:LS:/.crosspoint/fonts`. The CFF plan's device run left two folders:
  - `OpenDyslexicOtf/`: `OpenDyslexic-Regular.otf` alone, a regular-only CFF Latin family;
  - `NotoSansTCOtf/`: CJK, SD mode, regular only.

  Copy back any that are missing, as that plan's Task 8 Step 3 made them (`tools/serial_transfer.py`, `MSYS_NO_PATHCONV=1`). Add `lib/EpdFont/builtinFonts/source/NotoSans/NotoSans-Regular.ttf` alone into `/.crosspoint/fonts/NotoSansRegularOnly/`, a regular-only TrueType family.

- [ ] **Step 4: CJK bold.** With `tools/devctl.py`:
  - first `cmd:GET:fontFamily cmd:GET:fontSize cmd:GET:publisherFonts`;
  - `cmd:SET:fontFamily=NotoSansTCOtf` (the label `GET:*` lists) and `cmd:SET:fontSize=12pt` (a size is set by its label);
  - `"cmd:ACT:READER:/test/test_cjk_zh_honglou.epub@<a chapter's spine>:1" idle:120000:SCREEN cmd:TTF shot:/c/tmp/bold-x4-honglou-12.png`;
  - the same at 26 pt, where the headings are native sizes.

Expected: `TTF:... sd=1`. The chapter headings are visibly heavier than the text, with the counters of 書, 慶 and the like open. The log has no `unsupported` lines. Claude reviews the screenshots for stroke weight and counters; the maintainer does not read CJK.

- [ ] **Step 5: Latin.**
  - `OpenDyslexicOtf` and `NotoSansRegularOnly`, each on a `/test` book with bold headings or `<b>` spans (`cmd:FS:LS:/test`); shot each page.
  - Then `cmd:SET:publisherFonts=<on>` with `/test/bookfont_cff.epub`, or another book whose embedded font has no bold face; shot a page with bold.

Expected: bold text is heavier and wider than the text around it, and no glyph is missing.

- [ ] **Step 6: The X3.** The maintainer flashes it, then Steps 3–5 run there too (never RTS or esptool). Files go onto its card without a reset: `tools/serial_transfer.py`'s protocol over a port opened as `devctl.py` opens it, on the USB Transfer screen the maintainer opens; leaving that screen restarts the X3, so the maintainer does it.

- [ ] **Step 7: Restore.** `cmd:SET` every changed setting back to the value `GET` gave. If Step 3 added `NotoSansRegularOnly`, remove it with `cmd:FS:RM:/.crosspoint/fonts/NotoSansRegularOnly`. Record the bench figures and the screenshots for the PR.
