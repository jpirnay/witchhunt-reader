# CFF outlines — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The TrueType engine draws OpenType fonts with CFF outlines (`.otf`, sfnt `OTTO`), name-keyed and CID-keyed, matching FreeType's unhinted CFF output pixel for pixel, in flash mode and SD mode, from the card and from books.

**Architecture:** Two new engine units in `lib/EpdFont/ttf/`: `TtfCff` parses the `CFF ` table (INDEXes, DICTs, FDArray/FDSelect, the font matrix) and locates a glyph's charstring; `TtfCffCharString` is FreeType's cf2 Type 2 interpreter and path builder rebuilt in integers, leaving the outline in font units in the rasterizer's scratch arrays. The rasterizer then transforms and scales it as `cff_slot_load` does and draws it with the existing ftgrays. Around it: `parseFace` and the table walker accept `OTTO`, discovery lists `.otf`, SD mode rebuilds the `CFF ` table so the charstring data is the one far range on the card, and Publisher Fonts accepts CFF faces.

**Tech Stack:** C++17 firmware (PlatformIO, ESP32-C3/S3), GoogleTest host tests (`test/ttf_rasterizer`, `test/ttf_font`) with the SDK's FreeType as the oracle, Python fontTools for the test fonts.

**Spec:** `docs/design/cjk-ttf-fonts.md`, section 9 (with sections 10 and 11). Order of work step 4; the maintainer moved it ahead of synthetic bold on 2026-10-21. Not prototyped.

**FreeType reference.** The SDK's copy is `freeink-sdk/libs/font/FreeInkFont/third_party/freetype/src/` (below: `FT/`). Every rule the port must match is in these files; a task names the lines it ports. Summary of the unhinted path (`FT_LOAD_NO_HINTING`, the cf2 engine; `CFF_CONFIG_OPTION_OLD_ENGINE` is off, stem darkening off by default):
- cf2 interprets in 16.16 (`CF2_Fixed`) at unit scale: a point becomes `FT_MulFix(X, 0x400) >> 10` (`FT/psaux/pshints.c:1180-1203`, `FT/psaux/psobjs.c:2217-2219`), i.e. integer font units, fractions floored.
- `cff_slot_load` (`FT/cff/cffgload.c:420-740`): picks the font matrix (top DICT, or for CID the FD's matrix concatenated with the top's, `FT/cff/cffobjs.c:725-814`), adjusts the scale when the FD's units per em differ from the top's (`:446-452`), applies the matrix when it is not identity (`FT_Outline_Transform`, `:689-692`), the integer offset (`:700-707`), then `FT_MulFix(point, x_scale)` per point (`:721-726`) to 26.6. No left-side-bearing shift. The advance comes from hmtx (`:627-640`).
- Emission (`FT/psaux/pshints.c:1694-1949`, `FT/psaux/psft.c:130-243`, `FT/psaux/psobjs.c:2302-2350`): zero-length lines dropped in character space and again in 16.16 device space; curves always kept; a contour closes with a line to its start only when non-zero; the closing point is removed when it equals the first after `>> 10` and is on-curve; single-point contours are dropped. Point order is kept exactly (ftgrays' fill rule is not symmetric).
- Operators (`FT/psaux/psintrp.c`): all of Type 2, including flex (`:336-412`), hint masks (`:2576-2655`; more than 96 stems fails the glyph), the arithmetic and storage operators (`roll` rotates the bottom entries, `psstack.c:297-318`), and `endchar` with 4 arguments as seac (accent first at (adx, ady), then the base; components via StandardEncoding and the charset, which in an OpenType file FreeType searches for a CID-keyed font too, `FT/psaux/cffdecode.c:422-464`). Stack 48 entries, subroutine nesting 16, bias 107/1131/32768 (`FT/psaux/cffdecode.c:402-418`).
- Arithmetic: `FT_MulFix`, `FT_DivFix`, `FT_MulDiv`, `FT_SqrtFixed` as in `FT/base/ftcalc.c:174-263, 893-954` (the engine's `TtfFixed.h` has the first three).

## Global Constraints

- Base: master (PR #531 merged). PR #532 (glyph store) touches `TtfFont.cpp` and `FlashGlyphStore`; rebase onto it if it lands first. Synthetic bold is not merged; it works on cubic outlines unchanged when it lands.
- Integer only: every new engine source lives in `lib/EpdFont/ttf/`, which `scripts/check_no_float.py` checks.
- TrueType output stays byte-identical: `TtfVsFreeType.*`, the Bookerly golden hash shared with `bench/ttf_raster_main.cpp`, and every existing `test/ttf_font` test pass unchanged. `ttf::kRenderVersion` (2), `SECTION_FILE_VERSION` and the glyph store's format stay.
- The bar (spec 9.3): every glyph of the CFF test fonts at 8, 14 and 40 pt: advances and boxes exact; bitmaps byte-identical, or, if rounding makes that impossible for a font, at most 3% differing pixels per font and size in all, recorded. A glyph FreeType cannot load is one the engine reports as Malformed or Unsupported.
- Not read: CFF2 (variable OpenType, a `CFF2` table), bare CFF and Type 1 files: Unsupported, as today.
- Heap: no new allocation. The interpreter's state lives in the scratch block (`ScratchPlan::decoderBytes`, 0 for a TrueType face); the Face grows by the `CffFace` fields only.
- Robustness: every read goes through `FontBytes` (bounds-checked); a glyph's interpretation stops after `kCffMaxOperators` (65,536) operators, far above any real glyph and far below FreeType's 20,000,000, so a hostile book cannot stall the reader.
- Comments say what the code does and why; no history narration. clang-format 21.1.8 (`bin/clang-format-fix` with it first on the PATH).
- Host tests from Git Bash: configure with `-DFETCHCONTENT_SOURCE_DIR_SDFAT=/c/tmp/SdFat-2.3.1` (and the googletest/ArduinoJson source dirs as before); build with `cmake --build build/test -j 8 -- -k 0`. Three targets never build on Windows (`PngDecoderArenaTest`, `BookMetadataCacheTest`, `EpubPipelineTest`).
- Firmware: `pio run -e default` with `PLATFORMIO_CORE_DIR=C:/pio`, one `pio run` at a time, in the background; `x4pro` and `lilygo_t5s3` are left to CI.
- Devices: never pulse RTS or run esptool against an X3. Read each setting with `CMD:GET` before changing it and restore it afterwards (the X4 runs in German).

## Review Focus

1. **A hostile or broken CFF in a book** (offsets past the table, an INDEX count of 65,535, subroutines calling themselves, a charstring that loops through subroutines, a stack overflow, an FDSelect naming an FD past the FDArray): no read outside the font, no hang, the glyph draws as missing. Task 2's `ACorruptedCffIsRefusedOrDrawsNothing`, Task 3's `ARunawayCharStringStops`.
2. **A glyph FreeType draws but the engine refuses** (only `random`, whose seed FreeType takes from memory addresses): Unsupported, drawn as missing, logged; never a wrong outline. Task 3's `RandomIsUnsupported`.
3. **A book whose embedded CFF faces were refused before the update:** examined again (`BookFonts::kVersion` 6), its faces now used. Task 7's `ABookExaminedBeforeIsExaminedAgain`.
4. **An SD-mode CFF family whose file on the card changed:** the block is planned again and the store installed over, as for TrueType. Task 6's `AChangedOtfFileIsInstalledOver`.
5. **A folder holding `.ttf` and `.otf` files of one family** (a TrueType regular and a CFF bold): one family, every style drawn by its own outlines. Task 5's `AFolderOfTtfAndOtfFilesIsOneFamily`.

---

### Task 1: The oracle draws CFF, and the test fonts

**Files:**
- Modify: `test/ttf_rasterizer/CMakeLists.txt` (FreeType with its CFF driver; the fixture directory define)
- Create: `lib/EpdFont/scripts/make_cff_fixtures.py`
- Create: `test/fixtures/fonts/` (the fonts, `README.md`, `OFL.txt`)
- Modify: `test/ttf_rasterizer/TtfTestUtil.h`
- Create: `test/ttf_rasterizer/CffFixtureTest.cpp`

**Interfaces:**
- Produces: `TTF_FIXTURE_DIR` (absolute path of `test/fixtures/fonts`, a compile definition of the `ttf_rasterizer` and `ttf_font` test targets); in `TtfTestUtil.h`, `std::vector<uint8_t> readFixture(const std::string& name)` and `std::vector<std::string> cffTestFonts()` (paths readable by `readFont`/`readFixture`, see Step 3).
- The fonts, all from the script, sorted:
  - `NotoSansJP-Cff.otf`, `NotoSerifJP-Cff.otf`: CID-keyed subsets of Noto Sans/Serif CJK JP (sources pinned as in `sd-fonts.yaml` of the CJK fonts plan: noto-cjk Sans2.004 `Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf`, Serif2.003 `Serif/OTF/Japanese/NotoSerifCJKjp-Regular.otf`, each by sha256), subroutines kept;
  - `NotoSansJP-CffFlat.otf`, `NotoSerifJP-CffFlat.otf`: the same with subroutines removed (`cff.desubroutinize()`);
  - `NotoSansJP-CffSlanted.otf`: `NotoSansJP-CffFlat` with the top DICT's FontMatrix `[0.001 0 0.0002 0.001 0 0]` (a non-identity matrix);
  - `CffOperators.otf`: a name-keyed font the script builds with fontTools' `FontBuilder`, one glyph per operator family: every line and curve operator with odd and even argument counts, `flex`, `hflex`, `hflex1`, `flex1` (both branches of its dx/dy rule), `hintmask`/`cntrmask` with 8 and 9 stems, a local and a global subroutine call, nested subroutines 15 deep, `add sub mul div neg abs sqrt dup exch index roll put get ifelse and or not eq drop`, a fractional coordinate (255-prefixed), a zero-length line, a single-point contour, and a seac glyph (`endchar` with adx ady bchar achar over StandardEncoding `A` and `grave`). Its own work; no third-party data.
- The character set of the Noto subsets: ASCII U+0020-007E, `、。「」・ー`, hiragana U+3041-3093, katakana U+30A1-30F3, and the kanji `日本語文字書読鬱慶議驚龍鑑響漢点線曲円角` (the script holds the list); about 200 glyphs each.

- [ ] **Step 1: Write the failing test** `test/ttf_rasterizer/CffFixtureTest.cpp`:

```cpp
TEST(CffFixtures, FreeTypeLoadsEveryCffTestFont) {
  for (const std::string& name : cffTestFonts()) {
    const std::vector<uint8_t> font = name.find("OpenDyslexic") == 0 ? readFont(name) : readFixture(name);
    FT_Face face = nullptr;
    ASSERT_EQ(0, FT_New_Memory_Face(library(), font.data(), static_cast<FT_Long>(font.size()), 0, &face)) << name;
    EXPECT_GT(face->num_glyphs, 1) << name;
    const bool cid = name.rfind("Noto", 0) == 0;
    FT_Bool isCid = 0;
    FT_Get_CID_Is_Internally_CID_Keyed(face, &isCid);
    EXPECT_EQ(cid, isCid != 0) << name;
    FT_Done_Face(face);
  }
}
```

(`library()`: the FT_Library the oracle already creates in `FreeTypeReference.cpp`; expose it if it is file-local. `cffTestFonts()` lists the six fixtures and `OpenDyslexic/OpenDyslexic-{Regular,Bold,Italic,BoldItalic}.otf`.)

- [ ] **Step 2: Run it to see it fail.**
Run: `cmake --build build/test --target TtfRasterizerTest -j 8 && ./build/test/ttf_rasterizer/TtfRasterizerTest.exe --gtest_filter='CffFixtures.*'`
Expected: FAIL — the fixtures do not exist, and `FT_New_Memory_Face` refuses CFF (the driver is compiled out).

- [ ] **Step 3: Build the fixtures and enable the driver.**
  - `make_cff_fixtures.py --source-dir <dir with the two Noto OTFs> --out test/fixtures/fonts`: checks each source's sha256, subsets with `fontTools.subset` (`--no-hinting`, layout features `kern` only, `--recalc-timestamp=false`, CID structure kept), writes the fonts above reproducibly (same bytes on every run), and writes nothing else. `README.md` says what each font is for, the sources and pins, the command, and the licences; `OFL.txt` is the SIL OFL 1.1 with the Noto CJK copyright line (Copyright 2014-2021 Adobe, Reserved Font Name 'Source').
  - `test/ttf_rasterizer/CMakeLists.txt`: `target_compile_definitions(freetype_host PUBLIC FREEINK_FONT_ENABLE_CFF=1)` (this registers psnames, psaux, pshinter and the cff driver, `config/ftmodule.h:11-27`), and `TTF_FIXTURE_DIR` for both test targets (`test/ttf_font/CMakeLists.txt` too).

- [ ] **Step 4: Run the test and the whole rasterizer suite.**
Run: `./build/test/ttf_rasterizer/TtfRasterizerTest.exe`
Expected: PASS, and every TrueType oracle test still passes unchanged (registering the CFF driver does not change how FreeType loads a TrueType face).

- [ ] **Step 5: Run the script twice and compare.** `python lib/EpdFont/scripts/make_cff_fixtures.py ... ; git status --short test/fixtures` → no change after the second run.

- [ ] **Step 6: Commit.** `git commit -m "test(ttf): CFF test fonts, and FreeType's CFF driver in the host oracle"`

### Task 2: The CFF table, and faces that hold one

**Files:**
- Create: `lib/EpdFont/ttf/TtfCff.h`, `lib/EpdFont/ttf/TtfCff.cpp`
- Modify: `lib/EpdFont/ttf/TtfFace.h`, `lib/EpdFont/ttf/TtfFace.cpp`
- Modify: `test/ttf_rasterizer/CMakeLists.txt`, `test/ttf_font/CMakeLists.txt` (the new source)
- Create: `test/ttf_rasterizer/TtfCffTest.cpp`; Modify: `test/ttf_rasterizer/TtfFaceTest.cpp`

**Interfaces:**
- Produces, in `namespace ttf`:
  - `struct CffIndex { uint32_t start; uint16_t count; uint8_t offSize; uint32_t dataStart; uint32_t end; }` — an INDEX at `start` (offsets into the FontBytes), empty when `count == 0`.
  - `bool readCffIndex(FontBytes& bytes, uint32_t at, uint32_t limit, CffIndex* out)` and `bool cffIndexItem(FontBytes& bytes, const CffIndex& index, uint32_t item, uint32_t* offset, uint32_t* length)` (out-of-range offsets clamped as `FT/cff/cffload.c:461-464, 556-566` do).
  - `struct CffMatrix { int32_t xx, xy, yx, yy; int32_t dx, dy; uint32_t upm; }` — 16.16, normalized as `FT/cff/cffobjs.c:725-741`; `bool identity() const`.
  - `struct CffFace { uint32_t table, tableLength; CffIndex charStrings, globalSubrs; CffIndex fdArray; uint32_t fdSelect; uint8_t fdSelectFormat; uint32_t privateDict, privateSize; CffIndex localSubrs; uint32_t charset; CffMatrix matrix; bool cid; }` — for a name-keyed font `fdArray.count == 0`, and `privateDict`/`localSubrs`/`charset` are the top DICT's; for a CID font they are found per glyph.
  - `struct CffGlyphSource { uint32_t offset, length; CffIndex localSubrs; CffMatrix matrix; uint32_t scaleNum, scaleDen; }` — a glyph's charstring, its font DICT's local subroutines, its matrix (the FD's concatenated with the top's for CID), and the scale ratio top upm / sub upm (`FT/cff/cffgload.c:446-452`; 1/1 when equal).
  - `Status cffGlyphSource(FontBytes& bytes, const Face& face, uint16_t glyph, CffGlyphSource* out)` — Malformed for a glyph past the CharStrings count or an FD past the FDArray (FreeType fails both, `FT/cff/cffgload.c:238-239`, `FT/psaux/cffdecode.c:2366-2402`).
  - `int32_t cffStandardCharGlyph(FontBytes& bytes, const Face& face, int32_t code)` — seac's component lookup, `FT/psaux/cffdecode.c:422-464`: -1 for a CID-keyed font or a code outside 0-255 or not in the charset.
- `Face` gains `enum class Outlines : uint8_t { TrueType, Cff }; Outlines outlines = Outlines::TrueType; CffFace cff;`. For a CFF face `numGlyphs` is the CharStrings count (as FreeType's `num_glyphs`), and `maxPoints`/`maxContours` stay 0 (Task 4 plans at the caps).
- `parseFace` accepts sfnt `OTTO` with a `CFF ` table (CFF major version 1, `hdrSize >= 4`, a Name INDEX of exactly one entry, `FT/cff/cffload.c:2237-2364`), requiring `CFF `, cmap, head, hhea, hmtx and maxp (version 0.5 or 1.0); `glyf`/`loca` are not read. A font with only `CFF2`, a collection, or a bare CFF stays Unsupported. FDSelect formats 0 and 3 only (else Malformed, `FT/cff/cffload.c:721-747`); at most 256 FDs. DICT numbers and reals as `FT/cff/cffparse.c` reads them (reals in BCD to 16.16 with FreeType's rounding; `defaultWidthX`/`nominalWidthX` truncated); the FontMatrix as `cff_parse_font_matrix`.

- [ ] **Step 1: Write the failing tests** in `TtfCffTest.cpp` (fixtures from Task 1; FreeType as the oracle where it can say):
  - `EveryCffTestFontParses`: `parseFace` Ok, `outlines == Outlines::Cff`, `numGlyphs` equal to FreeType's `num_glyphs`, `cff.cid` true for the Noto fixtures and false for OpenDyslexic and `CffOperators`, `unitsPerEm` equal to FreeType's.
  - `OpenDyslexicRegularIsNameKeyedWithSubroutines`: `cff.globalSubrs.count == 504`, `cff.localSubrs.count == 408`, `cff.fdArray.count == 0`.
  - `EveryGlyphHasACharStringAndAFontDict`: for every glyph of every fixture `cffGlyphSource` is Ok, `length > 0` for all but `.notdef`-like empty glyphs FreeType also draws empty.
  - `ASlantedMatrixIsKept`: `NotoSansJP-CffSlanted`'s glyph matrix has `xx == 0x10000`, `yy == 0x10000`, `xy != 0`; the others are identity.
  - `ACorruptedCffIsRefusedOrDrawsNothing` (Review Focus 1): for every byte position of `NotoSansJP-Cff.otf`'s `CFF ` table, a copy with that byte set to 0xFF, and a copy truncated there, `parseFace` returns without reading outside the buffer (FontBytes over exactly the copy), and when Ok, `cffGlyphSource` for every glyph returns Ok or Malformed without crashing. (Sample every 7th byte if the full sweep takes over 10 s; say so in a comment.)
  - In `TtfFaceTest.cpp`: the tests that expected OTTO and `OpenDyslexic-Regular.otf` to be Unsupported now expect Ok; new: `ACff2OnlyFontIsUnsupported` (a `TtfBuilder` font with sfnt OTTO and a `CFF2` table, no `CFF `).

- [ ] **Step 2: Run them to see them fail.**
Run: `cmake --build build/test --target TtfRasterizerTest -j 8 && ./build/test/ttf_rasterizer/TtfRasterizerTest.exe --gtest_filter='TtfCff*:TtfFace*'`
Expected: compile errors for the missing names, then FAILs (OTTO is Unsupported).

- [ ] **Step 3: Implement `TtfCff` and the `parseFace` branch.** Offsets only, never pointers, as the rest of `Face`.

- [ ] **Step 4: Run the rasterizer suite.** Expected: PASS (TrueType tests unchanged).

- [ ] **Step 5: Commit.** `git commit -m "feat(ttf): read the CFF table of OpenType fonts"`

### Task 3: The Type 2 interpreter

**Files:**
- Create: `lib/EpdFont/ttf/TtfCffCharString.h`, `lib/EpdFont/ttf/TtfCffCharString.cpp`
- Modify: the two test CMakeLists (the new source)
- Test: `test/ttf_rasterizer/TtfCffCharStringTest.cpp`

**Interfaces:**
- Consumes: Task 2's `CffFace`, `CffIndex`, `CffGlyphSource`, `cffGlyphSource`, `cffStandardCharGlyph`.
- Produces:
  - `struct CffOutline { FT_Vector* points; char* tags; unsigned short* contours; uint16_t maxPoints, maxContours; uint16_t numPoints = 0, numContours = 0; }` — the rasterizer's scratch arrays; points in integer font units, tags `FT_CURVE_TAG_ON` or `FT_CURVE_TAG_CUBIC`.
  - `struct CffDecoder` — the interpreter's whole state (operand stack of 48 typed entries, 32 transient values, 16 subroutine return frames, stem count, hint-mask state, current point, open-path state); `constexpr uint32_t kCffDecoderBytes = sizeof(CffDecoder)`, rounded up to 8.
  - `Status runCharString(FontBytes& bytes, const Face& face, const CffGlyphSource& glyph, CffDecoder* decoder, CffOutline* out)` — Ok; Malformed for what FreeType fails (`Invalid_Glyph_Format`, stack errors, a bad subroutine index, a nested seac or one whose component the charset does not name, more than 96 stems); Unsupported for `random`, for more points or contours than `out` holds, and past `kCffMaxOperators` (65,536); Transient when `bytes.ioError()`.
  - `constexpr uint32_t kCffMaxOperators = 65536;`

Port `cf2_interpT2CharString` and the glyph-path rules of `FT/psaux/psintrp.c`, `pshints.c`, `psft.c`, `psobjs.c` (header of this plan), without hinting: a hint map is not built, but stems are counted and hint-mask bytes consumed exactly as FreeType does. Character space is 16.16; the point a segment emits is `FT_MulFix(X, 0x400) >> 10`.

- [ ] **Step 1: Write the failing tests.** A test helper encodes Type 2 numbers and operators into a charstring and builds a FontBytes holding it with a global and a local subroutine INDEX (`CffGlyphSource` filled by hand; a CID-free Face). Assertions are exact point lists:
  - `MovesAndLinesMakeAClosedContour`: `10 20 rmoveto 100 0 0 100 -100 0 rlineto endchar` → points (10,20),(110,20),(110,120),(10,120), all ON, one contour ending at 3 (the closing line back to the start adds no point).
  - `ZeroLengthLinesAreDropped`, `ASinglePointContourIsDropped`, `CurvesKeepBothControlPointsAsCubic` (`rrcurveto` → CUBIC, CUBIC, ON).
  - `HhAndVvCurvesWithAnOddCountDropTwoBottomArguments` (the `count &= ~2` rule, `psintrp.c:2757,2802,2851`).
  - `Flex1PutsTheLastDeltaOnTheLargerAxis` (both branches, strict `>`), `HflexEndsOnTheStartingY`.
  - `SubroutinesUseTheirBias`: a local INDEX of 1,239 and of 1,240 entries; `callsubr` with -107 and -1131 reaches entry 0 respectively.
  - `SubroutineNestingStopsAtSixteen`: 16 nested calls Ok, 17 Malformed.
  - `HintMasksConsumeTheirBytes`: 9 stems then `hintmask` reads 2 bytes; the following operator is read correctly.
  - `MoreThanNinetySixStemsFail`: Malformed.
  - `TheWidthIsReadAndIgnored`: a leading odd argument to `rmoveto`/`hstem`/`endchar` does not move the outline.
  - `RollRotatesTheBottomEntries`, `ArithmeticMatchesFreeType` (`div` = `FT_DivFix`, `mul` = `FT_MulFix`, `sqrt` = `FT_SqrtFixed`, each on a few values; the expected values computed with FreeType's own functions linked into the test).
  - `RandomIsUnsupported` (Review Focus 2).
  - `AStackOverflowFails` (49 operands: Malformed).
  - `ARunawayCharStringStops` (Review Focus 1): a chain of 15 local subroutines, each calling the next twice (2^15 calls of the last, within the nesting limit) → Unsupported after `kCffMaxOperators` operators (the test asserts the status; no timing).
  - `SeacDrawsTheAccentThenTheBase`: on `CffOperators.otf`, the seac glyph's points are the accent's (translated by adx, ady) followed by the base's, as FreeType orders them (compare with FreeType's outline points for that glyph, unscaled: `FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING)`).
  - `EveryGlyphOfTheOperatorFontMatchesFreeTypeUnscaled`: for every glyph of `CffOperators.otf`, the engine's points, tags and contour ends equal FreeType's `FT_LOAD_NO_SCALE` outline (this pins every operator's emission at once).

- [ ] **Step 2: Run them to see them fail.** Run: `./build/test/ttf_rasterizer/TtfRasterizerTest.exe --gtest_filter='TtfCffCharString*'` → compile errors, then FAIL.

- [ ] **Step 3: Implement the interpreter.** No recursion: subroutine calls push return frames. No heap.

- [ ] **Step 4: Run the rasterizer suite.** Expected: PASS.

- [ ] **Step 5: Commit.** `git commit -m "feat(ttf): the Type 2 charstring interpreter, as FreeType's cf2 runs unhinted"`

### Task 4: The rasterizer draws CFF glyphs

**Files:**
- Modify: `lib/EpdFont/ttf/TtfRasterizer.h`, `lib/EpdFont/ttf/TtfRasterizer.cpp`
- Create: `test/ttf_rasterizer/TtfVsFreeTypeCffTest.cpp` (or a second parameter set in `TtfVsFreeTypeTest.cpp`, sharing its checks)

**Interfaces:**
- Consumes: Tasks 2-3.
- Produces: `ScratchPlan::decoderBytes` (`kCffDecoderBytes` for a CFF face, 0 for TrueType; `mergePlans` takes the larger; `setPlan` places the region after the tags); `planScratch` plans a CFF face at the caps (`kMaxScratchPoints` points, `kMaxScratchContours` contours), its output region from head's box as for TrueType. `Rasterizer::load` serves both outline formats with the same signature.

`load` for a CFF face: `cffGlyphSource`, `runCharString` into the scratch arrays (font units), then as `FT/cff/cffgload.c:688-731`: the matrix when not identity (each point `FT_Vector_Transform`ed with `FT_MulFix`, `FT/base/ftoutln.c:696-713`), the integer offset, then `x = FT_MulFix(x, xs)`, `y = FT_MulFix(y, ys)` where `xs = ys = unitsToPixels(face, ppem)`, multiplied by `FT_MulDiv(xs, scaleNum, scaleDen)` when the ratio is not 1; coordinates past ±2^24 (26.6) are Unsupported as for TrueType; no left-side-bearing shift; the advance (`advance12_4`) and the box exactly as for TrueType.

- [ ] **Step 1: Write the failing oracle test,** parameterised over `cffTestFonts()`, with `TtfVsFreeTypeTest`'s checks: at 8, 14 and 40 pt, `numGlyphs` equal; a glyph FreeType cannot load is Malformed or Unsupported here, and the count of such glyphs is equal (only `CffOperators`' `random` glyph, if the script adds one, may differ: Unsupported here, loaded by FreeType; the test names it); a glyph over 255 px or the output region is Unsupported; the advance exactly `(linearHoriAdvance + 2048) >> 12`; the box exactly FreeType's; the bitmap byte-identical. Report per font and size the glyph count and the differing-pixel count; the test fails when any bitmap differs, unless the font is listed in a `kCffPixelTolerance` table with its measured rate (at most 3% of its pixels at that size), which stays empty unless exactness proves impossible. Plus `TrueTypeOutputIsUnchanged`: the Bookerly golden hash (as `TtfVsFreeTypeTest` checks it) with a CFF face planned into the same rasterizer first.

- [ ] **Step 2: Run it to see it fail.** Run: `./build/test/ttf_rasterizer/TtfRasterizerTest.exe --gtest_filter='*Cff*'` → FAIL (load reports the CFF face Malformed: it reads glyf).

- [ ] **Step 3: Implement** the plan and the CFF branch of `load`.

- [ ] **Step 4: Run the whole rasterizer suite.** Expected: PASS, every TrueType test unchanged, every CFF glyph exact (or a tolerance entry with its rate, ledgered as a ruling).

- [ ] **Step 5: Commit.** `git commit -m "feat(ttf): draw CFF glyphs exactly as FreeType does, unhinted"`

### Task 5: CFF families from the card, in flash mode

**Files:**
- Modify: `lib/EpdFont/ttf/TtfFamily.h`, `lib/EpdFont/ttf/TtfFamily.cpp` (`isOutlineFontFileName`, `forEachTable`, `probeFile`)
- Modify: `lib/EpdFont/SdCardFontRegistry.cpp` (and `.h` comments), every caller of `isTtfFileName`
- Test: `test/ttf_font/TtfFontTest.cpp`, `test/ttf_font/TtfRegistryTest.cpp` (both in the `TtfFontTest` target)

**Interfaces:**
- Produces: `bool isOutlineFontFileName(const char* name)` (`.ttf` or `.otf`, any case; `x.otf.tmp` does not count), replacing `isTtfFileName`. `forEachTable` accepts sfnt `OTTO`. `probeFile` requires, for an `OTTO` file, `CFF `, cmap, head, hhea, hmtx and maxp (a `CFF2`-only file probes as style -1); its `sdBytes` for a CFF file is `UINT32_MAX` until Task 6 (so such a family installs only in flash mode).

- [ ] **Step 1: Write the failing tests:**
  - `AnOtfFamilyDrawsInFlashMode` (TtfFontTest): a set of `OpenDyslexic-Regular.otf` through `MemoryFile`; `glyph(kRegular, 'A')` returns a stored glyph whose bitmap equals the rasterizer's render of the same glyph at that size; kerning of a pair OpenDyslexic's GPOS kerns is non-zero.
  - `ACjkCffFamilyDrawsInFlashMode`: `NotoSansJP-CffFlat.otf`, a kanji and a kana drawn and stored.
  - `AFolderOfTtfAndOtfFilesIsOneFamily` (Review Focus 5, registry test): a folder with a TrueType regular (a bundled TTF) and `OpenDyslexic-Bold.otf` named as its bold is one family with both styles.
  - `AnOtfFileIsListedAndACff2FileIsNot`: discovery lists a folder of `.otf` files; a file with only a `CFF2` table probes -1 and its family is not listed.

- [ ] **Step 2: Run them to see them fail.** Run: `cmake --build build/test --target TtfFontTest -j 8 && ./build/test/ttf_font/TtfFontTest.exe --gtest_filter='*Otf*:*Cff*:*TtfAndOtf*'` → FAIL.

- [ ] **Step 3: Implement.**

- [ ] **Step 4: Run** `ctest -j 8 -R "Ttf|Glyph|Font"`. Expected: PASS.

- [ ] **Step 5: Commit.** `git commit -m "feat(fonts): .otf families on the card draw through the TrueType engine"`

### Task 6: CFF families in SD mode

**Files:**
- Modify: `lib/EpdFont/ttf/TtfFamily.h`, `lib/EpdFont/ttf/TtfFamily.cpp` (`SdBlock` for a CFF file; `probeFile`'s `sdBytes`)
- Possibly create: `lib/EpdFont/ttf/TtfCffBlock.h/.cpp` (the rebuilt `CFF ` table's layout and bytes), if it keeps `TtfFamily.cpp` focused
- Test: `test/ttf_font/TtfFontTest.cpp`, `test/ttf_rasterizer/TtfCffTest.cpp`

**Interfaces:**
- Consumes: Task 2's parser (over the file, through a FontBytes on the FileSource), Task 5.
- Produces: `SdBlock::plan` for an `OTTO` file. The block holds the table directory, the tables the engine reads but `CFF ` (cmap, head, hhea, hmtx, maxp, OS/2, GPOS, GSUB, kern), and last a rebuilt `CFF ` table that ends exactly at the block's end, with only the CharStrings INDEX's data left in the file: `outlines()` gives that data's file offset, `fontOffset == bytes()` and its length. The rebuilt table, in order: header, Name INDEX, Top DICT INDEX, String INDEX, Global Subr INDEX, charset, Encoding (when the top DICT has an offset), FDSelect, FDArray INDEX, each Private DICT followed by its local Subrs INDEX (the Subrs offset is relative to its Private DICT, so it holds), and last the CharStrings INDEX's count, offset size and offset array. Every offset operand that moves (Top DICT: charset, Encoding, CharStrings, Private, FDArray, FDSelect; each FD's Private) is written as a 5-byte integer (operator 29), so each DICT's size is known in one pass. The table is always rebuilt this way, already in order or not (one code path; my call). `sameAs` and `read` serve it as they serve the TrueType block; `probeFile`'s `sdBytes` is its size.

- [ ] **Step 1: Write the failing tests:**
  - `TheRebuiltCffDrawsAsTheOriginal` (TtfCffTest): for each Noto fixture and OpenDyslexic, the block's bytes (`SdBlock::source()`) plus an `SdRange` onto the original file make a FontBytes whose `parseFace` is Ok and whose every glyph, rendered at 14 pt, is byte-identical to the flash-mode render of the original file.
  - `ACffFamilyDrawsInSdMode` (TtfFontTest, the existing SD-mode set-up forcing SD mode with 24 pages): `NotoSerifJP-CffFlat.otf` in SD mode; every fixture glyph equals the flash-mode bitmap; the card reads (`sdPageReads()`) for a batch of N new glyphs are at most N plus the cache's first page (one read per charstring).
  - `ASubroutinizedCffKeepsItsSubroutinesInTheBlock`: `NotoSerifJP-Cff.otf` in SD mode reads from the card only inside the charstring data range (a FileSource that fails any read outside it: the glyphs still draw).
  - `AChangedOtfFileIsInstalledOver` (Review Focus 4): the stamp of the SD-mode `.otf` changes between loads; the set installs again and draws.

- [ ] **Step 2: Run them to see them fail.** → FAIL (`SdBlock::plan` refuses an `OTTO` file).

- [ ] **Step 3: Implement** the rebuilt block and `sdBytes`.

- [ ] **Step 4: Run** `ctest -j 8 -R "Ttf|Glyph|Font"`. Expected: PASS.

- [ ] **Step 5: Commit.** `git commit -m "feat(ttf): CFF families in SD mode, the charstring data read from the card"`

### Task 7: Publisher Fonts takes CFF faces; the docs

**Files:**
- Modify: `lib/Epub/Epub/BookFonts.h`, `lib/Epub/Epub/BookFonts.cpp` (the signature check, `kVersion` 5 → 6, comments)
- Test: `test/epub_pipeline/BookFontsTest.cpp` (target `EpubPipelineTest`)
- Modify: `test/book_metadata_cache/AllocationRefusal.cpp`: it calls glibc's `malloc_usable_size`, so `EpubPipelineTest` (which compiles it) does not build on Windows; on MinGW use `_msize` from `<malloc.h>` (`#ifdef _WIN32`), so BookFonts' tests run locally. If the target still does not build on Windows for another reason, say so in the report; CI runs it.
- Modify: `docs/file-formats.md` (:63 `kVersion`, :388 CFF no longer refused, :507), `USER_GUIDE.md` (:375 Font Family; :398-404 Publisher Fonts: TrueType or OpenType), `README.md` (:178 credits: TrueType and CFF outlines), `docs/future_work/cjk.md` (CFF shipped)

**Interfaces:**
- Produces: `isTrueType` becomes `isOutlineFont` accepting `0x00010000`, `true` and `OTTO` (after the obfuscation key is undone, as now); WOFF, WOFF2 and collections stay refused. Copies keep their `<style>.ttf` names (discovery reads the content, Task 5; my call).

- [ ] **Step 1: Write the failing tests:** `AnEmbeddedCffFaceIsAccepted` (a face starting `OTTO`), `AnObfuscatedCffFaceIsAccepted` (IDPF key), `WoffAndCollectionsStayRefused`, `ABookExaminedBeforeIsExaminedAgain` (Review Focus 3: a record written with `kVersion` 5 is examined again).
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement, and update the docs.**
- [ ] **Step 4: Run** the host suite: `cmake --build build/test -j 8 -- -k 0` then `ctest -j 8` (all pass but the three Windows-only targets).
- [ ] **Step 5: Build the firmware** (`pio run -e default`): SUCCESS, `check_no_float: ... no floating point`; note the flash size against master.
- [ ] **Step 6: Format and commit.** `git commit -m "feat(epub): Publisher Fonts uses a book's CFF fonts"`

### Task 8: On the X4 and the X3

**Files:** `bench/ttf_raster_main.cpp` (a CFF run); the figures go into the PR description.

- [ ] **Step 1: The bench.** Add a run over `NotoSansJP-CffFlat.otf`'s kanji and kana at 12 pt (µs a glyph, regular), next to the existing CJK TrueType figure; `pio run -e bench_ttf_raster -t upload`; record both. The TrueType hash stays as it is.
- [ ] **Step 2: Flash** the X4 (`pio run -e default -t upload --upload-port <port>`; the X3 is flashed by the maintainer).
- [ ] **Step 3: Fonts on the card** (`CMD:FS:LS`, `tools/serial_transfer.py` with the USB Transfer screen): `OpenDyslexic-Regular.otf` into `/.crosspoint/fonts/OpenDyslexicOtf/`; the desubroutinized `NotoSansTC-Regular.otf` (3.45 MB, SD mode) into `/.crosspoint/fonts/NotoSansTCOtf/`.
- [ ] **Step 4: Read.** With `tools/devctl.py` (GET each setting first): OpenDyslexicOtf on a Latin test book; NotoSansTCOtf on `test_cjk_zh_honglou.epub` and the store-fill book at 12 and 26 pt; `/test/bookfont_cff.epub` with Publisher Fonts on. Screenshots (`shot:`) reviewed by Claude for correct, complete glyphs; `CMD:TTF` for SD mode and store use; `CMD:STATE` heap.
Expected: every page draws its characters; no `Unsupported`/`Malformed` log lines for the hosted font; heap as with the TrueType font.
- [ ] **Step 5: Restore** every changed setting; record per board: µs a glyph CFF vs TrueType, layout and glyph-preparation times on the honglou book (as the spec's Appendix A), heap.
