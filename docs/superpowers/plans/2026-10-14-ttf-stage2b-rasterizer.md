# TTF Stage 2b: the rasterizer. Implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the TTF engine's rasterizer: a bounds-checked font reader (`FontBytes`), a face parser
(tables, cmap, metrics), an integer TrueType loader and FreeType's own anti-aliasing rasterizer, vendored and
patched. It turns a glyph of a `.ttf` into the same 2-bit bitmap and metrics the PC converter would have
produced. Nothing in the firmware calls it yet; Stage 2c wires it to `TtfFont` and the glyph store.

**Architecture:**

- **`FontBytes`** (`lib/EpdFont/ttf/FontBytes.h`) is the engine's only way to read a font: big-endian reads,
  each one bounds-checked, with separate "out of range" (broken font) and "I/O error" (transient) flags.
  This stage provides the flash mode (a pointer and a length); Stage 2c adds the SD mode.
- **`ttf::Face`** (`TtfFace.h/.cpp`) holds a font's table offsets and metrics. It reads the cmap (formats 4
  and 12) and `hmtx`, and computes sizes and line metrics exactly as FreeType does for the converter.
- **`ttf::Rasterizer`** (`TtfRasterizer.h/.cpp`) owns the family's scratch block. `load()` reads a glyph
  outline (simple glyphs, compound glyphs with offsets and 2.14 transforms) and scales it in integers, as
  FreeType's TrueType loader does unhinted, then reports the glyph's `EpdGlyph` metrics. `rasterize()` runs
  FreeType's `ftgrays` over it and packs the coverage spans straight into the 2-bit stream.
- **`third_party/freetype/`** holds `ftgrays.c` from FreeType 2.13.3, built standalone and patched (pool from
  the caller, FreeType's 64-bit paths, a renamed raster). `scripts/vendor_ftgrays.py` regenerates it.
- **Host tests** build FreeType itself (the SDK's copy) and require every glyph of every bundled TTF to match
  it.

**Tech Stack:** C++20 host tests (gtest via CMake, UCRT64 gcc; ASan and UBSan where they link); firmware
gnu++2a (PlatformIO, ESP32-C3/S3); FreeType 2.13.3 (`ftgrays.c` standalone in the firmware, the full library
in host tests only); Python 3 for the vendoring and build-check scripts; clang-format 21.1.8.

**Spec:** `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md` (branch `docs/ttf-fonts-design`): sections 2,
5.1-5.3, 5.6, 6.1, 6.3, 9.3, 10 and 11. Task 9 updates it with this plan's decisions.

## Global Constraints

- **Branch:** `feat/ttf-rasterizer`, created from `master` (which has Stages 1 and 2a, PRs #468 and #469).
- **No behaviour change:** nothing in the firmware calls the engine. `default` and `x4pro` must build, and
  their flash size may change by at most a few hundred bytes (the linker drops unreferenced code).
- **No floating point** anywhere in `lib/EpdFont/ttf`, the vendored rasterizer included (spec 5.2). Task 7's
  build check enforces it.
- **FreeType's numbers, bit for bit:** scales are `FT_DivFix(ppem26_6, unitsPerEm)`, points are scaled with
  `FT_MulFix` as they are read, advances are `(FT_MulDiv(advance, scale, 64) + 2048) >> 12`, and the line
  metrics are FreeType's size metrics (height rounded, ascender up, descender down). Pixels per em for a
  point size: `(pt * 64 * 150 + 36) / 72` in 26.6 (14 pt -> 1867).
- **2-bit packing:** the coverage nibble maps `>= 12 -> 3`, `>= 8 -> 2`, `>= 4 -> 1`, else 0; pixels
  MSB-first, rows continuous, as `.cpfont` bitmaps.
- **Scratch caps (spec 5.3):** 512 points, 256 contours, a 4,096 B output region; the cell pool is 4 KB in
  flash mode and 8 KB in SD mode, never less than 2 KB.
- **Heap:** one scratch block per family, from `makeUniqueNoThrow`, null-checked with `LOG_ERR`. No
  allocation on any per-glyph path.
- **Glyph failures (spec 6.1):** `Unsupported` and `Malformed` are permanent; `Transient` is not.
- **Concurrency:** `ftgrays`' standalone raster object is a static, so renders must not overlap. The engine
  renders only under `RenderLock` (spec 3.4).
- **Style:** clang-format 21 with the repo's `.clang-format` (120 columns); `///` doc comments; engine code
  in `namespace ttf`. The vendored FreeType files are never reformatted.
- **Commits** follow the repo's style (`feat(ttf): ...`, `test(ttf): ...`); the PR title needs a semantic
  prefix (`feat: ...`).

## Decisions this plan takes

The prototype on `bench/ttf-step0` was rebuilt and measured against FreeType while this plan was written.
These decisions refine the spec; Task 9 writes them into it.

1. **`FontBytes` comes in 2b, flash mode only.** The spec lists it under 2c, but the rasterizer's interface
   is built on it. The fast path (a pointer and a length) is inline; 2c adds the SD page cache behind the
   same calls.
2. **The rasterizer owns the scratch block.** The spec sketches `render(FontBytes&, glyph, ppem, Scratch&)`.
   Here `ttf::Rasterizer` holds the block (one per family) and the outline it last loaded.
3. **Two steps, `load()` then `rasterize(dst)`.** `GlyphBatch::add()` needs a glyph's metrics before it hands
   out the place for its bitmap (spec 8.1, "render straight into `add()`'s pointer"). `render()` combines
   both into the scratch block's output region.
4. **The vendored `ftgrays` defines FreeType's 64-bit types (`FT_INT64`).** Without them it takes an older
   line renderer and another conic-flattening algorithm than a full FreeType build. That was the step 0
   prototype's state, and 28-61 glyphs per Bookerly or Ubuntu face differed from FreeType by one pixel.
   With them, every glyph without overlap flags is byte-identical to FreeType at 8, 14 and 40 pt, and the
   output no longer depends on the width of `long`, so host and device agree. On the C3 it adds `__divdi3`
   (at most two per line segment) and `__ashldi3`: integer, but not free. Measured on the X3 with the bench
   of Task 8 while writing this plan:
   - all 1,867 Bookerly glyphs at 14 pt (29 px), 4 KB pool: 460 µs per glyph on average. Step 0 measured
     429 µs on a sample of text glyphs.
   - The output hash equals the host's.
5. **FreeType's exact scaling and phantom point.** Points are scaled with `FT_MulFix` by `FT_DivFix`'s
   scale, not by `v * ppem / upem`. Each outline is shifted by `xMin - lsb` (from the top glyph, or from a
   component flagged `USE_MY_METRICS`), as FreeType places glyphs. Together these remove the prototype's
   5 box differences per font: there are 0.
6. **A stronger regression gate than spec 9.3.** It keeps the spec's limits: no load failures, at most 5 box
   differences per font, at most 3% of pixels different. It adds:
   - glyphs without overlap flags must be byte-identical to FreeType;
   - advances must be identical;
   - the cmap must be identical up to U+2FFFF;
   - line metrics must equal the converter's at 8-40 pt.

   FreeType renders glyphs flagged `OVERLAP_SIMPLE` or `OVERLAP_COMPOUND` 4x4 oversampled (deferred, spec
   11.6). Those stay under the 3% limit: measured 0.24-1.30%.
7. **The no-float check reads the engine's objects, not `firmware.map`.** The map names only the first object
   that pulled in each archive member, and SdFat and the battery monitor already pull in the double
   routines, so a TTF use would be invisible there. `scripts/check_no_float.py` runs `nm -u` on every
   object under `EpdFont/ttf/` after each link.
8. **`Transient` instead of `IoError`** in the status enum. It also covers "no scratch block yet", which must
   never mark a glyph missing for good.
9. **Apple's scaled component offsets** (`SCALED_COMPONENT_OFFSET` with a transform) report `Unsupported`,
   like point matching. No bundled font uses either; added to the deferred list.
10. **Sizes stop at 255 px per em** (`kMaxPpem26_6`). A larger glyph could not fit `EpdGlyph`'s 8-bit width
    and height anyway.
11. **Sanitizers per spec 9.3:** the engine's test target builds with ASan and UBSan where they link (CI's
    Linux GCC), and with UBSan in trap mode where ASan cannot link (MinGW). FreeType itself is not
    instrumented.

## Review Focus

The input classes the spec implies that are most likely to bite a reader, each pinned by a test:

1. **Sizes far from 14 pt** (8 pt notes, 40 pt headings) must match FreeType as well as the body size does:
   Task 5 compares every glyph at 8, 14 and 40 pt.
2. **Glyphs no codepoint reaches** (components, alternates) must load as safely as mapped ones: Task 5 walks
   every glyph index, not the cmap.
3. **A glyph larger than an `EpdGlyph` can hold** (Ubuntu's 3.5 em wordmark at 40 pt) must report
   `Unsupported`, not wrap its width: Task 4 (`BitmapsTooLargeAreUnsupported`) and Task 5.
4. **A font whose tables lie** (wrong `maxp`, `loca` past `glyf`, a compound that contains itself or explodes
   into thousands of components, coordinates at the `int16` limits) must fail the glyph, never the
   device: Tasks 4 and 6.
5. **The device must draw what the host checked:** Task 8 compares a hash of every Bookerly glyph at 14 pt
   on the X3 with the host's.

## File map

| File | Task | Role |
| --- | --- | --- |
| `scripts/vendor_ftgrays.py` | 1 | Copies FreeType's rasterizer from the SDK and applies the patches |
| `lib/EpdFont/ttf/third_party/freetype/{ftgrays.c,ftgrays.h,ftimage.h,FTL.TXT}` | 1 | Generated by the script |
| `lib/EpdFont/ttf/third_party/freetype/PATCHES.md` | 1 | What was changed and why; licence note |
| `lib/EpdFont/ttf/TtfGrays.h` | 1 | Private include shim for the one `.cpp` that drives `ftgrays` |
| `lib/EpdFont/ttf/FontBytes.h` | 2 | Bounds-checked big-endian reads |
| `lib/EpdFont/ttf/TtfFixed.h` | 2 | `FT_MulFix`, `FT_DivFix`, `FT_MulDiv` |
| `lib/EpdFont/ttf/TtfFace.h/.cpp` | 3 | Table directory, cmap, `hmtx`, sizes, line metrics |
| `lib/EpdFont/ttf/TtfRasterizer.h/.cpp` | 4 | Scratch block, outline loader, span packer |
| `scripts/check_no_float.py` | 7 | Post-link check: no soft-float symbol in a TTF object |
| `bench/ttf_raster_main.cpp`, `bench/ttf_raster_golden.h` | 8 | Device bench and the shared output hash |
| `test/ttf_rasterizer/*` | 1-6, 8 | Host tests; `TtfBuilder.h` builds small fonts in memory |
| `test/CMakeLists.txt`, `bin/clang-format-fix`, `platformio.ini`, `docs/contributing/getting-started.md` | 1, 7, 8 | Wiring |

## Tools (Git Bash on the dev machine)

```bash
# host tests
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S test -B build/test
cmake --build build/test --target TtfRasterizerTest -j 8
./build/test/ttf_rasterizer/TtfRasterizerTest.exe            # add --gtest_filter='Suite*' to narrow
# firmware
export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8
"$HOME/.platformio/penv/Scripts/pio.exe" run -e default
# formatting (clang-format 21; the local default is 22)
/c/tmp/cf21/bin/clang-format-21.exe -i <files>
```

A new worktree needs the SDK submodule: `git submodule update --init freeink-sdk`. The FreeType comparison
compiles FreeType from it.

---

### Task 1: Vendor FreeType's rasterizer

The rasterizer comes from the SDK's FreeType 2.13.3 through a script, so a FreeType update is a re-run, not a
hand merge. Its patches move the cell pool off the stack, enable FreeType's 64-bit paths, and rename the
raster so a full FreeType can link next to it (the host tests do that in Task 5).

**Files:**
- Create: `scripts/vendor_ftgrays.py`
- Create (generated by the script): `lib/EpdFont/ttf/third_party/freetype/ftgrays.c`, `ftgrays.h`, `ftimage.h`, `FTL.TXT`
- Create: `lib/EpdFont/ttf/third_party/freetype/PATCHES.md`
- Create: `lib/EpdFont/ttf/TtfGrays.h`
- Create: `test/ttf_rasterizer/CMakeLists.txt`, `test/ttf_rasterizer/TtfGraysTest.cpp`
- Modify: `test/CMakeLists.txt` (one line), `bin/clang-format-fix` (two lines)

**Interfaces:**
- Produces:
  - `extern const FT_Raster_Funcs ttf_grays_raster` (from `TtfGrays.h`; include it only from `.cpp` files).
    `raster_new(nullptr, &raster)` hands out the static raster object.
    `raster_reset(raster, pool, bytes)` sets the cell pool; it must be at least 2,048 B and aligned for a
    pointer.
    `raster_render(raster, &params)` returns 0 on success, non-zero without a usable pool.
  - Test target `TtfRasterizerTest` (`test/ttf_rasterizer/CMakeLists.txt`), which later tasks extend.

- [ ] **Step 1: Branch**

```bash
git checkout master && git pull --ff-only
git checkout -b feat/ttf-rasterizer
git submodule update --init freeink-sdk
```

- [ ] **Step 2: Write the failing test**

Create `test/ttf_rasterizer/TtfGraysTest.cpp`:

<!-- plan-file: test/ttf_rasterizer/TtfGraysTest.cpp -->
```cpp
// The vendored FreeType rasterizer (lib/EpdFont/ttf/third_party/freetype): it renders in the caller's cell
// pool, never on the stack, and the pool's size changes nothing but the number of bands.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

#include "TtfGrays.h"

namespace {

// An outline of straight-edged contours in 26.6, every point on the curve.
struct Polygon {
  std::vector<FT_Vector> points;
  std::vector<unsigned char> tags;
  std::vector<unsigned short> contours;

  void add(const std::initializer_list<std::pair<long, long>> corners) {
    for (const auto& [x, y] : corners) {
      points.push_back({x, y});
      tags.push_back(FT_CURVE_TAG_ON);
    }
    contours.push_back(static_cast<unsigned short>(points.size() - 1));
  }
  FT_Outline outline() {
    FT_Outline o{};
    o.n_points = static_cast<unsigned short>(points.size());
    o.n_contours = static_cast<unsigned short>(contours.size());
    o.points = points.data();
    o.tags = tags.data();
    o.contours = contours.data();
    return o;
  }
};

// Coverage per pixel, top row first, from the spans ftgrays emits (it counts rows from the bottom).
struct Image {
  int width;
  int height;
  std::vector<int> coverage;
};

void accumulate(const int y, const int count, const FT_Span* spans, void* user) {
  auto* image = static_cast<Image*>(user);
  for (int i = 0; i < count; ++i) {
    for (int x = spans[i].x; x < spans[i].x + spans[i].len; ++x) {
      image->coverage[(image->height - 1 - y) * image->width + x] += spans[i].coverage;
    }
  }
}

int render(Polygon& polygon, Image* image, unsigned char* pool, const unsigned long poolBytes) {
  image->coverage.assign(static_cast<size_t>(image->width * image->height), 0);
  FT_Outline outline = polygon.outline();
  FT_Raster_Params params{};
  params.source = &outline;
  params.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
  params.gray_spans = accumulate;
  params.user = image;
  params.clip_box.xMax = image->width;
  params.clip_box.yMax = image->height;
  FT_Raster raster = nullptr;
  ttf_grays_raster.raster_new(nullptr, &raster);
  ttf_grays_raster.raster_reset(raster, pool, poolBytes);
  return ttf_grays_raster.raster_render(raster, &params);
}

// A diamond 60 px across with a diamond hole: enough edge cells to need several bands in a 2 KB pool.
Polygon diamond() {
  Polygon polygon;
  polygon.add({{32 * 64, 2 * 64}, {62 * 64, 32 * 64}, {32 * 64, 62 * 64}, {2 * 64, 32 * 64}});
  polygon.add({{32 * 64, 22 * 64}, {22 * 64, 32 * 64}, {32 * 64, 42 * 64}, {42 * 64, 32 * 64}});  // reversed
  return polygon;
}

}  // namespace

TEST(TtfGrays, RendersInTheCallersPool) {
  Polygon square;
  square.add({{0, 0}, {256, 0}, {256, 256}, {0, 256}});  // 4 x 4 px
  alignas(8) unsigned char pool[4096];
  std::memset(pool, 0xA5, sizeof pool);
  Image image{4, 4, {}};
  ASSERT_EQ(0, render(square, &image, pool, sizeof pool));
  for (const int c : image.coverage) EXPECT_EQ(255, c);
  bool used = false;
  for (const unsigned char byte : pool) used |= byte != 0xA5;
  EXPECT_TRUE(used);
}

TEST(TtfGrays, WithoutAPoolNothingIsRendered) {
  Polygon square;
  square.add({{0, 0}, {256, 0}, {256, 256}, {0, 256}});
  Image image{4, 4, {}};
  EXPECT_NE(0, render(square, &image, nullptr, 0));
  alignas(8) unsigned char small[1024];
  EXPECT_NE(0, render(square, &image, small, sizeof small));  // under the 2 KB minimum
  for (const int c : image.coverage) EXPECT_EQ(0, c);
}

TEST(TtfGrays, ThePoolSizeChangesOnlyTheBands) {
  Polygon shape = diamond();
  alignas(8) static unsigned char pool[16384];
  Image small{64, 64, {}};
  Image large{64, 64, {}};
  ASSERT_EQ(0, render(shape, &small, pool, 2048));
  ASSERT_EQ(0, render(shape, &large, pool, sizeof pool));
  EXPECT_EQ(large.coverage, small.coverage);
  int inked = 0;
  for (const int c : large.coverage) inked += c > 0;
  EXPECT_GT(inked, 1500);  // 2 * 30^2 less the hole's 2 * 10^2, plus the edges
}
```

Create `test/ttf_rasterizer/CMakeLists.txt`:

<!-- plan-file: test/ttf_rasterizer/CMakeLists.txt -->
```cmake
# The TTF engine (lib/EpdFont/ttf): FontBytes, face parsing, the integer TrueType loader and the vendored
# ftgrays, checked against FreeType itself over every bundled TTF.

add_executable(TtfRasterizerTest
  TtfGraysTest.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/third_party/freetype/ftgrays.c
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(TtfRasterizerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/shims
)

target_link_libraries(TtfRasterizerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

# Spec 9.3: the engine runs under AddressSanitizer and UBSan (CI's Linux GCC). Where ASan cannot link
# (MinGW), UBSan still traps on undefined behaviour without its runtime. FreeType is not instrumented.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address,undefined)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_ASAN)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=undefined -fsanitize-undefined-trap-on-error")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_UBSAN_TRAP)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(TTF_TESTS_HAVE_ASAN)
  set(TTF_SANITIZERS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
elseif(TTF_TESTS_HAVE_UBSAN_TRAP)
  set(TTF_SANITIZERS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
endif()
target_compile_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
target_link_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
message(STATUS "TtfRasterizerTest sanitizers: ${TTF_SANITIZERS}")

gtest_discover_tests(TtfRasterizerTest DISCOVERY_TIMEOUT 60)
```

In `test/CMakeLists.txt`, add the new directory after the glyph store's:

<!-- plan-edit: test/CMakeLists.txt -->
```cmake
add_subdirectory(glyph_store)
```
with:

```cmake
add_subdirectory(glyph_store)
add_subdirectory(ttf_rasterizer)
```

- [ ] **Step 3: Run it to see it fail**

Run: `cmake -S test -B build/test`
Expected: `CMake Error ... Cannot find source file: .../lib/EpdFont/ttf/third_party/freetype/ftgrays.c`

- [ ] **Step 4: Write the vendoring script and run it**

Create `scripts/vendor_ftgrays.py`:

<!-- plan-file: scripts/vendor_ftgrays.py -->
```python
"""Vendor FreeType's smooth rasterizer (ftgrays.c) for the TTF engine, standalone and patched.

Copies src/smooth/ftgrays.c, src/smooth/ftgrays.h, include/freetype/ftimage.h and FTL.TXT from the
SDK's FreeType (freeink-sdk/libs/font/FreeInkFont/third_party/freetype, FreeType 2.13.3) into
lib/EpdFont/ttf/third_party/freetype/, with LF line endings, and applies the patches listed in
PATCHES.md there. Every patched spot is marked PATCH(witchhunt).

Each patch replaces text that must occur exactly once in the source, so a FreeType update that moves
the code fails here instead of producing a silently different rasterizer. Run from the repository
root:

    python scripts/vendor_ftgrays.py
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "freeink-sdk/libs/font/FreeInkFont/third_party/freetype"
DST = ROOT / "lib/EpdFont/ttf/third_party/freetype"

HEADER_NOTE = """\

/* PATCH(witchhunt): FreeType 2.13.3 src/smooth/ftgrays.c, built standalone for the TTF engine in */
/* lib/EpdFont/ttf.  Regenerate with scripts/vendor_ftgrays.py; the patches are listed in        */
/* PATCHES.md next to this file.                                                                 */
#define STANDALONE_
"""

FALL_THROUGH = """\
#define ft_jmp_buf  jmp_buf

  /* PATCH(witchhunt): FreeType's compiler-macros.h defines this; a standalone build has none. */
#ifndef FALL_THROUGH
#if ( defined( __GNUC__ ) && __GNUC__ >= 7 ) || defined( __clang__ )
#define FALL_THROUGH  __attribute__(( fallthrough ))
#else
#define FALL_THROUGH  ( (void)0 )
#endif
#endif
"""

POOL_CAP_OLD = """\
  /* FreeInkFont: the pool below lives on the STACK of the rendering task
   * (`gray_convert_glyph`), so it must fit an embedded FreeRTOS task stack.
   * The cap lives here rather than only in ftoption.h because build systems
   * cannot scan FreeType's `#include FT_CONFIG_OPTIONS_H` macro indirection,
   * so ftoption.h edits do not invalidate compiled objects.  A smaller pool
   * only makes the rasterizer subdivide into more bands; output is identical.
   */
#undef  FT_RENDER_POOL_SIZE
#define FT_RENDER_POOL_SIZE  4096L

  /* maximum number of gray cells in the buffer */
#if FT_RENDER_POOL_SIZE > 2048
#define FT_MAX_GRAY_POOL  ( FT_RENDER_POOL_SIZE / sizeof ( TCell ) )
#else
#define FT_MAX_GRAY_POOL  ( 2048 / sizeof ( TCell ) )
#endif
"""

POOL_CAP_NEW = """\
  /* PATCH(witchhunt): no FT_MAX_GRAY_POOL; the cell pool is the caller's (gray_raster_reset). */
  /* A pool must hold at least this many bytes, the smallest the upstream code allows.         */
#define GRAY_MIN_POOL_BYTES  2048
"""

RASTER_OLD = """\
  typedef struct gray_TRaster_
  {
    void*  memory;

  } gray_TRaster, *gray_PRaster;
"""

RASTER_NEW = """\
  typedef struct gray_TRaster_
  {
    void*  memory;
    void*  pool;       /* PATCH(witchhunt): the cell pool given to raster_reset */
    size_t pool_size;  /* PATCH(witchhunt): its size in bytes                   */

  } gray_TRaster, *gray_PRaster;
"""

CONVERT_OLD = """\
  static int
  gray_convert_glyph( RAS_ARG )
  {
    TCell    buffer[FT_MAX_GRAY_POOL];
    size_t   height = (size_t)( ras.cbox.yMax - ras.cbox.yMin );
    size_t   n = FT_MAX_GRAY_POOL / 8;
    TCoord   y;
    TCoord   bands[32];  /* enough to accommodate bisections */
    TCoord*  band;

    int  continued = 0;


    /* Initialize the null cell at the end of the poll. */
    ras.cell_null        = buffer + FT_MAX_GRAY_POOL - 1;
"""

CONVERT_NEW = """\
  static int
  gray_convert_glyph( RAS_ARG_ gray_PRaster  raster )
  {
    /* PATCH(witchhunt): the cells live in the caller's pool, not in a stack array */
    TCell*   buffer = (TCell*)raster->pool;
    size_t   cells  = raster->pool_size / sizeof ( TCell );
    size_t   height = (size_t)( ras.cbox.yMax - ras.cbox.yMin );
    size_t   n = cells / 8;
    TCoord   y;
    TCoord   bands[32];  /* enough to accommodate bisections */
    TCoord*  band;

    int  continued = 0;


    if ( !buffer || raster->pool_size < GRAY_MIN_POOL_BYTES )
      return FT_THROW( Invalid_Argument );

    /* Initialize the null cell at the end of the poll. */
    ras.cell_null        = buffer + cells - 1;
"""

CALL_OLD = """\
    return gray_convert_glyph( RAS_VAR );
"""

CALL_NEW = """\
    return gray_convert_glyph( RAS_VAR_ (gray_PRaster)raster );
"""

RESET_OLD = """\
  static void
  gray_raster_reset( FT_Raster       raster,
                     unsigned char*  pool_base,
                     unsigned long   pool_size )
  {
    FT_UNUSED( raster );
    FT_UNUSED( pool_base );
    FT_UNUSED( pool_size );
  }
"""

RESET_NEW = """\
  static void
  gray_raster_reset( FT_Raster       raster,
                     unsigned char*  pool_base,
                     unsigned long   pool_size )
  {
    /* PATCH(witchhunt): keep the caller's cell pool; it must be aligned for a TCell */
    gray_PRaster  rast = (gray_PRaster)raster;


    if ( rast )
    {
      rast->pool      = pool_base;
      rast->pool_size = pool_base ? (size_t)pool_size : 0;
    }
  }
"""

INT64_OLD = "#define FT_ULONG_MAX  ULONG_MAX\n"
INT64_NEW = """\
#define FT_ULONG_MAX  ULONG_MAX

  /* PATCH(witchhunt): the 64-bit types a full FreeType build defines, so this build takes the same */
  /* code paths (64-bit line divisions, DDA conics) and gives FreeType's output on every platform.  */
#define FT_INT64   long long int
#define FT_UINT64  unsigned long long int
  typedef FT_INT64   FT_Int64;
  typedef FT_UINT64  FT_UInt64;
  typedef unsigned int  FT_UInt;
"""

TAGS_OLD ="    char*       tags;\n"
TAGS_NEW = "    unsigned char*  tags;  /* PATCH(witchhunt): ftimage.h's type (was char*) */\n"

RENAME_OLD = "ft_grays_raster"
RENAME_NEW = "ttf_grays_raster"


def read_lf(path: pathlib.Path) -> str:
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def replace_once(text: str, old: str, new: str, what: str) -> str:
    count = text.count(old)
    if count != 1:
        sys.exit(f"vendor_ftgrays: {what}: expected the anchor once, found it {count} times")
    return text.replace(old, new)


def patch_ftgrays(text: str) -> str:
    # The note and STANDALONE_ go after the licence block, before any #include or #ifdef.
    licence_end = text.index("*/\n") + len("*/\n")
    text = text[:licence_end] + HEADER_NOTE + text[licence_end:]
    text = replace_once(text, "#define ft_jmp_buf  jmp_buf\n", FALL_THROUGH, "FALL_THROUGH shim")
    text = replace_once(text, INT64_OLD, INT64_NEW, "64-bit types")
    text = replace_once(text, POOL_CAP_OLD, POOL_CAP_NEW, "pool cap")
    text = replace_once(text, RASTER_OLD, RASTER_NEW, "raster object")
    text = replace_once(text, CONVERT_OLD, CONVERT_NEW, "gray_convert_glyph")
    text = replace_once(text, CALL_OLD, CALL_NEW, "gray_convert_glyph call")
    text = replace_once(text, RESET_OLD, RESET_NEW, "gray_raster_reset")
    text = replace_once(text, TAGS_OLD, TAGS_NEW, "outline tags type")
    text = replace_once(text, RENAME_OLD + ",\n", RENAME_NEW + ",\n", "raster funcs name")
    return text


def patch_ftgrays_h(text: str) -> str:
    return replace_once(text, RENAME_OLD + ";", RENAME_NEW + ";  /* PATCH(witchhunt): renamed */",
                        "ftgrays.h name")


def main() -> None:
    DST.mkdir(parents=True, exist_ok=True)
    files = {
        "ftgrays.c": patch_ftgrays(read_lf(SRC / "src/smooth/ftgrays.c")),
        "ftgrays.h": patch_ftgrays_h(read_lf(SRC / "src/smooth/ftgrays.h")),
        "ftimage.h": read_lf(SRC / "include/freetype/ftimage.h"),
        "FTL.TXT": read_lf(SRC / "FTL.TXT"),
    }
    for name, text in files.items():
        (DST / name).write_bytes(text.encode("utf-8"))
        print(f"wrote {(DST / name).relative_to(ROOT)}")


if __name__ == "__main__":
    main()
```

Run: `python scripts/vendor_ftgrays.py`
Expected: four `wrote lib/EpdFont/ttf/third_party/freetype/...` lines (`ftgrays.c`, `ftgrays.h`, `ftimage.h`,
`FTL.TXT`).

- [ ] **Step 5: Write the patch notes and the include shim**

Create `lib/EpdFont/ttf/third_party/freetype/PATCHES.md`:

<!-- plan-file: lib/EpdFont/ttf/third_party/freetype/PATCHES.md -->
```markdown
# FreeType smooth rasterizer (vendored)

`ftgrays.c`, `ftgrays.h` and `ftimage.h` are FreeType 2.13.3's anti-aliasing rasterizer, the version the
SDK vendors under `freeink-sdk/libs/font/FreeInkFont/third_party/freetype`. The TTF engine
(`lib/EpdFont/ttf/TtfRasterizer.cpp`) uses it to turn glyph outlines into coverage spans. It is built
standalone: nothing else from FreeType is compiled into the firmware.

Licence: the FreeType License, `FTL.TXT` in this folder. It requires crediting the FreeType project in
the documentation of any product that ships this code; that credit belongs in the user guide's TTF
section.

## Regenerating

    python scripts/vendor_ftgrays.py

The script copies the four files from the SDK with LF line endings and applies the patches below. Each
patch must match its anchor exactly once, so a FreeType update that moves the code fails in the script.

## Patches (each marked `PATCH(witchhunt)` in the source)

1. **Standalone build.** `ftgrays.c` defines `STANDALONE_` itself, so PlatformIO and CMake compile it
   directly with no extra flags.
2. **`FALL_THROUGH`.** FreeType's `compiler-macros.h` defines it; the standalone build has none. Defined
   as `__attribute__((fallthrough))` on GCC 7+ and clang.
3. **The cell pool comes from the caller.** Upstream keeps the pool in a stack array in
   `gray_convert_glyph` (the SDK's copy caps it at 4 KB; still a 4,400 B frame on the C3). Here the pool
   is passed to `raster_reset(raster, pool, bytes)`, kept in the raster object, and `gray_convert_glyph`
   works in it. The pool must be aligned for a cell and at least 2,048 B; without one, `raster_render`
   returns `Invalid_Argument`. The pool size changes only how many bands a glyph is split into, never
   the output. `FT_MAX_GRAY_POOL` and the SDK's pool cap are removed.
4. **`tags` type.** `FT_Outline_Decompose` declared `char* tags` against `unsigned char*` in `ftimage.h`
   (a sign warning in the standalone build).
5. **Renamed raster.** `ft_grays_raster` is `ttf_grays_raster`, so it cannot clash with a full FreeType
   linked into the same program (the host tests link one for the comparison).

The standalone raster object is a single static (upstream): renders must not overlap. The TTF engine
renders only under `RenderLock`.
```

Create `lib/EpdFont/ttf/TtfGrays.h`:

<!-- plan-file: lib/EpdFont/ttf/TtfGrays.h -->
```cpp
#pragma once

// The vendored FreeType rasterizer (third_party/freetype/PATCHES.md), for the one .cpp that drives it.
// ftimage.h and ftgrays.h expect the macros that ftgrays.c defines for itself before it includes them.
// Never include this from a header: ftimage.h's types would clash with a full FreeType's in a program that
// links both, as the host tests do.
#ifndef STANDALONE_
#define STANDALONE_
#endif
#ifndef FT_BEGIN_HEADER
#define FT_BEGIN_HEADER
#define FT_END_HEADER
#endif
#ifndef FT_STATIC_BYTE_CAST
#define FT_STATIC_BYTE_CAST(type, var) (type)(unsigned char)(var)
#endif

#include "third_party/freetype/ftgrays.h"
```

- [ ] **Step 6: Check the vendored file on both compilers**

```bash
F=lib/EpdFont/ttf/third_party/freetype/ftgrays.c
RV=/c/pio/packages/toolchain-riscv32-esp/bin
grep -c "PATCH(witchhunt)" $F
gcc -std=c11 -O2 -Wall -Wextra -pedantic -c $F -o /tmp/ftgrays-host.o
$RV/riscv32-esp-elf-gcc.exe -march=rv32imc_zicsr_zifencei -mabi=ilp32 -Os -fstack-usage -c $F -o /tmp/ftgrays-rv.o
sort -t$'\t' -k2 -n -r /tmp/ftgrays-rv.su | head -2
$RV/riscv32-esp-elf-nm.exe -u /tmp/ftgrays-rv.o
```

Expected:
- `9` patch markers.
- No warnings from either compiler.
- The largest RISC-V frame is `gray_raster_render`, 704 B. Unpatched, `gray_convert_glyph` alone was 4,400 B.
- The undefined symbols are exactly `__ashldi3 __divdi3 longjmp memcpy memset setjmp`: integer helpers only,
  no `__*sf*` or `__*df*`.

- [ ] **Step 7: Run the tests**

```bash
cmake -S test -B build/test
cmake --build build/test --target TtfRasterizerTest -j 8
./build/test/ttf_rasterizer/TtfRasterizerTest.exe
```

Expected:
- Configure prints `TtfRasterizerTest sanitizers: -fsanitize=undefined;-fsanitize-undefined-trap-on-error`
  with MinGW. CI's Linux GCC gets ASan as well.
- `[  PASSED  ] 3 tests.`

- [ ] **Step 8: Keep the formatter off the vendored code**

In `bin/clang-format-fix` the vendored directories are excluded twice, once for `-g` and once for a full run.
Replace this text in both places:

<!-- plan-edit: bin/clang-format-fix all -->
```bash
lib/uzlib/|lib/TJpgDec/)
```
with:

```bash
lib/uzlib/|lib/TJpgDec/|lib/EpdFont/ttf/third_party/)
```

Then format the new files you wrote:
`/c/tmp/cf21/bin/clang-format-21.exe -i lib/EpdFont/ttf/TtfGrays.h test/ttf_rasterizer/TtfGraysTest.cpp`.
Check that `git status --short` shows nothing changed under `third_party/` since the script wrote it.

- [ ] **Step 9: Commit**

```bash
git add scripts/vendor_ftgrays.py lib/EpdFont/ttf/third_party lib/EpdFont/ttf/TtfGrays.h test/ttf_rasterizer test/CMakeLists.txt bin/clang-format-fix
git commit -m "feat(ttf): vendor FreeType's smooth rasterizer, standalone with the pool from the caller"
```

---

### Task 2: `FontBytes` and FreeType's fixed-point maths

**Files:**
- Create: `lib/EpdFont/ttf/FontBytes.h`, `lib/EpdFont/ttf/TtfFixed.h`
- Create: `test/ttf_rasterizer/FontBytesTest.cpp`, `test/ttf_rasterizer/TtfFixedTest.cpp`
- Modify: `test/ttf_rasterizer/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `class FontBytes` (global namespace): `FontBytes(const uint8_t* data, uint32_t size)`, `size()`,
    `u8/u16/i16/u32(uint32_t offset)`, `bool read(uint32_t offset, uint32_t len, void* dst)`,
    `outOfRange()`, `ioError()`, `clearErrors()`. A read past the end returns 0 (or false) and sets
    `outOfRange()`.
  - `namespace ttf`: `int64_t mulFixWide(int32_t, int32_t)`, `int32_t mulFix(int32_t, int32_t)`,
    `int32_t divFix(int32_t, int32_t)` and `int32_t mulDiv(int32_t, int32_t, int32_t)`, all `constexpr`.

- [ ] **Step 1: Write the failing tests**

Create `test/ttf_rasterizer/FontBytesTest.cpp`:

<!-- plan-file: test/ttf_rasterizer/FontBytesTest.cpp -->
```cpp
// FontBytes: big-endian reads, every one bounds-checked against the file.
#include <gtest/gtest.h>

#include <cstdint>

#include "FontBytes.h"

namespace {
const uint8_t kData[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
}

TEST(FontBytes, ReadsBigEndian) {
  FontBytes bytes(kData, sizeof kData);
  EXPECT_EQ(0x12, bytes.u8(0));
  EXPECT_EQ(0x3456, bytes.u16(1));
  EXPECT_EQ(-0x6544, bytes.i16(4));  // 0x9ABC
  EXPECT_EQ(0x3456789Au, bytes.u32(1));
  EXPECT_FALSE(bytes.outOfRange());
  EXPECT_FALSE(bytes.ioError());
}

TEST(FontBytes, TheLastBytesAreInRangeAndOneMoreIsNot) {
  FontBytes bytes(kData, sizeof kData);
  EXPECT_EQ(0xBC, bytes.u8(5));
  EXPECT_EQ(0x9ABC, bytes.u16(4));
  EXPECT_EQ(0x56789ABCu, bytes.u32(2));
  EXPECT_FALSE(bytes.outOfRange());
  EXPECT_EQ(0u, bytes.u32(3));
  EXPECT_TRUE(bytes.outOfRange());
}

TEST(FontBytes, EveryKindOfReadPastTheEndReturnsZeroAndSetsTheFlag) {
  for (int kind = 0; kind < 4; ++kind) {
    FontBytes bytes(kData, sizeof kData);
    switch (kind) {
      case 0:
        EXPECT_EQ(0, bytes.u8(6));
        break;
      case 1:
        EXPECT_EQ(0, bytes.u16(5));
        break;
      case 2:
        EXPECT_EQ(0, bytes.i16(0xFFFFFFFFu));  // the offset wraps: still refused
        break;
      default:
        EXPECT_EQ(0u, bytes.u32(0xFFFFFFFEu));
    }
    EXPECT_TRUE(bytes.outOfRange()) << kind;
  }
}

TEST(FontBytes, ReadCopiesAllOrNothing) {
  FontBytes bytes(kData, sizeof kData);
  uint8_t out[4] = {0xEE, 0xEE, 0xEE, 0xEE};
  ASSERT_TRUE(bytes.read(2, 4, out));
  EXPECT_EQ(0x56, out[0]);
  EXPECT_EQ(0xBC, out[3]);
  uint8_t untouched[4] = {0xEE, 0xEE, 0xEE, 0xEE};
  EXPECT_FALSE(bytes.read(3, 4, untouched));
  EXPECT_EQ(0xEE, untouched[0]);
  EXPECT_TRUE(bytes.outOfRange());
  EXPECT_TRUE(bytes.read(6, 0, untouched));  // nothing, at the very end
}

TEST(FontBytes, ClearErrorsForgetsTheFlag) {
  FontBytes bytes(kData, sizeof kData);
  bytes.u8(99);
  ASSERT_TRUE(bytes.outOfRange());
  bytes.clearErrors();
  EXPECT_FALSE(bytes.outOfRange());
  EXPECT_EQ(0x12, bytes.u8(0));
}

TEST(FontBytes, AnEmptySourceReadsNothing) {
  FontBytes bytes;
  EXPECT_EQ(0u, bytes.size());
  EXPECT_EQ(0, bytes.u8(0));
  EXPECT_TRUE(bytes.outOfRange());
}
```

Create `test/ttf_rasterizer/TtfFixedTest.cpp`. Its expected scales are the values FreeType itself reports for
the bundled fonts (measured with freetype-py while writing this plan):

<!-- plan-file: test/ttf_rasterizer/TtfFixedTest.cpp -->
```cpp
// FreeType's fixed-point helpers, to the bit. The expected scales are what FreeType 2.13 itself reports for
// the bundled fonts at 150 dpi (14 pt Bookerly: x_scale 122356; 14 pt Inter: 59744).
#include <gtest/gtest.h>

#include <cstdint>

#include "TtfFixed.h"

using namespace ttf;

TEST(TtfFixed, MulFixRoundsHalfAwayFromZero) {
  EXPECT_EQ(5, mulFix(5, 0x10000));
  EXPECT_EQ(1, mulFix(1, 0x8000));    // 0.5
  EXPECT_EQ(-1, mulFix(-1, 0x8000));  // -0.5
  EXPECT_EQ(0, mulFix(1, 0x7FFF));
  EXPECT_EQ(-2, mulFix(3, -0x8000));  // -1.5
  EXPECT_EQ(1973, mulFix(1057, 122356));
  EXPECT_EQ(-487, mulFix(-261, 122356));
}

TEST(TtfFixed, MulFixWideKeepsTheHighBits) {
  EXPECT_EQ(int64_t{1} << 34, mulFixWide(1 << 24, 1 << 26));
  EXPECT_EQ(-(int64_t{1} << 34), mulFixWide(-(1 << 24), 1 << 26));
}

TEST(TtfFixed, DivFixGivesFreeTypesScales) {
  EXPECT_EQ(122356, divFix(1867, 1000));  // 14 pt, 1000 units per em
  EXPECT_EQ(104858, divFix(1600, 1000));  // 12 pt
  EXPECT_EQ(59744, divFix(1867, 2048));   // 14 pt, 2048 units per em
  EXPECT_EQ(76800, divFix(2400, 2048));   // 18 pt
  EXPECT_EQ(-122356, divFix(-1867, 1000));
  EXPECT_EQ(0x7FFFFFFF, divFix(1, 0));
}

TEST(TtfFixed, MulDivRoundsHalfAwayFromZero) {
  EXPECT_EQ(955906, mulDiv(500, 122356, 64));  // a 500-unit advance at 14 pt, in 16.16
  EXPECT_EQ(-955906, mulDiv(-500, 122356, 64));
  EXPECT_EQ(2, mulDiv(3, 1, 2));  // 1.5
  EXPECT_EQ(0x7FFFFFFF, mulDiv(1, 1, 0));
}
```

Replace `test/ttf_rasterizer/CMakeLists.txt` with:

<!-- plan-file: test/ttf_rasterizer/CMakeLists.txt -->
```cmake
# The TTF engine (lib/EpdFont/ttf): FontBytes, face parsing, the integer TrueType loader and the vendored
# ftgrays, checked against FreeType itself over every bundled TTF.

add_executable(TtfRasterizerTest
  TtfGraysTest.cpp
  FontBytesTest.cpp
  TtfFixedTest.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/third_party/freetype/ftgrays.c
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(TtfRasterizerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/shims
)

target_link_libraries(TtfRasterizerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

# Spec 9.3: the engine runs under AddressSanitizer and UBSan (CI's Linux GCC). Where ASan cannot link
# (MinGW), UBSan still traps on undefined behaviour without its runtime. FreeType is not instrumented.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address,undefined)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_ASAN)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=undefined -fsanitize-undefined-trap-on-error")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_UBSAN_TRAP)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(TTF_TESTS_HAVE_ASAN)
  set(TTF_SANITIZERS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
elseif(TTF_TESTS_HAVE_UBSAN_TRAP)
  set(TTF_SANITIZERS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
endif()
target_compile_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
target_link_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
message(STATUS "TtfRasterizerTest sanitizers: ${TTF_SANITIZERS}")

gtest_discover_tests(TtfRasterizerTest DISCOVERY_TIMEOUT 60)
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build/test --target TtfRasterizerTest -j 8`
Expected: compile errors, `FontBytes.h: No such file or directory` and `TtfFixed.h: No such file or directory`.

- [ ] **Step 3: Write the code**

Create `lib/EpdFont/ttf/FontBytes.h`:

<!-- plan-file: lib/EpdFont/ttf/FontBytes.h -->
```cpp
#pragma once

#include <cstdint>
#include <cstring>

/// The TTF engine's only way to read a font file (spec 5.1). Values are big-endian, as in every TrueType
/// table, and every access is bounds-checked against the file: a read past its end returns zeros and sets
/// outOfRange(), which means the font is broken (permanent). ioError() is the transient kind, an SD read
/// that failed; it belongs to the SD mode (Stage 2c) and stays false in flash mode.
///
/// Flash mode: the whole file is addressable at `data` (the mapping of an installed style, or a buffer in the
/// host tests). FontBytes does not own it, and the caller re-points it when the mapping moves.
class FontBytes {
 public:
  FontBytes() = default;
  FontBytes(const uint8_t* data, const uint32_t size) : data_(data), size_(size) {}

  uint32_t size() const { return size_; }

  uint8_t u8(const uint32_t offset) {
    if (!contains(offset, 1)) return fail();
    return data_[offset];
  }
  uint16_t u16(const uint32_t offset) {
    if (!contains(offset, 2)) return fail();
    return static_cast<uint16_t>(data_[offset] << 8 | data_[offset + 1]);
  }
  int16_t i16(const uint32_t offset) { return static_cast<int16_t>(u16(offset)); }
  uint32_t u32(const uint32_t offset) {
    if (!contains(offset, 4)) return fail();
    const uint8_t* p = data_ + offset;
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           p[3];
  }
  /// Copies `len` bytes at `offset` to `dst`. False, with nothing copied, unless all of them are in the file.
  bool read(const uint32_t offset, const uint32_t len, void* dst) {
    if (!contains(offset, len)) {
      fail();
      return false;
    }
    if (len > 0) std::memcpy(dst, data_ + offset, len);
    return true;
  }

  bool outOfRange() const { return outOfRange_; }
  bool ioError() const { return ioError_; }
  /// Forgets both flags. The engine clears them before each glyph, so a broken glyph does not taint the next.
  void clearErrors() {
    outOfRange_ = false;
    ioError_ = false;
  }

 private:
  bool contains(const uint32_t offset, const uint32_t len) const { return offset <= size_ && len <= size_ - offset; }
  uint8_t fail() {
    outOfRange_ = true;
    return 0;
  }

  const uint8_t* data_ = nullptr;
  uint32_t size_ = 0;
  bool outOfRange_ = false;
  bool ioError_ = false;
};
```

Create `lib/EpdFont/ttf/TtfFixed.h`:

<!-- plan-file: lib/EpdFont/ttf/TtfFixed.h -->
```cpp
#pragma once

#include <cstdint>

/// FreeType's fixed-point arithmetic (ftcalc.c, 64-bit build), bit for bit. The TTF engine scales outlines and
/// metrics with these so its glyphs match FreeType's, and so the PC converter's: a TTF at 14 pt lays out like
/// a .cpfont converted at 14 pt (spec 5.2). Integer only; the engine links no floating point
/// (scripts/check_no_float.py).
namespace ttf {

/// a * b / 65536, rounded half away from zero (FT_MulFix), with the full 64-bit result for callers that
/// range-check it.
constexpr int64_t mulFixWide(const int32_t a, const int32_t b) {
  const int64_t ab = static_cast<int64_t>(a) * b;
  return (ab + 0x8000 - (ab < 0 ? 1 : 0)) >> 16;
}

/// FT_MulFix: 16.16 factors scale font units and 26.6 values.
constexpr int32_t mulFix(const int32_t a, const int32_t b) { return static_cast<int32_t>(mulFixWide(a, b)); }

/// a * 65536 / b, rounded half away from zero (FT_DivFix); 0x7FFFFFFF when b is 0.
constexpr int32_t divFix(const int32_t a, const int32_t b) {
  const bool negative = (a < 0) != (b < 0);
  const uint64_t ua = static_cast<uint64_t>(a < 0 ? -static_cast<int64_t>(a) : a);
  const uint64_t ub = static_cast<uint64_t>(b < 0 ? -static_cast<int64_t>(b) : b);
  const uint64_t q = ub > 0 ? ((ua << 16) + (ub >> 1)) / ub : 0x7FFFFFFFu;
  return static_cast<int32_t>(negative ? -static_cast<int64_t>(q) : static_cast<int64_t>(q));
}

/// a * b / c, rounded half away from zero (FT_MulDiv); 0x7FFFFFFF when c is 0.
constexpr int32_t mulDiv(const int32_t a, const int32_t b, const int32_t c) {
  const bool negative = ((a < 0) != (b < 0)) != (c < 0);
  const uint64_t ua = static_cast<uint64_t>(a < 0 ? -static_cast<int64_t>(a) : a);
  const uint64_t ub = static_cast<uint64_t>(b < 0 ? -static_cast<int64_t>(b) : b);
  const uint64_t uc = static_cast<uint64_t>(c < 0 ? -static_cast<int64_t>(c) : c);
  const uint64_t d = uc > 0 ? (ua * ub + (uc >> 1)) / uc : 0x7FFFFFFFu;
  return static_cast<int32_t>(negative ? -static_cast<int64_t>(d) : static_cast<int64_t>(d));
}

}  // namespace ttf
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build build/test --target TtfRasterizerTest -j 8 && ./build/test/ttf_rasterizer/TtfRasterizerTest.exe`
Expected: `[  PASSED  ] 13 tests.`

- [ ] **Step 5: Format and commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe -i lib/EpdFont/ttf/FontBytes.h lib/EpdFont/ttf/TtfFixed.h test/ttf_rasterizer/FontBytesTest.cpp test/ttf_rasterizer/TtfFixedTest.cpp
git add lib/EpdFont/ttf/FontBytes.h lib/EpdFont/ttf/TtfFixed.h test/ttf_rasterizer
git commit -m "feat(ttf): bounds-checked font reads and FreeType's fixed-point maths"
```

---

### Task 3: `ttf::Face`: tables, cmap, metrics

The face parser reads what every glyph needs and reproduces FreeType's choices:
- the Unicode cmap it picks;
- how it reads `hmtx` past `numberOfHMetrics`;
- when it takes the ascender from OS/2;
- the converter's sizes.

The tests run on fonts built in memory by `TtfBuilder.h`, which later tasks reuse.

**Files:**
- Create: `lib/EpdFont/ttf/TtfFace.h`, `lib/EpdFont/ttf/TtfFace.cpp`
- Create: `test/ttf_rasterizer/TtfBuilder.h`, `test/ttf_rasterizer/TtfTestUtil.h`, `test/ttf_rasterizer/TtfFaceTest.cpp`
- Modify: `test/ttf_rasterizer/CMakeLists.txt`

**Interfaces:**
- Consumes: `FontBytes`; `ttf::mulFix`, `ttf::divFix`, `ttf::mulDiv` (Task 2).
- Produces (`namespace ttf`):
  - `enum class FaceStatus { Ok, Unsupported, Malformed, Transient }`.
  - `struct Face`, with table offsets and lengths and the metrics: `glyf/glyfLength`, `loca/locaLength`,
    `hmtx/hmtxLength`, `cmapSubtable`, `cmapEnd`, `unitsPerEm`, `numGlyphs`, `numHMetrics`, `maxPoints`,
    `maxContours`, `xMin/yMin/xMax/yMax`, `ascender/descender/height`, `cmapFormat`, `longLoca`.
  - `FaceStatus parseFace(FontBytes&, Face*)` and `uint16_t glyphIndex(FontBytes&, const Face&, uint32_t codepoint)`.
  - `constexpr uint32_t ppemFromPoints(uint32_t)` and `int32_t unitsToPixels(const Face&, uint32_t ppem26_6)`.
  - `void horizontalMetrics(FontBytes&, const Face&, uint16_t glyph, uint16_t* advance, int16_t* lsb)`.
  - `uint16_t advance12_4(FontBytes&, const Face&, uint16_t glyph, uint32_t ppem26_6)`.
  - `struct LineMetrics { int32_t advanceY, ascender, descender; }` and `LineMetrics lineMetrics(const Face&, uint32_t ppem26_6)`.
- Produces (tests, `namespace ttf_test`):
  - `TtfBuilder.h`: `Font`, `Glyph`, `Point`, `Component`, `buildFont`, `rect`, `findRecord`,
    `cmapFormat4/12`, `cmapTable`, `put16/put32/set16/set32`, `tagOf`.
  - `TtfTestUtil.h`: `readFont(relative)`, `bundledTtfs()`, `picture(glyph, bits)`.

- [ ] **Step 1: Write the font builder and the test helpers**

Create `test/ttf_rasterizer/TtfBuilder.h`:

<!-- plan-file: test/ttf_rasterizer/TtfBuilder.h -->
```cpp
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

/// Small TrueType fonts built in memory for the TTF engine's tests: the tables the engine reads, laid out as a
/// font tool writes them (checksums left 0; nothing reads them).
namespace ttf_test {

using Bytes = std::vector<uint8_t>;

inline void put16(Bytes& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}
inline void put32(Bytes& out, const uint32_t value) {
  put16(out, value >> 16);
  put16(out, value & 0xFFFF);
}
inline void set16(Bytes& out, const size_t at, const uint32_t value) {
  out[at] = static_cast<uint8_t>(value >> 8);
  out[at + 1] = static_cast<uint8_t>(value);
}
inline void set32(Bytes& out, const size_t at, const uint32_t value) {
  set16(out, at, value >> 16);
  set16(out, at + 2, value & 0xFFFF);
}

struct Point {
  int16_t x = 0;
  int16_t y = 0;
  bool on = true;
};

/// A component of a compound glyph. `transform` holds F2Dot14 values: none, 1 (a scale), 2 (x and y scales) or
/// 4 (a 2x2 matrix in file order). `flags` adds bits to the ones the builder sets itself.
struct Component {
  Component(const uint16_t glyph, const int16_t dx, const int16_t dy, std::vector<int16_t> transform = {},
            const uint16_t flags = 0)
      : glyph(glyph), dx(dx), dy(dy), transform(std::move(transform)), flags(flags) {}

  uint16_t glyph;
  int16_t dx;
  int16_t dy;
  std::vector<int16_t> transform;
  uint16_t flags;
};

/// One glyph: a simple outline, a compound, or `raw` bytes taken as the whole glyf record.
struct Glyph {
  std::vector<std::vector<Point>> contours;
  std::vector<Component> components;
  Bytes raw;
  uint16_t advance = 500;
  bool lsbIsXMin = true;  ///< otherwise `lsb`
  int16_t lsb = 0;
};

struct Font {
  uint32_t sfntVersion = 0x00010000;
  uint16_t unitsPerEm = 1000;
  int16_t ascender = 800;
  int16_t descender = -200;
  int16_t lineGap = 0;
  bool longLoca = false;
  std::vector<Glyph> glyphs;  ///< glyph 0 is .notdef
  std::map<uint32_t, uint16_t> cmap;
  bool cmapFormat12 = false;     ///< a (3,10) format 12 subtable instead of (3,1) format 4
  bool cmapRangeOffsets = false;  ///< format 4 through glyphIdArray instead of deltas
  uint16_t numHMetrics = 0;       ///< 0: one hmtx entry per glyph
  bool os2 = false;
  uint16_t os2Selection = 0;
  int16_t typoAscender = 0;
  int16_t typoDescender = 0;
  int16_t typoLineGap = 0;
  uint16_t winAscent = 0;
  uint16_t winDescent = 0;
  std::vector<std::pair<std::string, Bytes>> extraTables;  ///< added or replacing a built table
};

inline uint32_t tagOf(const std::string& name) {
  return static_cast<uint32_t>(static_cast<uint8_t>(name[0])) << 24 |
         static_cast<uint32_t>(static_cast<uint8_t>(name[1])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(name[2])) << 8 | static_cast<uint8_t>(name[3]);
}

struct Box {
  int16_t xMin = 0, yMin = 0, xMax = 0, yMax = 0;
};

inline Box simpleBox(const Glyph& glyph) {
  Box box;
  bool first = true;
  for (const auto& contour : glyph.contours) {
    for (const Point& p : contour) {
      if (first) {
        box = {p.x, p.y, p.x, p.y};
        first = false;
      }
      box.xMin = std::min(box.xMin, p.x);
      box.yMin = std::min(box.yMin, p.y);
      box.xMax = std::max(box.xMax, p.x);
      box.yMax = std::max(box.yMax, p.y);
    }
  }
  return box;
}

// A simple glyph with the short, same and repeat encodings a font tool uses.
inline Bytes encodeSimple(const Glyph& glyph) {
  Bytes out;
  const Box box = simpleBox(glyph);
  put16(out, static_cast<uint16_t>(glyph.contours.size()));
  put16(out, static_cast<uint16_t>(box.xMin));
  put16(out, static_cast<uint16_t>(box.yMin));
  put16(out, static_cast<uint16_t>(box.xMax));
  put16(out, static_cast<uint16_t>(box.yMax));
  std::vector<Point> points;
  for (const auto& contour : glyph.contours) {
    points.insert(points.end(), contour.begin(), contour.end());
    put16(out, static_cast<uint16_t>(points.size() - 1));
  }
  put16(out, 0);  // no instructions
  std::vector<uint8_t> flags;
  Bytes xs, ys;
  int16_t lastX = 0, lastY = 0;
  for (const Point& p : points) {
    uint8_t flag = p.on ? 0x01 : 0x00;
    const int dx = p.x - lastX, dy = p.y - lastY;
    if (dx == 0) {
      flag |= 0x10;
    } else if (dx >= -255 && dx <= 255) {
      flag |= 0x02 | (dx > 0 ? 0x10 : 0);
      xs.push_back(static_cast<uint8_t>(dx > 0 ? dx : -dx));
    } else {
      put16(xs, static_cast<uint16_t>(dx));
    }
    if (dy == 0) {
      flag |= 0x20;
    } else if (dy >= -255 && dy <= 255) {
      flag |= 0x04 | (dy > 0 ? 0x20 : 0);
      ys.push_back(static_cast<uint8_t>(dy > 0 ? dy : -dy));
    } else {
      put16(ys, static_cast<uint16_t>(dy));
    }
    flags.push_back(flag);
    lastX = p.x;
    lastY = p.y;
  }
  for (size_t i = 0; i < flags.size();) {
    size_t run = 1;
    while (i + run < flags.size() && flags[i + run] == flags[i] && run < 256) ++run;
    if (run > 1) {
      out.push_back(flags[i] | 0x08);
      out.push_back(static_cast<uint8_t>(run - 1));
    } else {
      out.push_back(flags[i]);
    }
    i += run;
  }
  out.insert(out.end(), xs.begin(), xs.end());
  out.insert(out.end(), ys.begin(), ys.end());
  return out;
}

inline Bytes encodeCompound(const Glyph& glyph, const Box& box) {
  Bytes out;
  put16(out, 0xFFFF);  // -1 contours
  put16(out, static_cast<uint16_t>(box.xMin));
  put16(out, static_cast<uint16_t>(box.yMin));
  put16(out, static_cast<uint16_t>(box.xMax));
  put16(out, static_cast<uint16_t>(box.yMax));
  for (size_t i = 0; i < glyph.components.size(); ++i) {
    const Component& c = glyph.components[i];
    const bool words = c.dx < -128 || c.dx > 127 || c.dy < -128 || c.dy > 127;
    uint16_t flags = static_cast<uint16_t>(c.flags | 0x0002);  // ARGS_ARE_XY_VALUES
    if (words) flags |= 0x0001;
    if (i + 1 < glyph.components.size()) flags |= 0x0020;
    if (c.transform.size() == 1) flags |= 0x0008;
    if (c.transform.size() == 2) flags |= 0x0040;
    if (c.transform.size() == 4) flags |= 0x0080;
    put16(out, flags);
    put16(out, c.glyph);
    if (words) {
      put16(out, static_cast<uint16_t>(c.dx));
      put16(out, static_cast<uint16_t>(c.dy));
    } else {
      out.push_back(static_cast<uint8_t>(c.dx));
      out.push_back(static_cast<uint8_t>(c.dy));
    }
    for (const int16_t v : c.transform) put16(out, static_cast<uint16_t>(v));
  }
  return out;
}

inline Bytes cmapFormat4(const std::map<uint32_t, uint16_t>& map, const bool rangeOffsets) {
  // Segments of consecutive codepoints (and, for deltas, consecutive glyphs), then the 0xFFFF segment.
  struct Segment {
    uint16_t start, end;
    std::vector<uint16_t> glyphs;
  };
  std::vector<Segment> segments;
  for (const auto& [cp, glyph] : map) {
    if (cp > 0xFFFF) continue;
    if (!segments.empty() && segments.back().end + 1u == cp &&
        (rangeOffsets || segments.back().glyphs.back() + 1u == glyph)) {
      segments.back().end = static_cast<uint16_t>(cp);
      segments.back().glyphs.push_back(glyph);
    } else {
      segments.push_back({static_cast<uint16_t>(cp), static_cast<uint16_t>(cp), {glyph}});
    }
  }
  segments.push_back({0xFFFF, 0xFFFF, {0}});
  const uint16_t n = static_cast<uint16_t>(segments.size());
  Bytes out;
  put16(out, 4);
  put16(out, 0);  // length, set below
  put16(out, 0);  // language
  put16(out, n * 2);
  put16(out, 0);  // searchRange, entrySelector, rangeShift: nothing reads them
  put16(out, 0);
  put16(out, 0);
  for (const Segment& s : segments) put16(out, s.end);
  put16(out, 0);
  for (const Segment& s : segments) put16(out, s.start);
  for (const Segment& s : segments) {
    const bool last = s.start == 0xFFFF;
    put16(out, rangeOffsets || last ? (last ? 1 : 0) : static_cast<uint16_t>(s.glyphs[0] - s.start));
  }
  // idRangeOffset: bytes from this entry to the segment's first glyphIdArray entry.
  uint32_t arrayIndex = 0;
  for (uint16_t i = 0; i < n; ++i) {
    const bool last = segments[i].start == 0xFFFF;
    if (!rangeOffsets || last) {
      put16(out, 0);
      continue;
    }
    put16(out, static_cast<uint16_t>(2 * (n - i) + 2 * arrayIndex));
    arrayIndex += static_cast<uint32_t>(segments[i].glyphs.size());
  }
  if (rangeOffsets) {
    for (const Segment& s : segments) {
      if (s.start == 0xFFFF) continue;
      for (const uint16_t glyph : s.glyphs) put16(out, glyph);
    }
  }
  set16(out, 2, static_cast<uint32_t>(out.size()));
  return out;
}

inline Bytes cmapFormat12(const std::map<uint32_t, uint16_t>& map) {
  std::vector<std::array<uint32_t, 3>> groups;
  for (const auto& [cp, glyph] : map) {
    if (!groups.empty() && groups.back()[1] + 1 == cp && groups.back()[2] + (cp - groups.back()[0]) == glyph) {
      groups.back()[1] = cp;
    } else {
      groups.push_back({cp, cp, glyph});
    }
  }
  Bytes out;
  put16(out, 12);
  put16(out, 0);
  put32(out, static_cast<uint32_t>(16 + 12 * groups.size()));
  put32(out, 0);
  put32(out, static_cast<uint32_t>(groups.size()));
  for (const auto& g : groups) {
    put32(out, g[0]);
    put32(out, g[1]);
    put32(out, g[2]);
  }
  return out;
}

/// A cmap table from (platform, encoding, subtable) records, in the order given.
inline Bytes cmapTable(const std::vector<std::tuple<uint16_t, uint16_t, Bytes>>& subtables) {
  Bytes out;
  put16(out, 0);
  put16(out, static_cast<uint16_t>(subtables.size()));
  uint32_t offset = 4 + 8 * static_cast<uint32_t>(subtables.size());
  for (const auto& [platform, encoding, data] : subtables) {
    put16(out, platform);
    put16(out, encoding);
    put32(out, offset);
    offset += static_cast<uint32_t>(data.size());
  }
  for (const auto& entry : subtables) {
    const Bytes& data = std::get<2>(entry);
    out.insert(out.end(), data.begin(), data.end());
  }
  return out;
}

inline Bytes buildFont(const Font& font) {
  // glyf and loca, and each glyph's box (compound boxes from their components, one level deep).
  Bytes glyf;
  std::vector<uint32_t> offsets;
  std::vector<Box> boxes(font.glyphs.size());
  for (size_t g = 0; g < font.glyphs.size(); ++g) boxes[g] = simpleBox(font.glyphs[g]);
  for (size_t g = 0; g < font.glyphs.size(); ++g) {
    const Glyph& glyph = font.glyphs[g];
    offsets.push_back(static_cast<uint32_t>(glyf.size()));
    Bytes record;
    if (!glyph.raw.empty()) {
      record = glyph.raw;
    } else if (!glyph.components.empty()) {
      Box box = boxes[glyph.components[0].glyph];
      for (const Component& c : glyph.components) {
        const Box& b = boxes[c.glyph];
        box.xMin = std::min<int16_t>(box.xMin, static_cast<int16_t>(b.xMin + c.dx));
        box.yMin = std::min<int16_t>(box.yMin, static_cast<int16_t>(b.yMin + c.dy));
        box.xMax = std::max<int16_t>(box.xMax, static_cast<int16_t>(b.xMax + c.dx));
        box.yMax = std::max<int16_t>(box.yMax, static_cast<int16_t>(b.yMax + c.dy));
      }
      boxes[g] = box;
      record = encodeCompound(glyph, box);
    } else if (!glyph.contours.empty()) {
      record = encodeSimple(glyph);
    }
    glyf.insert(glyf.end(), record.begin(), record.end());
    while (glyf.size() % 4) glyf.push_back(0);
  }
  offsets.push_back(static_cast<uint32_t>(glyf.size()));
  Bytes loca;
  for (const uint32_t offset : offsets) {
    if (font.longLoca) {
      put32(loca, offset);
    } else {
      put16(loca, offset / 2);
    }
  }

  const uint16_t numGlyphs = static_cast<uint16_t>(font.glyphs.size());
  const uint16_t numHMetrics = font.numHMetrics ? font.numHMetrics : numGlyphs;
  Bytes hmtx;
  for (uint16_t g = 0; g < numGlyphs; ++g) {
    const Glyph& glyph = font.glyphs[g];
    const int16_t lsb = glyph.lsbIsXMin ? boxes[g].xMin : glyph.lsb;
    if (g < numHMetrics) put16(hmtx, glyph.advance);
    put16(hmtx, static_cast<uint16_t>(lsb));
  }

  // head's box: the union of the glyphs that have one.
  Box all;
  bool any = false;
  for (size_t g = 0; g < boxes.size(); ++g) {
    if (font.glyphs[g].contours.empty() && font.glyphs[g].components.empty()) continue;
    const Box& b = boxes[g];
    all = any ? Box{std::min(all.xMin, b.xMin), std::min(all.yMin, b.yMin), std::max(all.xMax, b.xMax),
                    std::max(all.yMax, b.yMax)}
              : b;
    any = true;
  }
  const int16_t xMin = all.xMin, yMin = all.yMin, xMax = all.xMax, yMax = all.yMax;
  Bytes head(54, 0);
  set32(head, 0, 0x00010000);
  set32(head, 12, 0x5F0F3CF5);
  set16(head, 18, font.unitsPerEm);
  set16(head, 36, static_cast<uint16_t>(xMin));
  set16(head, 38, static_cast<uint16_t>(yMin));
  set16(head, 40, static_cast<uint16_t>(xMax));
  set16(head, 42, static_cast<uint16_t>(yMax));
  set16(head, 50, font.longLoca ? 1 : 0);

  Bytes hhea(36, 0);
  set32(hhea, 0, 0x00010000);
  set16(hhea, 4, static_cast<uint16_t>(font.ascender));
  set16(hhea, 6, static_cast<uint16_t>(font.descender));
  set16(hhea, 8, static_cast<uint16_t>(font.lineGap));
  set16(hhea, 34, numHMetrics);

  uint16_t maxPoints = 0, maxContours = 0;
  for (const Glyph& glyph : font.glyphs) {
    uint16_t points = 0;
    for (const auto& contour : glyph.contours) points = static_cast<uint16_t>(points + contour.size());
    maxPoints = std::max(maxPoints, points);
    maxContours = std::max(maxContours, static_cast<uint16_t>(glyph.contours.size()));
  }
  Bytes maxp(32, 0);
  set32(maxp, 0, 0x00010000);
  set16(maxp, 4, numGlyphs);
  set16(maxp, 6, maxPoints);
  set16(maxp, 8, maxContours);
  set16(maxp, 10, maxPoints);  // maxCompositePoints: a generous stand-in
  set16(maxp, 12, maxContours);

  std::vector<std::pair<std::string, Bytes>> tables = {
      {"head", head},
      {"hhea", hhea},
      {"maxp", maxp},
      {"cmap", font.cmapFormat12 ? cmapTable({{3, 10, cmapFormat12(font.cmap)}})
                                 : cmapTable({{3, 1, cmapFormat4(font.cmap, font.cmapRangeOffsets)}})},
      {"loca", loca},
      {"glyf", glyf},
      {"hmtx", hmtx},
  };
  if (font.os2) {
    Bytes os2(78, 0);
    set16(os2, 62, font.os2Selection);
    set16(os2, 68, static_cast<uint16_t>(font.typoAscender));
    set16(os2, 70, static_cast<uint16_t>(font.typoDescender));
    set16(os2, 72, static_cast<uint16_t>(font.typoLineGap));
    set16(os2, 74, font.winAscent);
    set16(os2, 76, font.winDescent);
    tables.emplace_back("OS/2", os2);
  }
  for (const auto& [name, data] : font.extraTables) {
    const auto it = std::find_if(tables.begin(), tables.end(), [&](const auto& t) { return t.first == name; });
    if (it != tables.end()) {
      it->second = data;
    } else {
      tables.emplace_back(name, data);
    }
  }
  std::sort(tables.begin(), tables.end(), [](const auto& a, const auto& b) { return tagOf(a.first) < tagOf(b.first); });

  Bytes out;
  put32(out, font.sfntVersion);
  put16(out, static_cast<uint16_t>(tables.size()));
  put16(out, 0);
  put16(out, 0);
  put16(out, 0);
  uint32_t offset = 12 + 16 * static_cast<uint32_t>(tables.size());
  for (const auto& [name, data] : tables) {
    put32(out, tagOf(name));
    put32(out, 0);
    put32(out, offset);
    put32(out, static_cast<uint32_t>(data.size()));
    offset += (static_cast<uint32_t>(data.size()) + 3) & ~3u;
  }
  for (const auto& entry : tables) {
    const Bytes& data = entry.second;
    out.insert(out.end(), data.begin(), data.end());
    while (out.size() % 4) out.push_back(0);
  }
  return out;
}

/// Where the table directory's record for `name` starts in `font` (0 when it has none).
inline size_t findRecord(const Bytes& font, const std::string& name) {
  const uint32_t tables = static_cast<uint32_t>(font[4] << 8 | font[5]);
  for (uint32_t i = 0; i < tables; ++i) {
    const size_t at = 12 + 16 * i;
    if (font[at] == name[0] && font[at + 1] == name[1] && font[at + 2] == name[2] && font[at + 3] == name[3]) return at;
  }
  return 0;
}

/// A rectangle in font units, counter-clockwise from the bottom left (TrueType's outer contours run
/// clockwise, but the nonzero rule fills either way).
inline std::vector<Point> rect(const int16_t x0, const int16_t y0, const int16_t x1, const int16_t y1) {
  return {{x0, y0, true}, {x1, y0, true}, {x1, y1, true}, {x0, y1, true}};
}

}  // namespace ttf_test
```

Create `test/ttf_rasterizer/TtfTestUtil.h`:

<!-- plan-file: test/ttf_rasterizer/TtfTestUtil.h -->
```cpp
#pragma once

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "EpdFontData.h"

namespace ttf_test {

/// A bundled font source, relative to lib/EpdFont/builtinFonts/source.
inline std::vector<uint8_t> readFont(const std::string& relative) {
  std::ifstream in(std::string(TTF_FONT_DIR) + "/" + relative, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// Every TrueType source the reader bundles (the OpenDyslexic sources are CFF).
inline const std::vector<std::string>& bundledTtfs() {
  static const std::vector<std::string> fonts = {
      "Bookerly/Bookerly-Regular.ttf", "Bookerly/Bookerly-Bold.ttf",       "Bookerly/Bookerly-Italic.ttf",
      "Bookerly/Bookerly-BoldItalic.ttf", "Inter/Inter-Regular.ttf",       "Inter/Inter-Bold.ttf",
      "NotoSans/NotoSans-Regular.ttf",   "NotoSans/NotoSans-Bold.ttf",     "NotoSans/NotoSans-Italic.ttf",
      "NotoSans/NotoSans-BoldItalic.ttf", "Ubuntu/Ubuntu-Regular.ttf",     "Ubuntu/Ubuntu-Bold.ttf",
  };
  return fonts;
}

/// A 2-bit bitmap as text, top row first: '.' for 0, then '1', '2', '3'.
inline std::string picture(const EpdGlyph& glyph, const uint8_t* bits) {
  std::string out;
  for (int y = 0; y < glyph.height; ++y) {
    for (int x = 0; x < glyph.width; ++x) {
      const int p = y * glyph.width + x;
      out += ".123"[(bits[p >> 2] >> (6 - 2 * (p & 3))) & 3];
    }
    out += '\n';
  }
  return out;
}

}  // namespace ttf_test
```

- [ ] **Step 2: Write the failing test**

Create `test/ttf_rasterizer/TtfFaceTest.cpp`:

<!-- plan-file: test/ttf_rasterizer/TtfFaceTest.cpp -->
```cpp
// parseFace, glyphIndex and the metrics: what the engine reads from a font file, and the converter's sizes.
#include <gtest/gtest.h>

#include <cstdint>

#include "TtfBuilder.h"
#include "TtfFace.h"
#include "TtfTestUtil.h"

using namespace ttf;
using namespace ttf_test;

namespace {

// .notdef, 'A' and 'B': three glyphs of different widths.
Font threeGlyphs() {
  Font font;
  font.glyphs.resize(3);
  font.glyphs[0].contours = {rect(50, 0, 450, 700)};
  font.glyphs[1].contours = {rect(20, 0, 620, 700)};
  font.glyphs[1].advance = 640;
  font.glyphs[2].contours = {rect(60, 0, 560, 700), rect(160, 100, 460, 600)};
  font.glyphs[2].advance = 600;
  font.cmap = {{'A', 1}, {'B', 2}};
  return font;
}

FaceStatus parse(const Bytes& data, Face* face) {
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  return parseFace(bytes, face);
}

uint16_t lookup(const Bytes& data, const uint32_t codepoint) {
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  if (parseFace(bytes, &face) != FaceStatus::Ok) return 0xFFFF;
  return glyphIndex(bytes, face, codepoint);
}

}  // namespace

TEST(TtfFace, ParsesTheTablesAGlyphNeeds) {
  Font font = threeGlyphs();
  font.longLoca = true;
  const Bytes data = buildFont(font);
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(data, &face));
  EXPECT_EQ(1000, face.unitsPerEm);
  EXPECT_EQ(3, face.numGlyphs);
  EXPECT_EQ(3, face.numHMetrics);
  EXPECT_TRUE(face.longLoca);
  EXPECT_EQ(8, face.maxPoints);
  EXPECT_EQ(2, face.maxContours);
  EXPECT_EQ(20, face.xMin);
  EXPECT_EQ(0, face.yMin);
  EXPECT_EQ(620, face.xMax);
  EXPECT_EQ(700, face.yMax);
  EXPECT_EQ(800, face.ascender);
  EXPECT_EQ(-200, face.descender);
  EXPECT_EQ(1000, face.height);
  EXPECT_EQ(4, face.cmapFormat);
}

TEST(TtfFace, CffFontsAndCollectionsAreUnsupported) {
  Font font = threeGlyphs();
  font.sfntVersion = tagOf("OTTO");
  Face face;
  EXPECT_EQ(FaceStatus::Unsupported, parse(buildFont(font), &face));
  font.sfntVersion = tagOf("ttcf");
  EXPECT_EQ(FaceStatus::Unsupported, parse(buildFont(font), &face));
  font.sfntVersion = tagOf("true");  // Apple's TrueType tag is fine
  EXPECT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
}

TEST(TtfFace, ABundledCffSourceIsUnsupported) {
  const std::vector<uint8_t> data = readFont("OpenDyslexic/OpenDyslexic-Regular.otf");
  ASSERT_FALSE(data.empty());
  Face face;
  EXPECT_EQ(FaceStatus::Unsupported, parse(data, &face));
}

TEST(TtfFace, BrokenFilesAreMalformed) {
  Face face;
  EXPECT_EQ(FaceStatus::Malformed, parse(Bytes{}, &face));
  EXPECT_EQ(FaceStatus::Malformed, parse(Bytes{'w', 'O', 'F', 'F', 0, 0, 0, 0, 0, 0, 0, 0}, &face));
  const Bytes good = buildFont(threeGlyphs());
  EXPECT_EQ(FaceStatus::Malformed, parse(Bytes(good.begin(), good.begin() + 40), &face));  // directory cut

  Bytes data = buildFont(threeGlyphs());
  data[findRecord(data, "hmtx")] = 'x';  // the table is gone
  EXPECT_EQ(FaceStatus::Malformed, parse(data, &face));
}

TEST(TtfFace, ImpossibleHeaderValuesAreMalformed) {
  Face face;
  Font font = threeGlyphs();
  font.unitsPerEm = 8;
  EXPECT_EQ(FaceStatus::Malformed, parse(buildFont(font), &face));
  Bytes data = buildFont(threeGlyphs());
  set32(data, findRecord(data, "glyf") + 12, static_cast<uint32_t>(data.size()));  // glyf runs past the end
  EXPECT_EQ(FaceStatus::Malformed, parse(data, &face));
}

TEST(TtfFace, Format4MapsByDeltasAndByTheGlyphArray) {
  Font font = threeGlyphs();
  font.cmap = {{'A', 1}, {'B', 2}, {'C', 1}, {0x20AC, 2}};
  EXPECT_EQ(1, lookup(buildFont(font), 'A'));
  EXPECT_EQ(2, lookup(buildFont(font), 'B'));
  EXPECT_EQ(1, lookup(buildFont(font), 'C'));
  EXPECT_EQ(2, lookup(buildFont(font), 0x20AC));
  EXPECT_EQ(0, lookup(buildFont(font), 'D'));
  EXPECT_EQ(0, lookup(buildFont(font), 0x1F600));  // past the BMP: never in format 4
  font.cmapRangeOffsets = true;
  EXPECT_EQ(1, lookup(buildFont(font), 'A'));
  EXPECT_EQ(2, lookup(buildFont(font), 'B'));
  EXPECT_EQ(1, lookup(buildFont(font), 'C'));
  EXPECT_EQ(2, lookup(buildFont(font), 0x20AC));
  EXPECT_EQ(0, lookup(buildFont(font), 'D'));
}

TEST(TtfFace, Format12ReachesPastTheBmp) {
  Font font = threeGlyphs();
  font.cmapFormat12 = true;
  font.cmap = {{'A', 1}, {'B', 2}, {0x1F600, 1}, {0x1F601, 2}};
  const Bytes data = buildFont(font);
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(data, &face));
  EXPECT_EQ(12, face.cmapFormat);
  EXPECT_EQ(1, lookup(data, 'A'));
  EXPECT_EQ(2, lookup(data, 0x1F601));
  EXPECT_EQ(0, lookup(data, 0x1F602));
}

TEST(TtfFace, AGlyphPastNumGlyphsReadsAsNone) {
  Font font = threeGlyphs();
  font.cmap = {{'A', 1}, {'Z', 7}};
  EXPECT_EQ(1, lookup(buildFont(font), 'A'));
  EXPECT_EQ(0, lookup(buildFont(font), 'Z'));
}

TEST(TtfFace, TheCmapIsChosenAsFreeTypeChoosesIt) {
  // From the last record backwards: a UCS-4 map first, else any Unicode map; formats it cannot read are
  // passed over, and Mac Roman is not Unicode.
  const Bytes mapsA1 = cmapFormat4({{'A', 1}}, false);
  const Bytes mapsA2 = cmapFormat4({{'A', 2}}, false);
  const Bytes format12A2 = cmapFormat12({{'A', 2}});
  Bytes format6(10, 0);
  set16(format6, 0, 6);
  struct Case {
    const char* what;
    std::vector<std::tuple<uint16_t, uint16_t, Bytes>> subtables;
    int expected;  // the glyph for 'A', or -1 for "no usable map"
  };
  const Case cases[] = {
      {"the last Unicode BMP map wins", {{0, 3, mapsA1}, {3, 1, mapsA2}}, 2},
      {"a UCS-4 map wins over a later BMP map", {{3, 10, format12A2}, {3, 1, mapsA1}}, 2},
      {"(0,4) is UCS-4 too", {{0, 4, format12A2}, {3, 1, mapsA1}}, 2},
      {"a format it cannot read is passed over", {{3, 1, mapsA2}, {3, 10, format6}}, 2},
      {"variation sequences are not a map", {{3, 1, mapsA1}, {0, 5, mapsA2}}, 1},
      {"Mac Roman alone is no Unicode map", {{1, 0, mapsA1}}, -1},
  };
  for (const Case& c : cases) {
    Font font = threeGlyphs();
    font.extraTables = {{"cmap", cmapTable(c.subtables)}};
    const Bytes data = buildFont(font);
    Face face;
    if (c.expected < 0) {
      EXPECT_EQ(FaceStatus::Unsupported, parse(data, &face)) << c.what;
    } else {
      EXPECT_EQ(c.expected, lookup(data, 'A')) << c.what;
    }
  }
}

TEST(TtfFace, HmtxIsReadAsFreeTypeReadsIt) {
  Font font = threeGlyphs();
  font.numHMetrics = 2;  // glyph 2 shares glyph 1's advance and keeps its own left side bearing
  font.glyphs[2].lsbIsXMin = false;
  font.glyphs[2].lsb = 33;
  const Bytes data = buildFont(font);
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  uint16_t advance = 0;
  int16_t lsb = 0;
  horizontalMetrics(bytes, face, 1, &advance, &lsb);
  EXPECT_EQ(640, advance);
  EXPECT_EQ(20, lsb);
  horizontalMetrics(bytes, face, 2, &advance, &lsb);
  EXPECT_EQ(640, advance);
  EXPECT_EQ(33, lsb);
  // Past the table, both read 0 rather than anything outside it.
  face.hmtxLength = 8;
  horizontalMetrics(bytes, face, 2, &advance, &lsb);
  EXPECT_EQ(640, advance);
  EXPECT_EQ(0, lsb);
  face.hmtxLength = 2;
  horizontalMetrics(bytes, face, 1, &advance, &lsb);
  EXPECT_EQ(0, advance);
  EXPECT_FALSE(bytes.outOfRange());
}

TEST(TtfFace, PointSizesBecomeFreeTypesPixelsPerEm) {
  EXPECT_EQ(1600u, ppemFromPoints(12));
  EXPECT_EQ(1867u, ppemFromPoints(14));
  EXPECT_EQ(2400u, ppemFromPoints(18));
}

TEST(TtfFace, AdvancesAreTheConvertersTwelveFour) {
  const Bytes data = buildFont(threeGlyphs());
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  EXPECT_EQ(122356, unitsToPixels(face, 1867));
  EXPECT_EQ(233, advance12_4(bytes, face, 0, 1867));  // 500 units: 955906 in 16.16, 14.59 px
  EXPECT_EQ(299, advance12_4(bytes, face, 1, 1867));  // 640 units: 1223560 in 16.16
  EXPECT_EQ(80, advance12_4(bytes, face, 0, 640));    // 500 units at 10 px: 4.99998 px rounds to 5
}

TEST(TtfFace, LineMetricsAreTheConverters) {
  Font font = threeGlyphs();
  font.ascender = 1057;  // Bookerly's hhea
  font.descender = -261;
  const Bytes data = buildFont(font);
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(data, &face));
  const LineMetrics m = lineMetrics(face, 1867);  // FreeType: height 2432, ascender 1984, descender -512
  EXPECT_EQ(38, m.advanceY);
  EXPECT_EQ(31, m.ascender);
  EXPECT_EQ(-8, m.descender);
}

TEST(TtfFace, Os2MetricsAreUsedWhereFreeTypeUsesThem) {
  Font font = threeGlyphs();
  font.os2 = true;
  font.typoAscender = 900;
  font.typoDescender = -300;
  font.typoLineGap = 100;
  font.winAscent = 1100;
  font.winDescent = 400;
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  EXPECT_EQ(800, face.ascender);  // hhea, while OS/2 does not ask for its typo metrics

  font.os2Selection = 0x80;  // USE_TYPO_METRICS
  ASSERT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  EXPECT_EQ(900, face.ascender);
  EXPECT_EQ(-300, face.descender);
  EXPECT_EQ(1300, face.height);

  font.os2Selection = 0;
  font.ascender = 0;  // hhea empty: OS/2's typo metrics
  font.descender = 0;
  ASSERT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  EXPECT_EQ(900, face.ascender);

  font.typoAscender = 0;  // and those empty too: the win metrics
  font.typoDescender = 0;
  ASSERT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  EXPECT_EQ(1100, face.ascender);
  EXPECT_EQ(-400, face.descender);
  EXPECT_EQ(1500, face.height);
}
```

Replace `test/ttf_rasterizer/CMakeLists.txt` with:

<!-- plan-file: test/ttf_rasterizer/CMakeLists.txt -->
```cmake
# The TTF engine (lib/EpdFont/ttf): FontBytes, face parsing, the integer TrueType loader and the vendored
# ftgrays, checked against FreeType itself over every bundled TTF.

add_executable(TtfRasterizerTest
  TtfGraysTest.cpp
  FontBytesTest.cpp
  TtfFixedTest.cpp
  TtfFaceTest.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFace.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/third_party/freetype/ftgrays.c
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(TtfRasterizerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/shims
)

target_compile_definitions(TtfRasterizerTest PRIVATE
  TTF_FONT_DIR="${REPO_ROOT}/lib/EpdFont/builtinFonts/source"
)

target_link_libraries(TtfRasterizerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

# Spec 9.3: the engine runs under AddressSanitizer and UBSan (CI's Linux GCC). Where ASan cannot link
# (MinGW), UBSan still traps on undefined behaviour without its runtime. FreeType is not instrumented.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address,undefined)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_ASAN)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=undefined -fsanitize-undefined-trap-on-error")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_UBSAN_TRAP)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(TTF_TESTS_HAVE_ASAN)
  set(TTF_SANITIZERS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
elseif(TTF_TESTS_HAVE_UBSAN_TRAP)
  set(TTF_SANITIZERS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
endif()
target_compile_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
target_link_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
message(STATUS "TtfRasterizerTest sanitizers: ${TTF_SANITIZERS}")

gtest_discover_tests(TtfRasterizerTest DISCOVERY_TIMEOUT 60)
```

- [ ] **Step 3: Run it to see it fail**

Run: `cmake -S test -B build/test`
Expected: `CMake Error ... Cannot find source file: .../lib/EpdFont/ttf/TtfFace.cpp`

- [ ] **Step 4: Write the code**

Create `lib/EpdFont/ttf/TtfFace.h`:

<!-- plan-file: lib/EpdFont/ttf/TtfFace.h -->
```cpp
#pragma once

#include <cstdint>

#include "FontBytes.h"

/// One TrueType font file as the TTF engine reads it: where its tables are, its metrics, and codepoint to
/// glyph lookup. A Face holds offsets, never pointers, so it stays valid when the file's mapping moves; the
/// FontBytes is passed to every call.
///
/// Sizes and metrics reproduce FreeType 2.13 as the PC converter (fontconvert_sdcard.py) drives it:
/// FT_Set_Char_Size at 150 dpi, advances from linearHoriAdvance, line metrics from the size metrics. A TTF
/// at 14 pt therefore lays out like a .cpfont converted at 14 pt (spec 5.2). Integer only.
namespace ttf {

enum class FaceStatus : uint8_t {
  Ok,
  Unsupported,  ///< a CFF font ('OTTO'), a collection ('ttcf'), or no Unicode cmap of format 4 or 12
  Malformed,    ///< not a font, or a required table is missing, outside the file or impossible
  Transient,    ///< an SD read failed (Stage 2c): try again later
};

struct Face {
  uint32_t glyf = 0;
  uint32_t glyfLength = 0;
  uint32_t loca = 0;
  uint32_t locaLength = 0;
  uint32_t hmtx = 0;
  uint32_t hmtxLength = 0;
  uint32_t cmapSubtable = 0;  ///< the subtable glyphIndex() reads
  uint32_t cmapEnd = 0;       ///< end of the cmap table: the subtable's reads stay below it
  uint16_t unitsPerEm = 0;
  uint16_t numGlyphs = 0;
  uint16_t numHMetrics = 0;
  uint16_t maxPoints = 0;    ///< the larger of maxp's maxPoints and maxCompositePoints
  uint16_t maxContours = 0;  ///< the larger of maxp's maxContours and maxCompositeContours
  int16_t xMin = 0;          ///< head: the box around every glyph, in font units
  int16_t yMin = 0;
  int16_t xMax = 0;
  int16_t yMax = 0;
  int16_t ascender = 0;  ///< FreeType's face metrics: hhea, or OS/2's when it asks (USE_TYPO_METRICS)
  int16_t descender = 0;
  int16_t height = 0;  ///< ascender - descender + line gap
  uint8_t cmapFormat = 0;
  bool longLoca = false;
};

/// Reads the table directory and the tables every glyph needs. Read errors set the FontBytes flags as well.
FaceStatus parseFace(FontBytes& bytes, Face* face);

/// The glyph for `codepoint`, or 0 (the font has none, or the cmap is broken: see bytes.outOfRange()).
uint16_t glyphIndex(FontBytes& bytes, const Face& face, uint32_t codepoint);

/// 26.6 pixels per em for a point size at the converter's 150 dpi, as FreeType derives it: 14 pt -> 1867.
constexpr uint32_t ppemFromPoints(const uint32_t points) { return (points * 64 * 150 + 36) / 72; }

/// The 16.16 factor from font units to 26.6 pixels at `ppem26_6` (FreeType's x_scale, equal to y_scale).
int32_t unitsToPixels(const Face& face, uint32_t ppem26_6);

/// hmtx for `glyph`, read as FreeType reads it: glyphs past numHMetrics share the last advance, and an entry
/// past the end of the table reads 0.
void horizontalMetrics(FontBytes& bytes, const Face& face, uint16_t glyph, uint16_t* advance, int16_t* lsb);

/// The glyph's advance in 12.4 pixels, as the converter derives advanceX from FreeType's linearHoriAdvance.
uint16_t advance12_4(FontBytes& bytes, const Face& face, uint16_t glyph, uint32_t ppem26_6);

/// Line metrics in whole pixels, as fontconvert_sdcard.py writes them: advanceY from the rounded height,
/// the ascender rounded up, the descender rounded down.
struct LineMetrics {
  int32_t advanceY = 0;
  int32_t ascender = 0;
  int32_t descender = 0;
};
LineMetrics lineMetrics(const Face& face, uint32_t ppem26_6);

}  // namespace ttf
```

Create `lib/EpdFont/ttf/TtfFace.cpp`:

<!-- plan-file: lib/EpdFont/ttf/TtfFace.cpp -->
```cpp
#include "TtfFace.h"

#include "TtfFixed.h"

namespace ttf {

namespace {

constexpr uint32_t tag(const char (&name)[5]) {
  return static_cast<uint32_t>(static_cast<uint8_t>(name[0])) << 24 |
         static_cast<uint32_t>(static_cast<uint8_t>(name[1])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(name[2])) << 8 | static_cast<uint8_t>(name[3]);
}

constexpr uint32_t kVersionTrueType = 0x00010000;
constexpr uint32_t kVersionApple = tag("true");
constexpr uint32_t kVersionCff = tag("OTTO");
constexpr uint32_t kVersionCollection = tag("ttcf");
constexpr uint32_t kMaxpVersion1 = 0x00010000;  // 0.5 is the CFF form, without the outline limits

struct Table {
  uint32_t offset = 0;
  uint32_t length = 0;
};

// The table `wanted`, when the directory lists it, it lies inside the file and it holds `minLength` bytes.
bool findTable(FontBytes& bytes, const uint16_t numTables, const uint32_t wanted, const uint32_t minLength,
               Table* out) {
  for (uint32_t i = 0; i < numTables; ++i) {
    const uint32_t record = 12 + 16 * i;
    if (bytes.u32(record) != wanted) continue;
    out->offset = bytes.u32(record + 8);
    out->length = bytes.u32(record + 12);
    return static_cast<uint64_t>(out->offset) + out->length <= bytes.size() && out->length >= minLength;
  }
  return false;
}

// FreeType reads OS/2 field groups by version and treats a table too short for its version as missing.
uint32_t os2Length(const uint16_t version) {
  if (version >= 5) return 100;
  if (version >= 2) return 96;
  if (version >= 1) return 86;
  return 78;
}

FaceStatus readFailure(const FontBytes& bytes, const FaceStatus otherwise) {
  return bytes.ioError() ? FaceStatus::Transient : otherwise;
}

// FreeType's choice of Unicode map (find_unicode_charmap): from the last record backwards, first a UCS-4 map,
// then any Unicode map. Only formats 4 and 12 are read here, so other subtables are passed over.
bool chooseCmap(FontBytes& bytes, const Table& cmap, Face* face) {
  const uint16_t records = bytes.u16(cmap.offset + 2);
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t i = records; i-- > 0;) {
      const uint32_t record = 4 + 8 * i;
      if (record + 8 > cmap.length) continue;  // FreeType drops records past the table
      const uint16_t platform = bytes.u16(cmap.offset + record);
      const uint16_t encoding = bytes.u16(cmap.offset + record + 2);
      const uint32_t offset = bytes.u32(cmap.offset + record + 4);
      const bool ucs4 = (platform == 3 && encoding == 10) || (platform == 0 && encoding == 4);
      const bool unicode = platform == 0 || platform == 2 || (platform == 3 && (encoding == 1 || encoding == 10));
      if (pass == 0 ? !ucs4 : !unicode) continue;
      if (platform == 0 && encoding == 5) continue;  // variation sequences, not a character map
      if (offset == 0 || offset > cmap.length - 2) continue;
      const uint16_t format = bytes.u16(cmap.offset + offset);
      if (format != 4 && format != 12) continue;
      face->cmapSubtable = cmap.offset + offset;
      face->cmapFormat = static_cast<uint8_t>(format);
      return true;
    }
  }
  return false;
}

uint32_t format12Glyph(FontBytes& bytes, const Face& face, const uint32_t codepoint) {
  const uint32_t subtable = face.cmapSubtable;
  const uint32_t room = face.cmapEnd > subtable + 16 ? (face.cmapEnd - subtable - 16) / 12 : 0;
  const uint32_t groups = bytes.u32(subtable + 12);
  uint32_t lo = 0;
  uint32_t hi = groups < room ? groups : room;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const uint32_t group = subtable + 16 + 12 * mid;
    const uint32_t start = bytes.u32(group);
    if (codepoint < start) {
      hi = mid;
    } else if (codepoint > bytes.u32(group + 4)) {
      lo = mid + 1;
    } else {
      const uint32_t first = bytes.u32(group + 8);
      return first > 0xFFFFFFFFu - (codepoint - start) ? 0 : first + (codepoint - start);
    }
  }
  return 0;
}

uint32_t format4Glyph(FontBytes& bytes, const Face& face, const uint32_t codepoint) {
  if (codepoint > 0xFFFF) return 0;
  const uint32_t subtable = face.cmapSubtable;
  const uint32_t segX2 = bytes.u16(subtable + 6) & ~1u;
  const uint32_t ends = subtable + 14;
  const uint32_t starts = ends + segX2 + 2;  // past the reserved pad
  const uint32_t deltas = starts + segX2;
  const uint32_t rangeOffsets = deltas + segX2;
  uint32_t lo = 0;
  uint32_t hi = segX2 / 2;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    const uint32_t start = bytes.u16(starts + 2 * mid);
    if (codepoint < start) {
      hi = mid;
    } else if (codepoint > bytes.u16(ends + 2 * mid)) {
      lo = mid + 1;
    } else {
      const int32_t delta = bytes.i16(deltas + 2 * mid);
      const uint32_t rangePos = rangeOffsets + 2 * mid;
      const uint16_t rangeOffset = bytes.u16(rangePos);
      if (rangeOffset == 0xFFFF) return 0;  // FreeType reads this broken marker as "no glyph"
      if (rangeOffset == 0) return static_cast<uint32_t>(static_cast<int32_t>(codepoint) + delta) & 0xFFFF;
      const uint16_t glyph = bytes.u16(rangePos + rangeOffset + 2 * (codepoint - start));
      return glyph == 0 ? 0 : static_cast<uint32_t>(glyph + delta) & 0xFFFF;
    }
  }
  return 0;
}

}  // namespace

FaceStatus parseFace(FontBytes& bytes, Face* face) {
  *face = Face{};
  const uint32_t version = bytes.u32(0);
  if (version == kVersionCff || version == kVersionCollection) return FaceStatus::Unsupported;
  if (version != kVersionTrueType && version != kVersionApple) return readFailure(bytes, FaceStatus::Malformed);
  const uint16_t numTables = bytes.u16(4);
  if (12 + 16 * static_cast<uint64_t>(numTables) > bytes.size()) return readFailure(bytes, FaceStatus::Malformed);

  Table head, hhea, maxp, cmap, loca, glyf, hmtx;
  if (!findTable(bytes, numTables, tag("head"), 54, &head) || !findTable(bytes, numTables, tag("hhea"), 36, &hhea) ||
      !findTable(bytes, numTables, tag("maxp"), 32, &maxp) || !findTable(bytes, numTables, tag("cmap"), 4, &cmap) ||
      !findTable(bytes, numTables, tag("loca"), 0, &loca) || !findTable(bytes, numTables, tag("glyf"), 0, &glyf) ||
      !findTable(bytes, numTables, tag("hmtx"), 0, &hmtx)) {
    return readFailure(bytes, FaceStatus::Malformed);
  }

  face->unitsPerEm = bytes.u16(head.offset + 18);
  const int16_t locaFormat = bytes.i16(head.offset + 50);
  face->numGlyphs = bytes.u16(maxp.offset + 4);
  face->numHMetrics = bytes.u16(hhea.offset + 34);
  if (face->unitsPerEm < 16 || face->unitsPerEm > 16384 || (locaFormat != 0 && locaFormat != 1) ||
      bytes.u32(maxp.offset) != kMaxpVersion1 || face->numGlyphs == 0 || face->numHMetrics == 0) {
    return readFailure(bytes, FaceStatus::Malformed);
  }
  face->longLoca = locaFormat == 1;
  face->xMin = bytes.i16(head.offset + 36);
  face->yMin = bytes.i16(head.offset + 38);
  face->xMax = bytes.i16(head.offset + 40);
  face->yMax = bytes.i16(head.offset + 42);
  const uint16_t simplePoints = bytes.u16(maxp.offset + 6);
  const uint16_t compositePoints = bytes.u16(maxp.offset + 10);
  const uint16_t simpleContours = bytes.u16(maxp.offset + 8);
  const uint16_t compositeContours = bytes.u16(maxp.offset + 12);
  face->maxPoints = simplePoints > compositePoints ? simplePoints : compositePoints;
  face->maxContours = simpleContours > compositeContours ? simpleContours : compositeContours;
  face->glyf = glyf.offset;
  face->glyfLength = glyf.length;
  face->loca = loca.offset;
  face->locaLength = loca.length;
  face->hmtx = hmtx.offset;
  face->hmtxLength = hmtx.length;

  // FreeType's face metrics (sfnt_load_face): OS/2's typo metrics when it sets USE_TYPO_METRICS, else hhea's;
  // when hhea's are both 0, OS/2's typo metrics, or its win metrics when those are 0 too.
  Table os2;
  bool haveOs2 = findTable(bytes, numTables, tag("OS/2"), 78, &os2);
  if (haveOs2) haveOs2 = os2.length >= os2Length(bytes.u16(os2.offset));
  const int16_t typoAscender = haveOs2 ? bytes.i16(os2.offset + 68) : 0;
  const int16_t typoDescender = haveOs2 ? bytes.i16(os2.offset + 70) : 0;
  const int16_t typoLineGap = haveOs2 ? bytes.i16(os2.offset + 72) : 0;
  int32_t ascender = bytes.i16(hhea.offset + 4);
  int32_t descender = bytes.i16(hhea.offset + 6);
  int32_t height = ascender - descender + bytes.i16(hhea.offset + 8);
  const bool useTypo = haveOs2 && (bytes.u16(os2.offset + 62) & 0x80) != 0;
  if (useTypo || (ascender == 0 && descender == 0 && haveOs2 && (typoAscender != 0 || typoDescender != 0))) {
    ascender = typoAscender;
    descender = typoDescender;
    height = ascender - descender + typoLineGap;
  } else if (ascender == 0 && descender == 0 && haveOs2) {
    ascender = bytes.u16(os2.offset + 74);
    descender = -static_cast<int32_t>(bytes.u16(os2.offset + 76));
    height = ascender - descender;
  }
  face->ascender = static_cast<int16_t>(ascender);
  face->descender = static_cast<int16_t>(descender);
  face->height = static_cast<int16_t>(height);

  face->cmapEnd = cmap.offset + cmap.length;
  if (!chooseCmap(bytes, cmap, face)) return readFailure(bytes, FaceStatus::Unsupported);
  if (bytes.ioError()) return FaceStatus::Transient;
  return bytes.outOfRange() ? FaceStatus::Malformed : FaceStatus::Ok;
}

uint16_t glyphIndex(FontBytes& bytes, const Face& face, const uint32_t codepoint) {
  const uint32_t glyph =
      face.cmapFormat == 12 ? format12Glyph(bytes, face, codepoint) : format4Glyph(bytes, face, codepoint);
  return glyph < face.numGlyphs ? static_cast<uint16_t>(glyph) : 0;
}

int32_t unitsToPixels(const Face& face, const uint32_t ppem26_6) {
  return divFix(static_cast<int32_t>(ppem26_6), face.unitsPerEm);
}

void horizontalMetrics(FontBytes& bytes, const Face& face, const uint16_t glyph, uint16_t* advance, int16_t* lsb) {
  *advance = 0;
  *lsb = 0;
  const uint32_t metrics = face.numHMetrics;
  if (glyph < metrics) {
    const uint32_t pos = 4 * static_cast<uint32_t>(glyph);
    if (pos + 4 > face.hmtxLength) return;
    *advance = bytes.u16(face.hmtx + pos);
    *lsb = bytes.i16(face.hmtx + pos + 2);
    return;
  }
  const uint32_t last = 4 * (metrics - 1);
  if (last + 2 > face.hmtxLength) return;
  *advance = bytes.u16(face.hmtx + last);
  const uint32_t pos = 4 * metrics + 2 * (glyph - metrics);
  if (pos + 2 <= face.hmtxLength) *lsb = bytes.i16(face.hmtx + pos);
}

uint16_t advance12_4(FontBytes& bytes, const Face& face, const uint16_t glyph, const uint32_t ppem26_6) {
  uint16_t advance = 0;
  int16_t lsb = 0;
  horizontalMetrics(bytes, face, glyph, &advance, &lsb);
  // FreeType: linearHoriAdvance = FT_MulDiv(advance, x_scale, 64) in 16.16; the converter rounds that to 12.4.
  const int32_t linear = mulDiv(advance, unitsToPixels(face, ppem26_6), 64);
  return static_cast<uint16_t>((linear + (1 << 11)) >> 12);
}

LineMetrics lineMetrics(const Face& face, const uint32_t ppem26_6) {
  const int32_t scale = unitsToPixels(face, ppem26_6);
  // FreeType's size metrics (ft_recompute_scaled_metrics): height rounded, ascender up, descender down.
  const int32_t height = (mulFix(face.height, scale) + 32) & -64;
  const int32_t ascender = (mulFix(face.ascender, scale) + 63) & -64;
  const int32_t descender = mulFix(face.descender, scale) & -64;
  return {height / 64, ascender / 64, descender / 64};
}

}  // namespace ttf
```

- [ ] **Step 5: Run the tests**

```bash
cmake -S test -B build/test
cmake --build build/test --target TtfRasterizerTest -j 8
./build/test/ttf_rasterizer/TtfRasterizerTest.exe
```

Expected: `[  PASSED  ] 27 tests.`

- [ ] **Step 6: Format and commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe -i lib/EpdFont/ttf/TtfFace.h lib/EpdFont/ttf/TtfFace.cpp test/ttf_rasterizer/TtfBuilder.h test/ttf_rasterizer/TtfTestUtil.h test/ttf_rasterizer/TtfFaceTest.cpp
git add lib/EpdFont/ttf/TtfFace.h lib/EpdFont/ttf/TtfFace.cpp test/ttf_rasterizer
git commit -m "feat(ttf): parse a face as FreeType does: tables, Unicode cmap, hmtx and the converter's sizes"
```

---

### Task 4: `ttf::Rasterizer`: the integer loader and the span packer

**Files:**
- Create: `lib/EpdFont/ttf/TtfRasterizer.h`, `lib/EpdFont/ttf/TtfRasterizer.cpp`
- Create: `test/ttf_rasterizer/TtfRasterizerTest.cpp`
- Modify: `test/ttf_rasterizer/CMakeLists.txt`

**Interfaces:**
- Consumes: `FontBytes` and `ttf::mulFix/mulFixWide` (Task 2); `ttf::Face`, `parseFace`, `horizontalMetrics`,
  `advance12_4` and `unitsToPixels` (Task 3); `ttf_grays_raster` through `TtfGrays.h` (Task 1);
  `EpdGlyph` and `glyphDataBytes` (`lib/EpdFont/EpdFontData.h`); `makeUniqueNoThrow` (`lib/Memory/Memory.h`).
- Produces (`namespace ttf`):
  - `enum class Status { Ok, Unsupported, Malformed, Transient }`, `constexpr bool isPermanent(Status)` and
    `const char* statusName(Status)`.
  - The constants `kMaxScratchPoints = 512`, `kMaxScratchContours = 256`, `kMaxOutputBytes = 4096`,
    `kPoolBytesFlash = 4096`, `kPoolBytesSd = 8192` and `kMaxPpem26_6 = 255 << 6`.
  - `struct ScratchPlan { uint16_t points; uint16_t contours; uint32_t outputBytes; uint32_t poolBytes; }`,
    `ScratchPlan planScratch(const Face&, uint32_t maxPpem26_6, uint32_t poolBytes)` and
    `ScratchPlan mergePlans(const ScratchPlan&, const ScratchPlan&)`.
  - `class Rasterizer`: `bool allocate(const ScratchPlan&)`, `release()`, `allocated()`, `scratchBytes()`,
    `plan()` and `uint8_t* output()`.
    `Status load(FontBytes&, const Face&, uint16_t glyph, uint32_t ppem26_6, EpdGlyph* metrics)` fills
    width, height, left, top, advanceX (12.4) and dataLength; dataOffset is 0.
    `Status rasterize(uint8_t* dst)` writes all of dataLength.
    `Status render(FontBytes&, const Face&, uint16_t glyph, uint32_t ppem26_6, EpdGlyph* metrics)` draws
    into `output()`.

- [ ] **Step 1: Write the failing test**

At 1,000 units per em and a size of 1,000 in 26.6, one font unit is exactly 1/64 px, so the test's outlines
are written in 26.6 and its expected bitmaps can be worked out by hand.

Create `test/ttf_rasterizer/TtfRasterizerTest.cpp`:

<!-- plan-file: test/ttf_rasterizer/TtfRasterizerTest.cpp -->
```cpp
// The integer TrueType loader and the span packer, on fonts built in memory. At 1000 units per em and a size of
// 1000 in 26.6 (15.6 px), one font unit is exactly 1/64 px, so the outlines below are written in 26.6.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "TtfBuilder.h"
#include "TtfRasterizer.h"
#include "TtfTestUtil.h"

using namespace ttf;
using namespace ttf_test;

namespace {

constexpr uint32_t kUnitPpem = 1000;  // one font unit = 1/64 px

ScratchPlan roomyPlan() {
  ScratchPlan plan;
  plan.points = 64;
  plan.contours = 16;
  plan.outputBytes = 1024;
  plan.poolBytes = kPoolBytesFlash;
  return plan;
}

class TtfRasterizerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    font.glyphs.resize(1);
    font.glyphs[0].contours = {rect(0, 0, 64, 64)};
  }

  // Builds the font, then loads and draws `glyph`; the picture is empty when the load fails.
  Status draw(const uint16_t glyph, const uint32_t ppem26_6 = kUnitPpem, const ScratchPlan& plan = roomyPlan()) {
    data = buildFont(font);
    bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
    if (parseFace(bytes, &face) != FaceStatus::Ok) return Status::Malformed;
    if (!rasterizer.allocated() || rasterizer.plan().points != plan.points ||
        rasterizer.plan().outputBytes != plan.outputBytes || rasterizer.plan().contours != plan.contours) {
      if (!rasterizer.allocate(plan)) return Status::Transient;
    }
    metrics = EpdGlyph{};
    const Status status = rasterizer.render(bytes, face, glyph, ppem26_6, &metrics);
    image = status == Status::Ok ? picture(metrics, rasterizer.output()) : std::string();
    return status;
  }

  uint16_t add(const Glyph& glyph) {
    font.glyphs.push_back(glyph);
    return static_cast<uint16_t>(font.glyphs.size() - 1);
  }

  Font font;
  Bytes data;
  FontBytes bytes;
  Face face;
  Rasterizer rasterizer;
  EpdGlyph metrics{};
  std::string image;
};

Glyph outline(std::vector<std::vector<Point>> contours, const uint16_t advance = 500) {
  Glyph glyph;
  glyph.contours = std::move(contours);
  glyph.advance = advance;
  return glyph;
}

Glyph compound(std::vector<Component> components) {
  Glyph glyph;
  glyph.components = std::move(components);
  return glyph;
}

Glyph raw(Bytes record) {
  Glyph glyph;
  glyph.raw = std::move(record);
  glyph.lsbIsXMin = false;
  return glyph;
}

// A simple-glyph header: contour count and a box.
Bytes header(const int16_t contours) {
  Bytes out;
  put16(out, static_cast<uint16_t>(contours));
  for (int i = 0; i < 4; ++i) put16(out, 0);
  return out;
}

}  // namespace

TEST_F(TtfRasterizerTest, ASquareFillsItsBox) {
  const uint16_t g = add(outline({rect(64, 0, 384, 320)}));  // x 1..6 px, y 0..5 px
  ASSERT_EQ(Status::Ok, draw(g));
  EXPECT_EQ(5, metrics.width);
  EXPECT_EQ(5, metrics.height);
  EXPECT_EQ(1, metrics.left);
  EXPECT_EQ(5, metrics.top);
  EXPECT_EQ(7, metrics.dataLength);
  EXPECT_EQ(0u, metrics.dataOffset);
  EXPECT_EQ("33333\n33333\n33333\n33333\n33333\n", image);
}

TEST_F(TtfRasterizerTest, RowsRunFromTheTop) {
  const uint16_t g = add(outline({rect(0, 0, 192, 64), rect(0, 64, 64, 192)}));  // an L
  ASSERT_EQ(Status::Ok, draw(g));
  EXPECT_EQ("3..\n3..\n333\n", image);
  EXPECT_EQ(3, metrics.top);
}

TEST_F(TtfRasterizerTest, CoverageQuantisesWithTheConvertersThresholds) {
  // A pixel 8, 20, 36 and 52 of 64 covered: coverage 32, 80, 144, 208 -> nibbles 2, 5, 9, 13.
  const int16_t widths[] = {8, 20, 36, 52};
  const char* expected[] = {".\n", "1\n", "2\n", "3\n"};
  for (int i = 0; i < 4; ++i) {
    font.glyphs.resize(1);
    const uint16_t g = add(outline({rect(0, 0, widths[i], 64)}));
    ASSERT_EQ(Status::Ok, draw(g)) << i;
    EXPECT_EQ(expected[i], image) << i;
  }
}

TEST_F(TtfRasterizerTest, AdvancesComeFromHmtx) {
  const uint16_t g = add(outline({rect(64, 0, 384, 320)}, 500));
  ASSERT_EQ(Status::Ok, draw(g, 640));  // 10 px: 500 units are 4.99998 px
  EXPECT_EQ(80, metrics.advanceX);
}

TEST_F(TtfRasterizerTest, AGlyphWithNoOutlineLoadsEmpty) {
  Glyph space;
  space.advance = 250;
  const uint16_t g = add(space);
  ASSERT_EQ(Status::Ok, draw(g));
  EXPECT_EQ(0, metrics.width);
  EXPECT_EQ(0, metrics.height);
  EXPECT_EQ(0, metrics.dataLength);
  EXPECT_EQ(63, metrics.advanceX);  // 250 units = 3.906 px = 62.5 in 12.4, rounded up
  uint8_t canary[4] = {0xEE, 0xEE, 0xEE, 0xEE};
  EXPECT_EQ(Status::Ok, rasterizer.rasterize(canary));
  EXPECT_EQ(0xEE, canary[0]);
}

TEST_F(TtfRasterizerTest, TheOutlineMovesSoTheLeftSideBearingStartsAtTheOrigin) {
  // FreeType places a glyph by its hmtx left side bearing, not by its outline: xMin - lsb is taken off.
  Glyph glyph = outline({rect(64, 0, 128, 64)});
  glyph.lsbIsXMin = false;
  glyph.lsb = 0;
  const uint16_t shifted = add(glyph);
  ASSERT_EQ(Status::Ok, draw(shifted));
  EXPECT_EQ(0, metrics.left);
  glyph.lsb = 64;
  const uint16_t inPlace = add(glyph);
  ASSERT_EQ(Status::Ok, draw(inPlace));
  EXPECT_EQ(1, metrics.left);
}

TEST_F(TtfRasterizerTest, ComponentsAreOffset) {
  const uint16_t dot = add(outline({rect(0, 0, 64, 64)}));
  const uint16_t g = add(compound({{dot, 128, 64}, {dot, 0, 0}}));
  ASSERT_EQ(Status::Ok, draw(g));
  EXPECT_EQ(0, metrics.left);
  EXPECT_EQ(2, metrics.top);
  EXPECT_EQ("..3\n3..\n", image);
}

TEST_F(TtfRasterizerTest, ComponentsAreScaled) {
  const uint16_t square = add(outline({rect(0, 0, 128, 128)}));  // 2 x 2 px
  const uint16_t half = add(compound({{square, 0, 0, {0x2000}}}));
  ASSERT_EQ(Status::Ok, draw(half));
  EXPECT_EQ("3\n", image);
  const uint16_t flat = add(compound({{square, 0, 0, {0x4000, 0x2000}}}));  // x 1.0, y 0.5
  ASSERT_EQ(Status::Ok, draw(flat));
  EXPECT_EQ("33\n", image);
}

TEST_F(TtfRasterizerTest, TheTwoByTwoTransformTakesItsEntriesInFileOrder) {
  // a, b, c, d in the file: x' = a*x + c*y, y' = b*x + d*y. This one turns a quarter left: (x, y) -> (-y, x).
  const uint16_t bar = add(outline({rect(0, 0, 192, 64)}));  // 3 x 1 px
  const uint16_t turned = add(compound({{bar, 0, 0, {0, 0x4000, -0x4000, 0}}}));
  ASSERT_EQ(Status::Ok, draw(turned));
  EXPECT_EQ(-1, metrics.left);
  EXPECT_EQ(3, metrics.top);
  EXPECT_EQ("3\n3\n3\n", image);
}

TEST_F(TtfRasterizerTest, UseMyMetricsTakesTheComponentsLeftSideBearing) {
  Glyph base = outline({rect(64, 0, 128, 64)});
  base.lsbIsXMin = false;
  base.lsb = 0;  // FreeType shifts this glyph 1 px left when it stands alone
  const uint16_t dot = add(base);
  const uint16_t plain = add(compound({{dot, 0, 0}}));
  ASSERT_EQ(Status::Ok, draw(plain));
  EXPECT_EQ(1, metrics.left);  // the compound's own lsb equals its xMin: no shift
  const uint16_t mine = add(compound({{dot, 0, 0, {}, 0x0200}}));
  ASSERT_EQ(Status::Ok, draw(mine));
  EXPECT_EQ(0, metrics.left);
}

TEST_F(TtfRasterizerTest, PointMatchedAndScaledOffsetComponentsAreUnsupported) {
  const uint16_t dot = add(outline({rect(0, 0, 64, 64)}));
  Bytes record = header(-1);
  put16(record, 0x0000);  // byte args that are point numbers: ARGS_ARE_XY_VALUES clear
  put16(record, dot);
  record.push_back(0);
  record.push_back(0);
  const uint16_t matched = add(raw(record));
  EXPECT_EQ(Status::Unsupported, draw(matched));
  const uint16_t scaledOffset = add(compound({{dot, 64, 0, {0x4000}, 0x0800}}));
  EXPECT_EQ(Status::Unsupported, draw(scaledOffset));
}

TEST_F(TtfRasterizerTest, ACompoundThatContainsItselfStopsAtTheDepthLimit) {
  const uint16_t self = static_cast<uint16_t>(font.glyphs.size());
  add(compound({{self, 0, 0}}));
  EXPECT_EQ(Status::Unsupported, draw(self));
}

TEST_F(TtfRasterizerTest, TooManyComponentsAreUnsupported) {
  const uint16_t empty = add(Glyph{});
  const uint16_t wide = add(compound(std::vector<Component>(200, Component{empty, 0, 0})));
  const uint16_t bomb = add(compound(std::vector<Component>(200, Component{wide, 0, 0})));  // 40,000 loads
  EXPECT_EQ(Status::Ok, draw(wide));
  EXPECT_EQ(Status::Unsupported, draw(bomb));
}

TEST_F(TtfRasterizerTest, OutlinesPastTheScratchCapsAreUnsupported) {
  ScratchPlan small = roomyPlan();
  small.points = 4;
  small.contours = 1;
  const uint16_t five = add(outline({{{0, 0}, {64, 0}, {64, 64}, {32, 96}, {0, 64}}}));
  EXPECT_EQ(Status::Unsupported, draw(five, kUnitPpem, small));
  const uint16_t two = add(outline({rect(0, 0, 32, 32), rect(64, 0, 96, 32)}));
  small.points = 8;
  EXPECT_EQ(Status::Unsupported, draw(two, kUnitPpem, small));
  small.contours = 2;
  EXPECT_EQ(Status::Ok, draw(two, kUnitPpem, small));
}

TEST_F(TtfRasterizerTest, BitmapsTooLargeAreUnsupported) {
  const uint16_t big = add(outline({rect(0, 0, 2000, 2000)}));
  EXPECT_EQ(Status::Unsupported, draw(big, kMaxPpem26_6));  // 510 px a side
  ScratchPlan tight = roomyPlan();
  tight.outputBytes = 6;
  const uint16_t square = add(outline({rect(0, 0, 320, 320)}));  // 5 x 5 px: 7 bytes
  EXPECT_EQ(Status::Unsupported, draw(square, kUnitPpem, tight));
  EXPECT_EQ(Status::Unsupported, draw(square, 0));
  EXPECT_EQ(Status::Unsupported, draw(square, kMaxPpem26_6 + 1));
}

TEST_F(TtfRasterizerTest, BrokenGlyphDataIsMalformed) {
  Bytes backwards = header(2);
  put16(backwards, 5);
  put16(backwards, 3);  // contour ends must increase
  Bytes longRun = header(1);
  put16(longRun, 2);  // 3 points
  put16(longRun, 0);  // no instructions
  longRun.push_back(0x3F);  // on the curve, one-byte positive x and y, repeated...
  longRun.push_back(5);     // ...5 more times: 6 flags for 3 points
  for (int i = 0; i < 6; ++i) longRun.push_back(10);  // the 3 points' coordinates are all there
  Bytes noCoordinates = header(1);
  put16(noCoordinates, 2);
  put16(noCoordinates, 0);
  for (int i = 0; i < 3; ++i) noCoordinates.push_back(0x01);  // word coordinates follow, but the record ends
  const Bytes cutHeader = {0x00, 0x01, 0x00};
  for (const Bytes& record : {backwards, longRun, noCoordinates, cutHeader}) {
    const uint16_t g = add(raw(record));
    EXPECT_EQ(Status::Malformed, draw(g)) << g;
  }
  EXPECT_EQ(Status::Malformed, draw(static_cast<uint16_t>(font.glyphs.size())));  // past numGlyphs
}

TEST_F(TtfRasterizerTest, ALocaEntryPastGlyfIsMalformed) {
  const uint16_t g = add(outline({rect(0, 0, 64, 64)}));
  data = buildFont(font);
  const size_t locaRecord = findRecord(data, "loca");
  const uint32_t loca = static_cast<uint32_t>(data[locaRecord + 8] << 24 | data[locaRecord + 9] << 16 |
                                              data[locaRecord + 10] << 8 | data[locaRecord + 11]);
  set16(data, loca + 2 * (g + 1), 0xFFFF);  // short loca: the glyph's end, in words
  bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  ASSERT_TRUE(rasterizer.allocate(roomyPlan()));
  EXPECT_EQ(Status::Malformed, rasterizer.load(bytes, face, g, kUnitPpem, &metrics));
}

TEST_F(TtfRasterizerTest, AFailedLoadDrawsNothing) {
  const uint16_t g = add(raw({0x00, 0x01, 0x00}));
  ASSERT_EQ(Status::Malformed, draw(g));
  uint8_t canary[4] = {0xEE, 0xEE, 0xEE, 0xEE};
  EXPECT_EQ(Status::Malformed, rasterizer.rasterize(canary));
  EXPECT_EQ(0xEE, canary[0]);
}

TEST_F(TtfRasterizerTest, WithoutAScratchBlockNothingIsWrongWithTheGlyph) {
  data = buildFont(font);
  bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  EXPECT_EQ(Status::Transient, rasterizer.load(bytes, face, 0, kUnitPpem, &metrics));
  EXPECT_FALSE(isPermanent(Status::Transient));
  EXPECT_TRUE(isPermanent(Status::Malformed));
  EXPECT_TRUE(isPermanent(Status::Unsupported));
}

TEST_F(TtfRasterizerTest, RasterizeWritesWhereItIsTold) {
  const uint16_t g = add(outline({rect(0, 0, 192, 64)}));
  data = buildFont(font);
  bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  ASSERT_TRUE(rasterizer.allocate(roomyPlan()));
  ASSERT_EQ(Status::Ok, rasterizer.load(bytes, face, g, kUnitPpem, &metrics));
  uint8_t dst[2] = {0xFF, 0xFF};  // a GlyphBatch slot starts erased: every byte must be written
  ASSERT_EQ(Status::Ok, rasterizer.rasterize(dst));
  EXPECT_EQ(0xFC, dst[0]);  // 3 3 3 and a padding pixel of 0
  EXPECT_EQ(0xFF, dst[1]);  // past dataLength: untouched
}

TEST(TtfScratch, ThePlanFollowsTheFontAndItsCaps) {
  Face face;
  face.unitsPerEm = 1000;
  face.maxPoints = 600;
  face.maxContours = 40;
  face.xMax = 1000;
  face.yMax = 1000;
  const ScratchPlan plan = planScratch(face, 1000, kPoolBytesFlash);
  EXPECT_EQ(kMaxScratchPoints, plan.points);
  EXPECT_EQ(40, plan.contours);
  EXPECT_EQ(81u, plan.outputBytes);  // 16 px and a pixel each way: 18 x 18 at 2 bits
  EXPECT_EQ(kPoolBytesFlash, plan.poolBytes);
  EXPECT_EQ(kMaxOutputBytes, planScratch(face, kMaxPpem26_6, kPoolBytesFlash).outputBytes);
}

TEST(TtfScratch, OneBlockServesEveryStyle) {
  const ScratchPlan a{100, 10, 500, kPoolBytesFlash};
  const ScratchPlan b{50, 20, 900, kPoolBytesSd};
  const ScratchPlan m = mergePlans(a, b);
  EXPECT_EQ(100, m.points);
  EXPECT_EQ(20, m.contours);
  EXPECT_EQ(900u, m.outputBytes);
  EXPECT_EQ(kPoolBytesSd, m.poolBytes);
}

TEST(TtfScratch, AllocationHoldsThePlanAndRefusesImpossibleOnes) {
  Rasterizer rasterizer;
  const ScratchPlan plan{512, 256, 4096, kPoolBytesSd};
  ASSERT_TRUE(rasterizer.allocate(plan));
  EXPECT_TRUE(rasterizer.allocated());
  // The pool, 512 points (a vector and a tag each), 256 contour ends, the output.
  EXPECT_GE(rasterizer.scratchBytes(), kPoolBytesSd + 512u * 9 + 256u * 2 + 4096);
  EXPECT_NE(nullptr, rasterizer.output());
  rasterizer.release();
  EXPECT_FALSE(rasterizer.allocated());
  EXPECT_EQ(nullptr, rasterizer.output());
  EXPECT_FALSE(rasterizer.allocate({512, 256, 4096, 1024}));             // pool under ftgrays' minimum
  EXPECT_FALSE(rasterizer.allocate({kMaxScratchPoints + 1, 1, 64, kPoolBytesFlash}));
  EXPECT_FALSE(rasterizer.allocate({8, 1, kMaxOutputBytes + 1, kPoolBytesFlash}));
}

TEST(TtfStatus, NamesForTheLog) {
  EXPECT_STREQ("ok", statusName(Status::Ok));
  EXPECT_STREQ("unsupported", statusName(Status::Unsupported));
  EXPECT_STREQ("malformed", statusName(Status::Malformed));
  EXPECT_STREQ("transient", statusName(Status::Transient));
}
```

Replace `test/ttf_rasterizer/CMakeLists.txt` with:

<!-- plan-file: test/ttf_rasterizer/CMakeLists.txt -->
```cmake
# The TTF engine (lib/EpdFont/ttf): FontBytes, face parsing, the integer TrueType loader and the vendored
# ftgrays, checked against FreeType itself over every bundled TTF.

add_executable(TtfRasterizerTest
  TtfGraysTest.cpp
  FontBytesTest.cpp
  TtfFixedTest.cpp
  TtfFaceTest.cpp
  TtfRasterizerTest.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFace.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfRasterizer.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/third_party/freetype/ftgrays.c
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(TtfRasterizerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/shims
)

target_compile_definitions(TtfRasterizerTest PRIVATE
  TTF_FONT_DIR="${REPO_ROOT}/lib/EpdFont/builtinFonts/source"
)

target_link_libraries(TtfRasterizerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

# Spec 9.3: the engine runs under AddressSanitizer and UBSan (CI's Linux GCC). Where ASan cannot link
# (MinGW), UBSan still traps on undefined behaviour without its runtime. FreeType is not instrumented.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address,undefined)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_ASAN)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=undefined -fsanitize-undefined-trap-on-error")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_UBSAN_TRAP)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(TTF_TESTS_HAVE_ASAN)
  set(TTF_SANITIZERS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
elseif(TTF_TESTS_HAVE_UBSAN_TRAP)
  set(TTF_SANITIZERS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
endif()
target_compile_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
target_link_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
message(STATUS "TtfRasterizerTest sanitizers: ${TTF_SANITIZERS}")

gtest_discover_tests(TtfRasterizerTest DISCOVERY_TIMEOUT 60)
```

- [ ] **Step 2: Run it to see it fail**

Run: `cmake -S test -B build/test`
Expected: `CMake Error ... Cannot find source file: .../lib/EpdFont/ttf/TtfRasterizer.cpp`

- [ ] **Step 3: Write the code**

Create `lib/EpdFont/ttf/TtfRasterizer.h`:

<!-- plan-file: lib/EpdFont/ttf/TtfRasterizer.h -->
```cpp
#pragma once

#include <cstdint>
#include <memory>

#include "EpdFontData.h"
#include "FontBytes.h"
#include "TtfFace.h"

namespace ttf {

/// What became of a glyph (spec 6.1).
enum class Status : uint8_t {
  Ok,           ///< loaded or drawn; a glyph with no outline (a space) loads with width and height 0
  Unsupported,  ///< permanent: a point-matched or scaled-offset component, more points or contours than the
                ///< scratch block holds, too deep or too many components, a bitmap over 255 px a side or
                ///< over the output region, or coordinates out of range
  Malformed,    ///< permanent: broken glyph data (a read past the glyph or the file, contour ends out of
                ///< order, a flag run past the last point, a glyph index past numGlyphs)
  Transient,    ///< nothing is known to be wrong with the glyph (an SD read failed, Stage 2c; or no scratch
                ///< block): not marked in the store, tried again at the next lookup
};

constexpr bool isPermanent(const Status status) {
  return status == Status::Unsupported || status == Status::Malformed;
}
const char* statusName(Status status);

constexpr uint16_t kMaxScratchPoints = 512;    ///< about 4.6 KB on the C3 (9 B a point)
constexpr uint16_t kMaxScratchContours = 256;  ///< 2 B a contour
constexpr uint32_t kMaxOutputBytes = 4096;     ///< the 2-bit output region
constexpr uint32_t kPoolBytesFlash = 4096;     ///< ftgrays' cell pool for a family in flash mode
constexpr uint32_t kPoolBytesSd = 8192;        ///< ... and in SD mode (CJK: about 37% faster than 4 KB)
constexpr uint32_t kMaxPpem26_6 = 255 << 6;    ///< larger sizes cannot fit EpdGlyph's 8-bit width and height

/// The parts of the scratch block (spec 5.3).
struct ScratchPlan {
  uint16_t points = 0;
  uint16_t contours = 0;
  uint32_t outputBytes = 0;
  uint32_t poolBytes = 0;
};

/// The plan for one face: points and contours from maxp, the output from head's box at `maxPpem26_6` (the
/// largest size the family renders), each capped. A glyph past a cap reports Unsupported.
ScratchPlan planScratch(const Face& face, uint32_t maxPpem26_6, uint32_t poolBytes);
/// The larger of each part, so one block serves every style of a family.
ScratchPlan mergePlans(const ScratchPlan& a, const ScratchPlan& b);

/// Glyph outlines to 2-bit bitmaps: FreeType's TrueType loader rebuilt in integers (simple glyphs, compound
/// glyphs with offsets and 2.14 transforms, unhinted, exactly as FreeType scales them) feeding FreeType's own
/// anti-aliasing rasterizer (ftgrays), whose coverage spans are packed straight into the 2-bit stream with the
/// converter's thresholds. One per loaded family; it owns the family's scratch block.
///
/// Two steps, so a caller can learn a glyph's size before choosing where it goes: load() reads and scales
/// the outline into the scratch block and reports the metrics; rasterize() draws it anywhere.
///
/// Not reentrant across instances: ftgrays' standalone raster object is a static, so renders must not overlap.
/// The engine renders only under RenderLock.
class Rasterizer {
 public:
  Rasterizer() = default;
  Rasterizer(const Rasterizer&) = delete;
  Rasterizer& operator=(const Rasterizer&) = delete;

  /// Allocates the scratch block (one heap block, held until release()). False on OOM, logged.
  bool allocate(const ScratchPlan& plan);
  void release();
  bool allocated() const { return block_ != nullptr; }
  uint32_t scratchBytes() const { return blockBytes_; }
  const ScratchPlan& plan() const { return plan_; }
  /// The output region, plan().outputBytes long: render() leaves its bitmap here.
  uint8_t* output() { return block_ ? block_.get() + outputOffset_ : nullptr; }

  /// Loads `glyph` of `face` at `ppem26_6` into the scratch block and fills `metrics` as the .cpfont
  /// converter would: width, height, left, top, advanceX (12.4) and dataLength; dataOffset is 0.
  Status load(FontBytes& bytes, const Face& face, uint16_t glyph, uint32_t ppem26_6, EpdGlyph* metrics);
  /// Draws the glyph of the last load() into `dst`, writing all of its dataLength bytes (MSB-first, rows
  /// continuous, as .cpfont bitmaps). After a failed load() it writes nothing and returns that status.
  Status rasterize(uint8_t* dst);
  /// load(), then rasterize() into output().
  Status render(FontBytes& bytes, const Face& face, uint16_t glyph, uint32_t ppem26_6, EpdGlyph* metrics);

 private:
  struct Loader;

  std::unique_ptr<uint8_t[]> block_;
  uint32_t blockBytes_ = 0;
  ScratchPlan plan_;
  uint32_t pointsOffset_ = 0;
  uint32_t contoursOffset_ = 0;
  uint32_t tagsOffset_ = 0;
  uint32_t outputOffset_ = 0;
  // The last load(): its status, outline size and bitmap box.
  Status loaded_ = Status::Malformed;
  uint16_t numPoints_ = 0;
  uint16_t numContours_ = 0;
  uint16_t width_ = 0;
  uint16_t height_ = 0;
};

}  // namespace ttf
```

Create `lib/EpdFont/ttf/TtfRasterizer.cpp`:

<!-- plan-file: lib/EpdFont/ttf/TtfRasterizer.cpp -->
```cpp
#include "TtfRasterizer.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "TtfFixed.h"
#include "TtfGrays.h"

namespace ttf {

namespace {

// Simple-glyph flags.
constexpr uint8_t kOnCurve = 0x01;
constexpr uint8_t kXShort = 0x02;
constexpr uint8_t kYShort = 0x04;
constexpr uint8_t kRepeat = 0x08;
constexpr uint8_t kXSameOrPositive = 0x10;
constexpr uint8_t kYSameOrPositive = 0x20;

// Compound-glyph component flags.
constexpr uint16_t kArgsAreWords = 0x0001;
constexpr uint16_t kArgsAreXy = 0x0002;
constexpr uint16_t kHaveScale = 0x0008;
constexpr uint16_t kMoreComponents = 0x0020;
constexpr uint16_t kHaveXyScale = 0x0040;
constexpr uint16_t kHaveTwoByTwo = 0x0080;
constexpr uint16_t kUseMyMetrics = 0x0200;
constexpr uint16_t kScaledOffset = 0x0800;

constexpr int kMaxDepth = 8;                  // nesting below the top glyph; fonts use 1 to 3
constexpr uint16_t kMaxComponentLoads = 256;  // per glyph: a compound of compounds cannot run away
constexpr int64_t kMaxCoord = 1 << 24;        // 26.6, far past any glyph that fits; keeps every product in range

uint32_t alignUp(const uint32_t value, const uint32_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// A glyph record: every read stays inside it, and the first that would not marks it overrun.
struct Cursor {
  FontBytes& bytes;
  uint32_t pos;
  uint32_t end;
  bool overrun = false;

  bool take(const uint32_t n) {
    if (n > end - pos) {
      overrun = true;
      pos = end;
      return false;
    }
    pos += n;
    return true;
  }
  uint8_t u8() { return take(1) ? bytes.u8(pos - 1) : 0; }
  uint16_t u16() { return take(2) ? bytes.u16(pos - 2) : 0; }
  int16_t i16() { return static_cast<int16_t>(u16()); }
};

bool inRange(const int64_t value) { return value >= -kMaxCoord && value <= kMaxCoord; }

// Packs ftgrays' coverage spans into the 2-bit stream with the PC converter's thresholds (coverage nibble
// >= 12 -> 3, >= 8 -> 2, >= 4 -> 1). ftgrays counts rows from the bottom; the bitmap starts at the top.
struct SpanSink {
  uint8_t* out;
  int width;
  int height;
};

void packSpans(const int y, const int count, const FT_Span* spans, void* user) {
  const auto* sink = static_cast<const SpanSink*>(user);
  const int row = sink->height - 1 - y;
  if (row < 0 || row >= sink->height) return;
  for (int i = 0; i < count; ++i) {
    const int level = spans[i].coverage >> 4;
    const uint8_t value = level >= 12 ? 3 : level >= 8 ? 2 : level >= 4 ? 1 : 0;
    if (value == 0) continue;
    const int from = spans[i].x < 0 ? 0 : spans[i].x;
    const int to = spans[i].x + spans[i].len < sink->width ? spans[i].x + spans[i].len : sink->width;
    for (int x = from; x < to; ++x) {
      const uint32_t pixel = static_cast<uint32_t>(row * sink->width + x);
      sink->out[pixel >> 2] |= static_cast<uint8_t>(value << (6 - 2 * (pixel & 3)));
    }
  }
}

}  // namespace

// Reads a glyph into the scratch arrays the way FreeType's TrueType loader does when it does not hint: each
// simple glyph's points scaled with FT_MulFix as they are read, each component transformed and then offset
// in 26.6, and the whole outline moved so the left side bearing's phantom point is the origin.
struct Rasterizer::Loader {
  FontBytes& bytes;
  const Face& face;
  int32_t scale;
  FT_Vector* points;
  unsigned short* contours;
  unsigned char* tags;
  uint16_t pointCap;
  uint16_t contourCap;
  uint16_t numPoints = 0;
  uint16_t numContours = 0;
  uint16_t componentLoads = 0;

  // Appends `glyph` and reports its phantom point pp1.x: the shift FreeType applies when it is the top glyph,
  // or a component flagged USE_MY_METRICS.
  Status load(const uint16_t glyph, const int depth, int32_t* pp1x) {
    if (glyph >= face.numGlyphs) return Status::Malformed;
    uint32_t start = 0;
    uint32_t end = 0;
    const uint32_t entry = static_cast<uint32_t>(glyph) * (face.longLoca ? 4 : 2);
    if (entry + (face.longLoca ? 8 : 4) > face.locaLength) return Status::Malformed;
    if (face.longLoca) {
      start = bytes.u32(face.loca + entry);
      end = bytes.u32(face.loca + entry + 4);
    } else {
      start = 2u * bytes.u16(face.loca + entry);
      end = 2u * bytes.u16(face.loca + entry + 2);
    }
    if (start > end || end > face.glyfLength) return Status::Malformed;

    uint16_t advance = 0;
    int16_t lsb = 0;
    horizontalMetrics(bytes, face, glyph, &advance, &lsb);
    if (start == end) {  // no outline, as a space; FreeType takes its box as 0
      *pp1x = mulFix(-lsb, scale);
      return Status::Ok;
    }
    Cursor cursor{bytes, face.glyf + start, face.glyf + end};
    const int16_t contourCount = cursor.i16();
    const int16_t xMin = cursor.i16();
    cursor.take(6);  // yMin, xMax, yMax
    if (cursor.overrun) return Status::Malformed;
    *pp1x = mulFix(xMin - lsb, scale);
    if (contourCount >= 0) return loadSimple(cursor, contourCount);
    if (depth >= kMaxDepth) return Status::Unsupported;
    return loadCompound(cursor, depth, pp1x);
  }

  Status loadSimple(Cursor& cursor, const int16_t count) {
    if (count == 0) return Status::Ok;
    if (count > contourCap - numContours) return Status::Unsupported;
    const uint16_t base = numPoints;
    int32_t last = -1;
    for (int16_t i = 0; i < count; ++i) {
      const uint16_t endPoint = cursor.u16();
      if (cursor.overrun || endPoint <= last) return Status::Malformed;  // FreeType: ends must increase
      if (endPoint >= pointCap - base) return Status::Unsupported;
      contours[numContours + i] = static_cast<unsigned short>(base + endPoint);
      last = endPoint;
    }
    const uint32_t count32 = static_cast<uint32_t>(last) + 1;
    cursor.take(cursor.u16());  // the instructions
    if (cursor.overrun) return Status::Malformed;

    unsigned char* flags = tags + base;
    for (uint32_t i = 0; i < count32;) {
      const uint8_t flag = cursor.u8();
      flags[i++] = flag;
      if (flag & kRepeat) {
        const uint8_t repeat = cursor.u8();
        if (repeat > count32 - i) return Status::Malformed;  // FreeType: a run past the last point
        for (uint8_t k = 0; k < repeat; ++k) flags[i++] = flag;
      }
      if (cursor.overrun) return Status::Malformed;
    }
    FT_Vector* out = points + base;
    int32_t value = 0;
    for (uint32_t i = 0; i < count32; ++i) {
      if (flags[i] & kXShort) {
        const int32_t delta = cursor.u8();
        value += (flags[i] & kXSameOrPositive) ? delta : -delta;
      } else if (!(flags[i] & kXSameOrPositive)) {
        value += cursor.i16();
      }
      out[i].x = value;
    }
    value = 0;
    for (uint32_t i = 0; i < count32; ++i) {
      if (flags[i] & kYShort) {
        const int32_t delta = cursor.u8();
        value += (flags[i] & kYSameOrPositive) ? delta : -delta;
      } else if (!(flags[i] & kYSameOrPositive)) {
        value += cursor.i16();
      }
      out[i].y = value;
    }
    if (cursor.overrun) return Status::Malformed;
    for (uint32_t i = 0; i < count32; ++i) {
      const int64_t x = mulFixWide(static_cast<int32_t>(out[i].x), scale);
      const int64_t y = mulFixWide(static_cast<int32_t>(out[i].y), scale);
      if (!inRange(x) || !inRange(y)) return Status::Unsupported;
      out[i].x = static_cast<FT_Pos>(x);
      out[i].y = static_cast<FT_Pos>(y);
      flags[i] = (flags[i] & kOnCurve) ? FT_CURVE_TAG_ON : FT_CURVE_TAG_CONIC;
    }
    numContours = static_cast<uint16_t>(numContours + count);
    numPoints = static_cast<uint16_t>(numPoints + count32);
    return Status::Ok;
  }

  Status loadCompound(Cursor& cursor, const int depth, int32_t* pp1x) {
    uint16_t flags = 0;
    do {
      if (++componentLoads > kMaxComponentLoads) return Status::Unsupported;
      flags = cursor.u16();
      const uint16_t child = cursor.u16();
      int32_t arg1 = 0;
      int32_t arg2 = 0;
      if (flags & kArgsAreWords) {
        arg1 = cursor.i16();
        arg2 = cursor.i16();
      } else {
        arg1 = static_cast<int8_t>(cursor.u8());
        arg2 = static_cast<int8_t>(cursor.u8());
      }
      // The 2.14 transform as FreeType holds it, in 16.16: x' = x*xx + y*xy, y' = x*yx + y*yy.
      int32_t xx = 0x10000;
      int32_t xy = 0;
      int32_t yx = 0;
      int32_t yy = 0x10000;
      if (flags & kHaveScale) {
        xx = yy = cursor.i16() * 4;
      } else if (flags & kHaveXyScale) {
        xx = cursor.i16() * 4;
        yy = cursor.i16() * 4;
      } else if (flags & kHaveTwoByTwo) {
        xx = cursor.i16() * 4;
        yx = cursor.i16() * 4;
        xy = cursor.i16() * 4;
        yy = cursor.i16() * 4;
      }
      if (cursor.overrun) return Status::Malformed;
      const bool transformed = (flags & (kHaveScale | kHaveXyScale | kHaveTwoByTwo)) != 0;
      if (!(flags & kArgsAreXy)) return Status::Unsupported;               // point matching (spec 5.6)
      if (transformed && (flags & kScaledOffset)) return Status::Unsupported;  // Apple's scaled offsets

      const uint16_t first = numPoints;
      int32_t childPp1x = 0;
      const Status status = load(child, depth + 1, &childPp1x);
      if (status != Status::Ok) return status;
      if (transformed) {
        for (uint16_t i = first; i < numPoints; ++i) {
          const int32_t x = static_cast<int32_t>(points[i].x);
          const int32_t y = static_cast<int32_t>(points[i].y);
          const int64_t tx = mulFixWide(x, xx) + mulFixWide(y, xy);
          const int64_t ty = mulFixWide(x, yx) + mulFixWide(y, yy);
          if (!inRange(tx) || !inRange(ty)) return Status::Unsupported;
          points[i].x = static_cast<FT_Pos>(tx);
          points[i].y = static_cast<FT_Pos>(ty);
        }
      }
      if (!translate(first, mulFix(arg1, scale), mulFix(arg2, scale))) return Status::Unsupported;
      if (flags & kUseMyMetrics) *pp1x = childPp1x;
    } while (flags & kMoreComponents);
    return Status::Ok;
  }

  // Moves the points from `first` on; false when one would leave the coordinate range.
  bool translate(const uint16_t first, const int32_t dx, const int32_t dy) {
    if (dx == 0 && dy == 0) return true;
    for (uint16_t i = first; i < numPoints; ++i) {
      const int64_t x = static_cast<int64_t>(points[i].x) + dx;
      const int64_t y = static_cast<int64_t>(points[i].y) + dy;
      if (!inRange(x) || !inRange(y)) return false;
      points[i].x = static_cast<FT_Pos>(x);
      points[i].y = static_cast<FT_Pos>(y);
    }
    return true;
  }
};

const char* statusName(const Status status) {
  switch (status) {
    case Status::Ok:
      return "ok";
    case Status::Unsupported:
      return "unsupported";
    case Status::Malformed:
      return "malformed";
    case Status::Transient:
      return "transient";
  }
  return "?";
}

ScratchPlan planScratch(const Face& face, const uint32_t maxPpem26_6, const uint32_t poolBytes) {
  ScratchPlan plan;
  plan.points = face.maxPoints < kMaxScratchPoints ? face.maxPoints : kMaxScratchPoints;
  plan.contours = face.maxContours < kMaxScratchContours ? face.maxContours : kMaxScratchContours;
  const int32_t scale = unitsToPixels(face, maxPpem26_6 < kMaxPpem26_6 ? maxPpem26_6 : kMaxPpem26_6);
  // head's box at that size, one pixel wider each way for the rounding of compound transforms.
  int32_t width = ((mulFix(face.xMax, scale) + 63) >> 6) - (mulFix(face.xMin, scale) >> 6) + 2;
  int32_t height = ((mulFix(face.yMax, scale) + 63) >> 6) - (mulFix(face.yMin, scale) >> 6) + 2;
  width = width < 0 ? 0 : width > 255 ? 255 : width;
  height = height < 0 ? 0 : height > 255 ? 255 : height;
  const uint32_t bytes = glyphDataBytes(width, height, true);
  plan.outputBytes = bytes < kMaxOutputBytes ? bytes : kMaxOutputBytes;
  plan.poolBytes = poolBytes;
  return plan;
}

ScratchPlan mergePlans(const ScratchPlan& a, const ScratchPlan& b) {
  ScratchPlan plan;
  plan.points = a.points > b.points ? a.points : b.points;
  plan.contours = a.contours > b.contours ? a.contours : b.contours;
  plan.outputBytes = a.outputBytes > b.outputBytes ? a.outputBytes : b.outputBytes;
  plan.poolBytes = a.poolBytes > b.poolBytes ? a.poolBytes : b.poolBytes;
  return plan;
}

bool Rasterizer::allocate(const ScratchPlan& plan) {
  release();
  if (plan.points > kMaxScratchPoints || plan.contours > kMaxScratchContours || plan.outputBytes > kMaxOutputBytes ||
      plan.poolBytes < 2048 || plan.poolBytes > kPoolBytesSd) {
    LOG_ERR("TTF", "scratch plan out of range: %u points, %u contours, %lu output, %lu pool", plan.points,
            plan.contours, static_cast<unsigned long>(plan.outputBytes), static_cast<unsigned long>(plan.poolBytes));
    return false;
  }
  // The pool comes first: the block's start is aligned for ftgrays' cells and for the points that follow.
  pointsOffset_ = alignUp(plan.poolBytes, 8);
  contoursOffset_ = pointsOffset_ + plan.points * static_cast<uint32_t>(sizeof(FT_Vector));
  tagsOffset_ = contoursOffset_ + plan.contours * static_cast<uint32_t>(sizeof(unsigned short));
  outputOffset_ = tagsOffset_ + plan.points;
  const uint32_t bytes = outputOffset_ + plan.outputBytes;
  // One block for the family's lifetime: a cell pool and outline arrays on the loop task's stack would take
  // 4-10 KB of its 16 KB, and per-glyph allocations would fragment the heap.
  block_ = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!block_) {
    LOG_ERR("TTF", "scratch block: %lu bytes, out of memory", static_cast<unsigned long>(bytes));
    return false;
  }
  blockBytes_ = bytes;
  plan_ = plan;
  loaded_ = Status::Transient;
  return true;
}

void Rasterizer::release() {
  block_.reset();
  blockBytes_ = 0;
  plan_ = ScratchPlan{};
  loaded_ = Status::Transient;
}

Status Rasterizer::load(FontBytes& bytes, const Face& face, const uint16_t glyph, const uint32_t ppem26_6,
                        EpdGlyph* metrics) {
  *metrics = EpdGlyph{};
  numPoints_ = numContours_ = width_ = height_ = 0;
  if (!block_) return loaded_ = Status::Transient;
  if (ppem26_6 == 0 || ppem26_6 > kMaxPpem26_6) return loaded_ = Status::Unsupported;

  bytes.clearErrors();
  auto* points = reinterpret_cast<FT_Vector*>(block_.get() + pointsOffset_);
  Loader loader{bytes,
                face,
                unitsToPixels(face, ppem26_6),
                points,
                reinterpret_cast<unsigned short*>(block_.get() + contoursOffset_),
                block_.get() + tagsOffset_,
                plan_.points,
                plan_.contours};
  int32_t pp1x = 0;
  Status status = loader.load(glyph, 0, &pp1x);
  if (bytes.ioError()) status = Status::Transient;
  if (status == Status::Ok && bytes.outOfRange()) status = Status::Malformed;
  if (status == Status::Ok && !loader.translate(0, -pp1x, 0)) status = Status::Unsupported;
  if (status != Status::Ok) return loaded_ = status;
  const uint16_t advance = advance12_4(bytes, face, glyph, ppem26_6);
  if (bytes.ioError()) return loaded_ = Status::Transient;

  int32_t left = 0;
  int32_t top = 0;
  int32_t width = 0;
  int32_t height = 0;
  if (loader.numPoints > 0) {
    // FreeType's bitmap box for gray rendering: the control box, floored and ceiled to whole pixels.
    FT_Pos xMin = points[0].x;
    FT_Pos yMin = points[0].y;
    FT_Pos xMax = xMin;
    FT_Pos yMax = yMin;
    for (uint16_t i = 1; i < loader.numPoints; ++i) {
      xMin = points[i].x < xMin ? points[i].x : xMin;
      yMin = points[i].y < yMin ? points[i].y : yMin;
      xMax = points[i].x > xMax ? points[i].x : xMax;
      yMax = points[i].y > yMax ? points[i].y : yMax;
    }
    left = static_cast<int32_t>(xMin >> 6);
    const int32_t bottom = static_cast<int32_t>(yMin >> 6);
    top = static_cast<int32_t>((yMax + 63) >> 6);
    width = static_cast<int32_t>((xMax + 63) >> 6) - left;
    height = top - bottom;
    if (width > 255 || height > 255 || glyphDataBytes(width, height, true) > plan_.outputBytes || left < INT16_MIN ||
        top > INT16_MAX) {
      return loaded_ = Status::Unsupported;
    }
    loader.translate(0, -left * 64, -bottom * 64);  // the box now starts at (0, 0), as ftgrays' clip box does
  }
  metrics->width = static_cast<uint8_t>(width);
  metrics->height = static_cast<uint8_t>(height);
  metrics->advanceX = advance;
  metrics->left = static_cast<int16_t>(left);
  metrics->top = static_cast<int16_t>(top);
  metrics->dataLength = glyphDataBytes(width, height, true);
  numPoints_ = loader.numPoints;
  numContours_ = loader.numContours;
  width_ = static_cast<uint16_t>(width);
  height_ = static_cast<uint16_t>(height);
  return loaded_ = Status::Ok;
}

Status Rasterizer::rasterize(uint8_t* dst) {
  if (loaded_ != Status::Ok) return loaded_;
  const uint32_t bytes = glyphDataBytes(width_, height_, true);
  if (bytes == 0) return Status::Ok;
  std::memset(dst, 0, bytes);

  FT_Outline outline{};
  outline.n_contours = numContours_;
  outline.n_points = numPoints_;
  outline.points = reinterpret_cast<FT_Vector*>(block_.get() + pointsOffset_);
  outline.tags = block_.get() + tagsOffset_;
  outline.contours = reinterpret_cast<unsigned short*>(block_.get() + contoursOffset_);
  outline.flags = FT_OUTLINE_NONE;  // nonzero winding, as TrueType

  SpanSink sink{dst, width_, height_};
  FT_Raster_Params params{};
  params.source = &outline;
  params.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
  params.gray_spans = packSpans;
  params.user = &sink;
  params.clip_box.xMax = width_;
  params.clip_box.yMax = height_;

  FT_Raster raster = nullptr;
  ttf_grays_raster.raster_new(nullptr, &raster);
  ttf_grays_raster.raster_reset(raster, block_.get(), plan_.poolBytes);
  return ttf_grays_raster.raster_render(raster, &params) == 0 ? Status::Ok : Status::Malformed;
}

Status Rasterizer::render(FontBytes& bytes, const Face& face, const uint16_t glyph, const uint32_t ppem26_6,
                          EpdGlyph* metrics) {
  const Status status = load(bytes, face, glyph, ppem26_6, metrics);
  return status == Status::Ok ? rasterize(output()) : status;
}

}  // namespace ttf
```

- [ ] **Step 4: Run the tests**

```bash
cmake -S test -B build/test
cmake --build build/test --target TtfRasterizerTest -j 8
./build/test/ttf_rasterizer/TtfRasterizerTest.exe
```

Expected: `[  PASSED  ] 51 tests.`

- [ ] **Step 5: Check that the tests bite**

Make each change below in turn, rebuild, run, and restore the file with `git checkout -- <file>`. Every one
must fail at least one test. These are the mutations checked while writing this plan.

| File | Change | Fails |
| --- | --- | --- |
| `TtfRasterizer.cpp` | `!loader.translate(0, -pp1x, 0)` → `!loader.translate(0, 0, 0)` | `TheOutlineMovesSo...`, `UseMyMetrics...` |
| `TtfRasterizer.cpp` | swap the `yx = ...` and `xy = ...` reads of the 2x2 transform | `TheTwoByTwoTransform...` |
| `TtfRasterizer.cpp` | `const int row = sink->height - 1 - y;` → `const int row = y;` | `RowsRunFromTheTop`, ... |
| `TtfRasterizer.cpp` | `level >= 8 ? 2` → `level >= 9 ? 2` | `CoverageQuantises...` |
| `TtfRasterizer.cpp` | delete `if (repeat > count32 - i) return Status::Malformed;` | `BrokenGlyphDataIsMalformed` |
| `TtfRasterizer.cpp` | `if (++componentLoads > kMaxComponentLoads)` → `if (false)` | `TooManyComponentsAreUnsupported` |
| `TtfFace.cpp` | `for (int pass = 0; pass < 2; ++pass)` → `pass = 1` | `TheCmapIsChosenAsFreeTypeChoosesIt` |
| `TtfFace.cpp` | `(mulFix(face.ascender, scale) + 63) & -64` → `mulFix(face.ascender, scale) & -64` | `LineMetricsAreTheConverters` |
| `TtfFace.cpp` | `const uint32_t last = 4 * (metrics - 1);` → `4 * glyph` | `HmtxIsReadAsFreeTypeReadsIt` |

- [ ] **Step 6: Format and commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe -i lib/EpdFont/ttf/TtfRasterizer.h lib/EpdFont/ttf/TtfRasterizer.cpp test/ttf_rasterizer/TtfRasterizerTest.cpp
git add lib/EpdFont/ttf/TtfRasterizer.h lib/EpdFont/ttf/TtfRasterizer.cpp test/ttf_rasterizer
git commit -m "feat(ttf): integer TrueType loader feeding ftgrays, packed straight to 2-bit"
```

---

### Task 5: The regression gate against FreeType

FreeType itself (the SDK's copy) is built for the host and is the reference, as spec 9.3 asks. The test walks
every glyph index of every bundled TTF at 8, 14 and 40 pt. It checks the cmap up to U+2FFFF and the line
metrics at six sizes. It also re-renders a whole face with three pool sizes. This task adds no engine code:
the gate holds Task 4's code to the numbers measured while writing this plan, and Step 3 proves it would
catch a regression.

**Files:**
- Create: `test/ttf_rasterizer/FreeTypeReference.h`, `test/ttf_rasterizer/FreeTypeReference.cpp`, `test/ttf_rasterizer/TtfVsFreeTypeTest.cpp`
- Modify: `test/ttf_rasterizer/CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Tasks 2-4; `ttf_test::readFont`, `bundledTtfs` and `picture` (Task 3).
- Produces: `ttf_test::FreeTypeReference(font, points)`, with `ok()`, `numGlyphs()`, `glyphIndex(cp)`,
  `glyph(index) -> ReferenceGlyph` and the size metrics `ascender()`, `descender()` and `height()` in 26.6.
  It is the only test file that includes FreeType's headers.

- [ ] **Step 1: Write the reference and the comparison**

Create `test/ttf_rasterizer/FreeTypeReference.h`:

<!-- plan-file: test/ttf_rasterizer/FreeTypeReference.h -->
```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace ttf_test {

/// One glyph as FreeType renders it unhinted.
struct ReferenceGlyph {
  bool loaded = false;
  bool overlap = false;  ///< the outline carries an OVERLAP flag: FreeType renders it 4x4 oversampled
  int width = 0;
  int height = 0;
  int left = 0;
  int top = 0;
  int32_t linearAdvance = 0;     ///< linearHoriAdvance, 16.16
  std::vector<uint8_t> bitmap;  ///< packed 2-bit, as fontconvert_sdcard.py packs it
};

/// FreeType 2.13.3, the SDK's copy, set up as the PC converter sets it up (FT_Set_Char_Size at 150 dpi) but
/// loading unhinted: the reference the TTF engine is checked against. The only test file that includes
/// FreeType's headers; the engine's ftimage.h would clash with them.
class FreeTypeReference {
 public:
  FreeTypeReference(const std::vector<uint8_t>& font, uint32_t points);
  ~FreeTypeReference();
  FreeTypeReference(const FreeTypeReference&) = delete;
  FreeTypeReference& operator=(const FreeTypeReference&) = delete;

  bool ok() const;
  uint32_t numGlyphs() const;
  uint32_t glyphIndex(uint32_t codepoint) const;
  ReferenceGlyph glyph(uint32_t index) const;
  /// The size metrics in 26.6, as the converter reads face.size.
  int32_t ascender() const;
  int32_t descender() const;
  int32_t height() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ttf_test
```

Create `test/ttf_rasterizer/FreeTypeReference.cpp`:

<!-- plan-file: test/ttf_rasterizer/FreeTypeReference.cpp -->
```cpp
#include "FreeTypeReference.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

namespace ttf_test {

struct FreeTypeReference::Impl {
  FT_Library library = nullptr;
  FT_Face face = nullptr;
};

FreeTypeReference::FreeTypeReference(const std::vector<uint8_t>& font, const uint32_t points) : impl_(new Impl) {
  if (FT_Init_FreeType(&impl_->library) != 0) return;
  if (FT_New_Memory_Face(impl_->library, font.data(), static_cast<FT_Long>(font.size()), 0, &impl_->face) != 0) {
    impl_->face = nullptr;
    return;
  }
  const FT_F26Dot6 size = static_cast<FT_F26Dot6>(points) << 6;
  if (FT_Set_Char_Size(impl_->face, size, size, 150, 150) != 0) {
    FT_Done_Face(impl_->face);
    impl_->face = nullptr;
  }
}

FreeTypeReference::~FreeTypeReference() {
  if (impl_->face) FT_Done_Face(impl_->face);
  if (impl_->library) FT_Done_FreeType(impl_->library);
}

bool FreeTypeReference::ok() const { return impl_->face != nullptr; }
uint32_t FreeTypeReference::numGlyphs() const { return static_cast<uint32_t>(impl_->face->num_glyphs); }
uint32_t FreeTypeReference::glyphIndex(const uint32_t codepoint) const {
  return FT_Get_Char_Index(impl_->face, codepoint);
}
int32_t FreeTypeReference::ascender() const { return static_cast<int32_t>(impl_->face->size->metrics.ascender); }
int32_t FreeTypeReference::descender() const { return static_cast<int32_t>(impl_->face->size->metrics.descender); }
int32_t FreeTypeReference::height() const { return static_cast<int32_t>(impl_->face->size->metrics.height); }

ReferenceGlyph FreeTypeReference::glyph(const uint32_t index) const {
  ReferenceGlyph out;
  FT_GlyphSlot slot = impl_->face->glyph;
  if (FT_Load_Glyph(impl_->face, index, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) return out;
  out.overlap = (slot->outline.flags & FT_OUTLINE_OVERLAP) != 0;
  out.linearAdvance = static_cast<int32_t>(slot->linearHoriAdvance);
  if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) return out;
  out.loaded = true;
  const FT_Bitmap& bitmap = slot->bitmap;
  out.width = static_cast<int>(bitmap.width);
  out.height = static_cast<int>(bitmap.rows);
  out.left = slot->bitmap_left;
  out.top = slot->bitmap_top;
  out.bitmap.assign((out.width * out.height + 3) / 4, 0);
  for (int y = 0; y < out.height; ++y) {
    const unsigned char* row = bitmap.buffer + y * bitmap.pitch;
    for (int x = 0; x < out.width; ++x) {
      const int level = row[x] >> 4;
      const int value = level >= 12 ? 3 : level >= 8 ? 2 : level >= 4 ? 1 : 0;
      const int p = y * out.width + x;
      out.bitmap[p >> 2] |= static_cast<uint8_t>(value << (6 - 2 * (p & 3)));
    }
  }
  return out;
}

}  // namespace ttf_test
```

Create `test/ttf_rasterizer/TtfVsFreeTypeTest.cpp`:

<!-- plan-file: test/ttf_rasterizer/TtfVsFreeTypeTest.cpp -->
```cpp
// The integer loader against FreeType itself, unhinted (spec 9.3), over every glyph of every bundled TTF.
// FreeType renders glyphs whose outlines carry an OVERLAP flag 4x4 oversampled, which the engine does not
// (spec 11.6): those are held to the spec's pixel budget. Every other glyph must come out byte for byte.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "FreeTypeReference.h"
#include "TtfRasterizer.h"
#include "TtfTestUtil.h"

using namespace ttf;
using namespace ttf_test;

namespace {

constexpr uint32_t kPoints = 14;

int pixelAt(const uint8_t* bits, const int p) { return (bits[p >> 2] >> (6 - 2 * (p & 3))) & 3; }

class TtfVsFreeType : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    data = readFont(GetParam());
    ASSERT_FALSE(data.empty());
    bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
    ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  }

  void compareEveryGlyph(uint32_t points);

  std::vector<uint8_t> data;
  FontBytes bytes;
  Face face;
};

std::string testName(const ::testing::TestParamInfo<std::string>& info) {
  std::string name = info.param.substr(info.param.find('/') + 1);
  for (char& c : name) {
    if (c == '-' || c == '.') c = '_';
  }
  return name;
}

}  // namespace

TEST_P(TtfVsFreeType, EveryGlyphMatchesFreeType) {
  // Small print, the body size, and a large heading.
  for (const uint32_t points : {8u, kPoints, 40u}) {
    SCOPED_TRACE(points);
    compareEveryGlyph(points);
  }
}

void TtfVsFreeType::compareEveryGlyph(const uint32_t points) {
  const FreeTypeReference reference(data, points);
  ASSERT_TRUE(reference.ok());
  ASSERT_EQ(reference.numGlyphs(), face.numGlyphs);
  const uint32_t ppem = ppemFromPoints(points);
  Rasterizer rasterizer;
  ASSERT_TRUE(rasterizer.allocate(planScratch(face, ppem, kPoolBytesFlash)));

  uint32_t glyphs = 0, tooLarge = 0, loadFailures = 0, advanceDiffs = 0, boxDiffs = 0, exactDiffs = 0;
  uint32_t overlapGlyphs = 0;
  uint64_t overlapPixels = 0, overlapPixelDiffs = 0;
  for (uint32_t g = 0; g < face.numGlyphs; ++g) {
    const ReferenceGlyph expected = reference.glyph(g);
    if (!expected.loaded) continue;
    ++glyphs;
    EpdGlyph metrics{};
    const Status status = rasterizer.render(bytes, face, static_cast<uint16_t>(g), ppem, &metrics);
    // A bitmap over 255 px a side cannot be an EpdGlyph, and one over the output region is past its cap
    // (spec 5.3): both are Unsupported by design. Ubuntu's wordmark at 40 pt is 288 px wide.
    if (expected.width > 255 || expected.height > 255 ||
        glyphDataBytes(expected.width, expected.height, true) > rasterizer.plan().outputBytes) {
      EXPECT_EQ(Status::Unsupported, status) << "glyph " << g;
      ++tooLarge;
      continue;
    }
    if (status != Status::Ok) {
      ++loadFailures;
      ADD_FAILURE() << "glyph " << g << ": " << statusName(status);
      continue;
    }
    if (metrics.advanceX != static_cast<uint16_t>((expected.linearAdvance + 2048) >> 12)) ++advanceDiffs;
    if (metrics.width != expected.width || metrics.height != expected.height || metrics.left != expected.left ||
        metrics.top != expected.top) {
      ++boxDiffs;
      continue;
    }
    const uint8_t* ours = rasterizer.output();
    if (!expected.overlap) {
      if (std::memcmp(ours, expected.bitmap.data(), expected.bitmap.size()) != 0 && ++exactDiffs <= 3) {
        ADD_FAILURE() << "glyph " << g << ", ours then FreeType's:\n"
                      << picture(metrics, ours) << "---\n"
                      << picture(metrics, expected.bitmap.data());
      }
      continue;
    }
    ++overlapGlyphs;
    for (int p = 0; p < expected.width * expected.height; ++p) {
      ++overlapPixels;
      overlapPixelDiffs += pixelAt(ours, p) != pixelAt(expected.bitmap.data(), p);
    }
  }
  std::printf("%s @%u pt: %u glyphs (%u too large), %u load failures, %u advance diffs, %u box diffs, %u inexact; %u overlap glyphs, "
              "%.2f%% of their pixels differ\n",
              GetParam().c_str(), points, glyphs, tooLarge, loadFailures, advanceDiffs, boxDiffs, exactDiffs, overlapGlyphs,
              overlapPixels ? 100.0 * static_cast<double>(overlapPixelDiffs) / static_cast<double>(overlapPixels) : 0.0);
  EXPECT_GT(glyphs, 1000u);
  EXPECT_EQ(0u, loadFailures);
  EXPECT_EQ(0u, advanceDiffs);
  EXPECT_EQ(0u, boxDiffs);  // spec 9.3 allows 5; scaling exactly as FreeType does leaves none
  EXPECT_EQ(0u, exactDiffs);
  EXPECT_LE(overlapPixelDiffs * 100, overlapPixels * 3);  // spec 9.3: at most 3% of pixels
}

TEST_P(TtfVsFreeType, TheCmapMatchesFreeType) {
  const FreeTypeReference reference(data, kPoints);
  ASSERT_TRUE(reference.ok());
  uint32_t mapped = 0, diffs = 0;
  for (uint32_t cp = 0; cp < 0x30000; ++cp) {
    const uint32_t expected = reference.glyphIndex(cp);
    mapped += expected != 0;
    if (glyphIndex(bytes, face, cp) != expected && ++diffs <= 5) ADD_FAILURE() << "U+" << std::hex << cp;
  }
  EXPECT_GT(mapped, 200u);
  EXPECT_EQ(0u, diffs);
  EXPECT_FALSE(bytes.outOfRange());
}

TEST_P(TtfVsFreeType, LineMetricsMatchTheConverterAtEverySize) {
  for (const uint32_t points : {8u, 12u, 14u, 18u, 24u, 40u}) {
    const FreeTypeReference reference(data, points);
    ASSERT_TRUE(reference.ok());
    const LineMetrics m = lineMetrics(face, ppemFromPoints(points));
    // fontconvert_sdcard.py: advanceY = ceil(height / 64), ascender = ceil, descender = floor.
    EXPECT_EQ(reference.height() / 64, m.advanceY) << points;
    EXPECT_EQ(reference.ascender() / 64, m.ascender) << points;
    EXPECT_EQ(reference.descender() / 64, m.descender) << points;
  }
}

INSTANTIATE_TEST_SUITE_P(BundledFonts, TtfVsFreeType, ::testing::ValuesIn(bundledTtfs()), testName);

TEST(TtfPoolSize, ThePoolNeverChangesTheOutput) {
  // Spec 2: the pool size is a speed dial only. Every glyph of a text face, with three pools.
  const std::vector<uint8_t> data = readFont("Bookerly/Bookerly-Regular.ttf");
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  const uint32_t ppem = ppemFromPoints(kPoints);
  Rasterizer small, flash, sd;
  ASSERT_TRUE(small.allocate(planScratch(face, ppem, 2048)));
  ASSERT_TRUE(flash.allocate(planScratch(face, ppem, kPoolBytesFlash)));
  ASSERT_TRUE(sd.allocate(planScratch(face, ppem, kPoolBytesSd)));
  for (uint16_t g = 0; g < face.numGlyphs; ++g) {
    EpdGlyph a{}, b{}, c{};
    ASSERT_EQ(Status::Ok, small.render(bytes, face, g, ppem, &a));
    ASSERT_EQ(Status::Ok, flash.render(bytes, face, g, ppem, &b));
    ASSERT_EQ(Status::Ok, sd.render(bytes, face, g, ppem, &c));
    ASSERT_EQ(0, std::memcmp(&a, &b, sizeof a)) << g;
    ASSERT_EQ(0, std::memcmp(&a, &c, sizeof a)) << g;
    ASSERT_EQ(0, std::memcmp(small.output(), flash.output(), a.dataLength)) << g;
    ASSERT_EQ(0, std::memcmp(small.output(), sd.output(), a.dataLength)) << g;
  }
}
```

Replace `test/ttf_rasterizer/CMakeLists.txt` with:

<!-- plan-file: test/ttf_rasterizer/CMakeLists.txt -->
```cmake
# The TTF engine (lib/EpdFont/ttf): FontBytes, face parsing, the integer TrueType loader and the vendored
# ftgrays, checked against FreeType itself over every bundled TTF.

# FreeType for the comparison only, never the firmware: the SDK's copy, compiled through its own build
# wrappers as FreeInkFont's host tests compile it. Third-party code, so its warnings are not ours.
set(FREEINK_FONT ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont)
file(GLOB FREETYPE_HOST_SOURCES ${FREEINK_FONT}/src/freetype/ft_*.c)
add_library(freetype_host STATIC ${FREETYPE_HOST_SOURCES})
target_include_directories(freetype_host PUBLIC ${FREEINK_FONT}/third_party/freetype/include)
target_compile_options(freetype_host PRIVATE -w)

add_executable(TtfRasterizerTest
  TtfGraysTest.cpp
  FontBytesTest.cpp
  TtfFixedTest.cpp
  TtfFaceTest.cpp
  TtfRasterizerTest.cpp
  TtfVsFreeTypeTest.cpp
  FreeTypeReference.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFace.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfRasterizer.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/third_party/freetype/ftgrays.c
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(TtfRasterizerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/shims
)

target_compile_definitions(TtfRasterizerTest PRIVATE
  TTF_FONT_DIR="${REPO_ROOT}/lib/EpdFont/builtinFonts/source"
)

target_link_libraries(TtfRasterizerTest PRIVATE
  crosspoint_test_common
  freetype_host
  GTest::gtest_main
)

# Spec 9.3: the engine runs under AddressSanitizer and UBSan (CI's Linux GCC). Where ASan cannot link
# (MinGW), UBSan still traps on undefined behaviour without its runtime. FreeType is not instrumented.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address,undefined)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_ASAN)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=undefined -fsanitize-undefined-trap-on-error")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_UBSAN_TRAP)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(TTF_TESTS_HAVE_ASAN)
  set(TTF_SANITIZERS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
elseif(TTF_TESTS_HAVE_UBSAN_TRAP)
  set(TTF_SANITIZERS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
endif()
target_compile_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
target_link_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
message(STATUS "TtfRasterizerTest sanitizers: ${TTF_SANITIZERS}")

gtest_discover_tests(TtfRasterizerTest DISCOVERY_TIMEOUT 60)
```

- [ ] **Step 2: Run the comparison**

```bash
cmake -S test -B build/test
cmake --build build/test --target TtfRasterizerTest -j 8
./build/test/ttf_rasterizer/TtfRasterizerTest.exe --gtest_filter='*FreeType*:TtfPoolSize*'
```

Expected:
- 37 tests pass, about 20 s with MinGW.
- One line per font and size. Every one has 0 load failures, 0 advance diffs, 0 box diffs and 0 inexact.
  For example:
  - `Bookerly/Bookerly-Regular.ttf @14 pt: 1867 glyphs (0 too large), 0 load failures, 0 advance diffs, 0 box diffs, 0 inexact; 0 overlap glyphs, 0.00% of their pixels differ`
  - `NotoSans/NotoSans-Regular.ttf @14 pt: 4503 glyphs (0 too large), ..., 0 inexact; 4468 overlap glyphs, 0.77% of their pixels differ`
- The overlap glyphs (Inter and Noto Sans) differ on 0.24-1.30% of their pixels.
- The only glyph that is too large is Ubuntu's wordmark (U+F200) at 40 pt, 288 px wide, in both Ubuntu
  faces.

- [ ] **Step 3: Check that the gate bites**

Make each change below in turn, rebuild, run the step 2 command, and restore with `git checkout -- <file>`.

| Change | Fails |
| --- | --- |
| In `third_party/freetype/ftgrays.c`, delete the seven lines of the `FT_INT64` patch, from its `PATCH(witchhunt)` comment to `typedef unsigned int  FT_UInt;` | the four Bookerly and two Ubuntu faces: 24-61 inexact glyphs per face at 8 and 14 pt, about 290 at 40 pt |
| In `TtfRasterizer.cpp`, `*pp1x = mulFix(xMin - lsb, scale);` → `*pp1x = 0;` | `Bookerly_Regular_ttf`: 2-5 box diffs per size (its 5 glyphs whose `lsb` is not their `xMin`) |

- [ ] **Step 4: Run every TTF test, format and commit**

```bash
./build/test/ttf_rasterizer/TtfRasterizerTest.exe   # [  PASSED  ] 88 tests.
/c/tmp/cf21/bin/clang-format-21.exe -i test/ttf_rasterizer/FreeTypeReference.h test/ttf_rasterizer/FreeTypeReference.cpp test/ttf_rasterizer/TtfVsFreeTypeTest.cpp
git add test/ttf_rasterizer
git commit -m "test(ttf): every glyph of every bundled TTF against FreeType itself"
```

---

### Task 6: Corrupted fonts

Spec 9.3: truncated files and random bytes in every table, under AddressSanitizer and UBSan. The test asserts
only that every result is a valid status. The sanitizers are its real teeth: full ASan and UBSan on CI's
Linux GCC, UBSan in trap mode with MinGW.

**Files:**
- Create: `test/ttf_rasterizer/TtfCorruptFontTest.cpp`
- Modify: `test/ttf_rasterizer/CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Tasks 2-4; `TtfBuilder.h` and `TtfTestUtil.h` (Task 3).

- [ ] **Step 1: Write the test**

Create `test/ttf_rasterizer/TtfCorruptFontTest.cpp`:

<!-- plan-file: test/ttf_rasterizer/TtfCorruptFontTest.cpp -->
```cpp
// Broken fonts (spec 9.3): cut short anywhere, or with random bytes in any table, a font never crashes the
// engine or makes it read outside the file or its scratch block. CI runs this under AddressSanitizer and
// UBSan (see CMakeLists.txt); elsewhere UBSan traps.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "TtfBuilder.h"
#include "TtfRasterizer.h"
#include "TtfTestUtil.h"

using namespace ttf;
using namespace ttf_test;

namespace {

constexpr uint32_t kPpems[] = {ppemFromPoints(12), ppemFromPoints(40)};

// Parses `font` and, when that works, renders a spread of its glyphs at two sizes. Returns how many rendered.
int exercise(const std::vector<uint8_t>& font, Rasterizer& rasterizer) {
  FontBytes bytes(font.data(), static_cast<uint32_t>(font.size()));
  Face face;
  if (parseFace(bytes, &face) != FaceStatus::Ok) return 0;
  int rendered = 0;
  for (uint32_t cp = 0x20; cp < 0x250; cp += 7) {
    const uint16_t glyph = glyphIndex(bytes, face, cp);
    for (const uint32_t ppem : kPpems) {
      EpdGlyph metrics{};
      const Status status = rasterizer.render(bytes, face, glyph, ppem, &metrics);
      EXPECT_TRUE(status == Status::Ok || isPermanent(status)) << statusName(status);
      rendered += status == Status::Ok;
    }
  }
  return rendered;
}

// The table directory's entries, to aim the mutations.
struct TableSpan {
  uint32_t offset;
  uint32_t length;
};

std::vector<TableSpan> tables(const std::vector<uint8_t>& font) {
  std::vector<TableSpan> out;
  const uint32_t count = static_cast<uint32_t>(font[4] << 8 | font[5]);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* r = font.data() + 12 + 16 * i;
    out.push_back({static_cast<uint32_t>(r[8] << 24 | r[9] << 16 | r[10] << 8 | r[11]),
                   static_cast<uint32_t>(r[12] << 24 | r[13] << 16 | r[14] << 8 | r[15])});
  }
  return out;
}

// A small linear congruential generator: the same mutations on every run and every platform.
struct Random {
  uint32_t state;
  uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  }
  uint32_t below(const uint32_t n) { return n ? next() % n : 0; }
};

Rasterizer& sharedRasterizer() {
  static Rasterizer rasterizer;
  if (!rasterizer.allocated()) {
    rasterizer.allocate({kMaxScratchPoints, kMaxScratchContours, kMaxOutputBytes, kPoolBytesFlash});
  }
  return rasterizer;
}

}  // namespace

TEST(TtfCorruptFont, TheIntactFontRenders) {
  EXPECT_GT(exercise(readFont("Bookerly/Bookerly-Regular.ttf"), sharedRasterizer()), 100);
}

TEST(TtfCorruptFont, AFontCutShortAnywhereIsSafe) {
  const std::vector<uint8_t> font = readFont("Bookerly/Bookerly-Regular.ttf");
  std::vector<uint32_t> cuts = {0, 1, 4, 11, 12, 13, 100, 200};
  for (const TableSpan& t : tables(font)) {
    for (const uint32_t at : {t.offset, t.offset + 1, t.offset + t.length / 2, t.offset + t.length - 1}) {
      cuts.push_back(at);
    }
  }
  cuts.push_back(static_cast<uint32_t>(font.size() - 1));
  for (const uint32_t cut : cuts) {
    if (cut > font.size()) continue;
    const std::vector<uint8_t> shortened(font.begin(), font.begin() + cut);
    exercise(shortened, sharedRasterizer());
  }
}

TEST(TtfCorruptFont, RandomBytesInAnyTableAreSafe) {
  const std::vector<std::string> sources = {"Bookerly/Bookerly-Regular.ttf", "NotoSans/NotoSans-Italic.ttf",
                                            "Inter/Inter-Bold.ttf"};
  Random random{20261014};
  for (const std::string& source : sources) {
    const std::vector<uint8_t> original = readFont(source);
    const std::vector<TableSpan> spans = tables(original);
    for (int round = 0; round < 150; ++round) {
      std::vector<uint8_t> font = original;
      const TableSpan& t = spans[random.below(static_cast<uint32_t>(spans.size()))];
      const uint32_t changes = 1 + random.below(8);
      for (uint32_t i = 0; i < changes && t.length > 0; ++i) {
        font[t.offset + random.below(t.length)] = static_cast<uint8_t>(random.next());
      }
      exercise(font, sharedRasterizer());
    }
  }
}

TEST(TtfCorruptFont, RandomBytesInTheDirectoryAreSafe) {
  const std::vector<uint8_t> original = readFont("Bookerly/Bookerly-Regular.ttf");
  Random random{7};
  const uint32_t directoryEnd = 12 + 16 * static_cast<uint32_t>(original[4] << 8 | original[5]);
  for (int round = 0; round < 300; ++round) {
    std::vector<uint8_t> font = original;
    font[random.below(directoryEnd)] = static_cast<uint8_t>(random.next());
    exercise(font, sharedRasterizer());
  }
}

TEST(TtfCorruptFont, HugeCoordinatesAreRefusedNotOverflowed) {
  Font font;
  font.glyphs.resize(2);
  font.glyphs[1].contours = {{{-32768, -32768}, {32767, -32768}, {32767, 32767}, {-32768, 32767}}};
  font.unitsPerEm = 16;
  font.cmap = {{'A', 1}};
  const Bytes data = buildFont(font);
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  EpdGlyph metrics{};
  EXPECT_EQ(Status::Unsupported, sharedRasterizer().render(bytes, face, 1, kMaxPpem26_6, &metrics));
}
```

Replace `test/ttf_rasterizer/CMakeLists.txt` with:

<!-- plan-file: test/ttf_rasterizer/CMakeLists.txt -->
```cmake
# The TTF engine (lib/EpdFont/ttf): FontBytes, face parsing, the integer TrueType loader and the vendored
# ftgrays, checked against FreeType itself over every bundled TTF.

# FreeType for the comparison only, never the firmware: the SDK's copy, compiled through its own build
# wrappers as FreeInkFont's host tests compile it. Third-party code, so its warnings are not ours.
set(FREEINK_FONT ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont)
file(GLOB FREETYPE_HOST_SOURCES ${FREEINK_FONT}/src/freetype/ft_*.c)
add_library(freetype_host STATIC ${FREETYPE_HOST_SOURCES})
target_include_directories(freetype_host PUBLIC ${FREEINK_FONT}/third_party/freetype/include)
target_compile_options(freetype_host PRIVATE -w)

add_executable(TtfRasterizerTest
  TtfGraysTest.cpp
  FontBytesTest.cpp
  TtfFixedTest.cpp
  TtfFaceTest.cpp
  TtfRasterizerTest.cpp
  TtfVsFreeTypeTest.cpp
  TtfCorruptFontTest.cpp
  FreeTypeReference.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFace.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfRasterizer.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/third_party/freetype/ftgrays.c
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(TtfRasterizerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/shims
)

target_compile_definitions(TtfRasterizerTest PRIVATE
  TTF_FONT_DIR="${REPO_ROOT}/lib/EpdFont/builtinFonts/source"
)

target_link_libraries(TtfRasterizerTest PRIVATE
  crosspoint_test_common
  freetype_host
  GTest::gtest_main
)

# Spec 9.3: the engine runs under AddressSanitizer and UBSan (CI's Linux GCC). Where ASan cannot link
# (MinGW), UBSan still traps on undefined behaviour without its runtime. FreeType is not instrumented.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address,undefined)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_ASAN)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=undefined -fsanitize-undefined-trap-on-error")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_UBSAN_TRAP)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(TTF_TESTS_HAVE_ASAN)
  set(TTF_SANITIZERS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
elseif(TTF_TESTS_HAVE_UBSAN_TRAP)
  set(TTF_SANITIZERS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
endif()
target_compile_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
target_link_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
message(STATUS "TtfRasterizerTest sanitizers: ${TTF_SANITIZERS}")

gtest_discover_tests(TtfRasterizerTest DISCOVERY_TIMEOUT 60)
```

- [ ] **Step 2: Run it**

```bash
cmake -S test -B build/test
cmake --build build/test --target TtfRasterizerTest -j 8
./build/test/ttf_rasterizer/TtfRasterizerTest.exe --gtest_filter='TtfCorruptFont*'
```

Expected: `[  PASSED  ] 5 tests.` in a few seconds.

- [ ] **Step 3: Check that it bites**

In `FontBytes.h`, make `contains()` return `true`, rebuild and run the step 2 command. Expected: the run
fails. The cut-short fonts are read past their end, and the process stops with a non-zero exit, a crash or
a UBSan trap. Restore with `git checkout -- lib/EpdFont/ttf/FontBytes.h`.

- [ ] **Step 4: Run every TTF test, format and commit**

```bash
./build/test/ttf_rasterizer/TtfRasterizerTest.exe   # [  PASSED  ] 93 tests.
/c/tmp/cf21/bin/clang-format-21.exe -i test/ttf_rasterizer/TtfCorruptFontTest.cpp
git add test/ttf_rasterizer
git commit -m "test(ttf): cut-short and randomly damaged fonts never crash the engine"
```

---

### Task 7: The no-float build check, and the firmware builds

**Files:**
- Create: `scripts/check_no_float.py`
- Modify: `platformio.ini` (`extra_scripts`)

**Interfaces:**
- Produces: `python scripts/check_no_float.py <nm> <build dir>`, which exits 1 and names the symbols when an
  object under `EpdFont/ttf/` references a soft-float or libm routine. PlatformIO runs the same check after
  every link.

- [ ] **Step 1: Make a soft-float object for the check to catch**

```bash
T=/tmp/nofloat && rm -rf $T && mkdir -p $T/lib1/EpdFont/ttf
printf 'int scaled(int v, int ppem) { return (int)(v * (ppem / 1000.0f)); }\n' > $T/f.c
RV=/c/pio/packages/toolchain-riscv32-esp/bin
$RV/riscv32-esp-elf-gcc.exe -march=rv32imc_zicsr_zifencei -mabi=ilp32 -Os -c $T/f.c -o $T/lib1/EpdFont/ttf/Bad.cpp.o
python scripts/check_no_float.py $RV/riscv32-esp-elf-nm.exe $T
```

Expected: `python: can't open file '.../scripts/check_no_float.py'`. The `-march` flag matters: the
toolchain defaults to an FPU target, while the firmware builds for `rv32imc`.

- [ ] **Step 2: Write the check**

Create `scripts/check_no_float.py`:

<!-- plan-file: scripts/check_no_float.py -->
```python
"""Fails the firmware build when the TTF engine (lib/EpdFont/ttf) uses floating point (spec 5.2).

The C3 has no FPU: float or double maths would pull in libgcc's slow soft-float routines, which the engine
was designed to avoid. The linker map cannot show this. It names only the first object that pulled in each
archive member, and other code (SdFat, the battery monitor) already pulls in the double routines. So this
script reads the engine's own objects instead: any undefined soft-float or libm symbol in one of them fails
the build.

PlatformIO runs it after every link (platformio.ini: extra_scripts = post:scripts/check_no_float.py).
By hand:
    python scripts/check_no_float.py <path to nm> <build dir>
"""

import os
import re
import subprocess
import sys

# libgcc's soft-float entry points: __addsf3, __muldf3, __eqsf2, __fixdfsi, __floatsisf, __extendsfdf2, ...
SOFT_FLOAT = re.compile(
    r"^__(?:(?:add|sub|mul|div|neg|eq|ne|ge|gt|le|lt|unord|cmp)(?:sf|df)\d"
    r"|(?:fix|fixuns)(?:sf|df)(?:si|di)"
    r"|float(?:un)?(?:si|di)(?:sf|df)"
    r"|extendsfdf2|truncdfsf2)$"
)
LIBM = re.compile(
    r"^(?:sqrt|cbrt|hypot|pow|exp|exp2|log|log2|log10|sin|cos|tan|asin|acos|atan|atan2"
    r"|floor|ceil|round|lround|trunc|fmod|ldexp|frexp|modf)f?$"
)
ENGINE_DIR = "/EpdFont/ttf/"


def engine_objects(build_dir):
    """Every object compiled from lib/EpdFont/ttf, its third_party folder included."""
    for root, _, files in os.walk(build_dir):
        path = root.replace("\\", "/") + "/"
        if ENGINE_DIR not in path:
            continue
        for name in files:
            if name.endswith(".o"):
                yield os.path.join(root, name)


def float_symbols(nm, obj, env=None):
    out = subprocess.run([nm, "-u", obj], capture_output=True, text=True, check=True, env=env).stdout
    symbols = {line.split()[-1] for line in out.splitlines() if line.strip()}
    return sorted(s for s in symbols if SOFT_FLOAT.match(s) or LIBM.match(s))


def check(nm, build_dir, env=None):
    """Prints the verdict; returns the number of offending objects."""
    objects = sorted(engine_objects(build_dir))
    bad = 0
    for obj in objects:
        found = float_symbols(nm, obj, env)
        if found:
            bad += 1
            print(f"check_no_float: {os.path.relpath(obj, build_dir)} uses floating point: {', '.join(found)}")
    if not objects:
        print("check_no_float: this build has no TTF engine objects; nothing to check")
        return 0
    if not bad:
        print(f"check_no_float: {len(objects)} TTF engine objects, no floating point")
    return bad


try:
    Import("env")  # noqa: F821 -- provided by PlatformIO when it runs this as an extra script
except NameError:
    env = None

if env is not None:

    def _after_link(target, source, env):  # noqa: ARG001 -- SCons action signature
        nm = env.WhereIs(env.subst("$CC").replace("gcc", "nm"))
        if not nm:
            print("check_no_float: cannot find the toolchain's nm")
            return 1
        return 1 if check(nm, env.subst("$BUILD_DIR"), env["ENV"]) else 0

    env.AddPostAction(  # noqa: F821
        "$BUILD_DIR/${PROGNAME}.elf", env.VerboseAction(_after_link, "Checking the TTF engine for floating point")
    )
elif __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(1 if check(sys.argv[1], sys.argv[2]) else 0)
```

- [ ] **Step 3: Run it on the soft-float object**

Run the last line of step 1 again.
Expected: `check_no_float: lib1/EpdFont/ttf/Bad.cpp.o uses floating point: __divsf3, __fixsfsi, __floatsisf, __mulsf3`
and exit code 1 (`echo $?`).

- [ ] **Step 4: Run it after every link**

In `platformio.ini`, add the script after the unit-test target's:

<!-- plan-edit: platformio.ini -->
```ini
  post:scripts/register_unit_tests_target.py
```
with:

```ini
  post:scripts/register_unit_tests_target.py
  post:scripts/check_no_float.py
```

- [ ] **Step 5: Build the firmware**

```bash
export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8
"$HOME/.platformio/penv/Scripts/pio.exe" run -e default > /tmp/pio-default.log 2>&1; echo rc=$?
"$HOME/.platformio/penv/Scripts/pio.exe" run -e x4pro > /tmp/pio-x4pro.log 2>&1; echo rc=$?
grep -E "check_no_float|RAM:|Flash:" /tmp/pio-default.log /tmp/pio-x4pro.log
```

Run them in the background. A fresh worktree takes 10-20 minutes for `default`.

Expected:
- `rc=0` twice.
- `Checking the TTF engine for floating point`, then
  `check_no_float: 5 TTF engine objects, no floating point` in each log: `FlashGlyphStore`,
  `GlyphStoreFormat`, `TtfFace`, `TtfRasterizer` and `ftgrays`.
- RAM and flash within a few hundred bytes of master, since nothing calls the engine yet. Note both
  numbers for the PR.

- [ ] **Step 6: Commit**

```bash
git add scripts/check_no_float.py platformio.ini
git commit -m "build: fail the link when the TTF engine uses floating point"
```

---

### Task 8: Device bench

The bench renders every glyph of Bookerly Regular at 14 and 40 pt on the X3, from the font embedded in the
image. It reports speed, the scratch block, heap and stack, and a hash of the 14 pt output. The host test
computes the same hash, so equal hashes mean the C3 draws exactly what the host checked against FreeType.

**Files:**
- Create: `bench/ttf_raster_golden.h`, `bench/ttf_raster_main.cpp`
- Modify: `test/ttf_rasterizer/TtfVsFreeTypeTest.cpp`, `test/ttf_rasterizer/CMakeLists.txt`, `platformio.ini`, `docs/contributing/getting-started.md`

**Interfaces:**
- Consumes: everything from Tasks 2-4.
- Produces: `ttf_golden::kSeed`, `ttf_golden::kBookerlyRegular14pt` and
  `ttf_golden::hashGlyph(hash, glyph, metrics, bitmap)`; env `bench_ttf_raster`.

- [ ] **Step 1: Write the failing host test**

In `test/ttf_rasterizer/TtfVsFreeTypeTest.cpp`, add the golden header after the test utilities:

<!-- plan-edit: test/ttf_rasterizer/TtfVsFreeTypeTest.cpp -->
```cpp
#include "TtfTestUtil.h"
```
with:

```cpp
#include "TtfTestUtil.h"
#include "ttf_raster_golden.h"
```

and append this test at the end of the file:

<!-- plan-append: test/ttf_rasterizer/TtfVsFreeTypeTest.cpp -->
```cpp
TEST(TtfGolden, TheDeviceBenchExpectsThisOutput) {
  // bench/ttf_raster_main.cpp computes the same hash on the device, over the same glyphs.
  const std::vector<uint8_t> data = readFont("Bookerly/Bookerly-Regular.ttf");
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  Rasterizer rasterizer;
  ASSERT_TRUE(rasterizer.allocate(planScratch(face, ppemFromPoints(40), kPoolBytesFlash)));
  uint32_t hash = ttf_golden::kSeed;
  for (uint16_t g = 0; g < face.numGlyphs; ++g) {
    EpdGlyph metrics{};
    if (rasterizer.render(bytes, face, g, ppemFromPoints(kPoints), &metrics) != Status::Ok) continue;
    hash = ttf_golden::hashGlyph(hash, g, metrics, rasterizer.output());
  }
  EXPECT_EQ(ttf_golden::kBookerlyRegular14pt, hash) << std::hex << hash;
}
```

Replace `test/ttf_rasterizer/CMakeLists.txt` with (it adds `${REPO_ROOT}/bench` to the include path):

<!-- plan-file: test/ttf_rasterizer/CMakeLists.txt -->
```cmake
# The TTF engine (lib/EpdFont/ttf): FontBytes, face parsing, the integer TrueType loader and the vendored
# ftgrays, checked against FreeType itself over every bundled TTF.

# FreeType for the comparison only, never the firmware: the SDK's copy, compiled through its own build
# wrappers as FreeInkFont's host tests compile it. Third-party code, so its warnings are not ours.
set(FREEINK_FONT ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont)
file(GLOB FREETYPE_HOST_SOURCES ${FREEINK_FONT}/src/freetype/ft_*.c)
add_library(freetype_host STATIC ${FREETYPE_HOST_SOURCES})
target_include_directories(freetype_host PUBLIC ${FREEINK_FONT}/third_party/freetype/include)
target_compile_options(freetype_host PRIVATE -w)

add_executable(TtfRasterizerTest
  TtfGraysTest.cpp
  FontBytesTest.cpp
  TtfFixedTest.cpp
  TtfFaceTest.cpp
  TtfRasterizerTest.cpp
  TtfVsFreeTypeTest.cpp
  TtfCorruptFontTest.cpp
  FreeTypeReference.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFace.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfRasterizer.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/third_party/freetype/ftgrays.c
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(TtfRasterizerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/bench
)

target_compile_definitions(TtfRasterizerTest PRIVATE
  TTF_FONT_DIR="${REPO_ROOT}/lib/EpdFont/builtinFonts/source"
)

target_link_libraries(TtfRasterizerTest PRIVATE
  crosspoint_test_common
  freetype_host
  GTest::gtest_main
)

# Spec 9.3: the engine runs under AddressSanitizer and UBSan (CI's Linux GCC). Where ASan cannot link
# (MinGW), UBSan still traps on undefined behaviour without its runtime. FreeType is not instrumented.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address,undefined)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_ASAN)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=undefined -fsanitize-undefined-trap-on-error")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_UBSAN_TRAP)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(TTF_TESTS_HAVE_ASAN)
  set(TTF_SANITIZERS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
elseif(TTF_TESTS_HAVE_UBSAN_TRAP)
  set(TTF_SANITIZERS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
endif()
target_compile_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
target_link_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
message(STATUS "TtfRasterizerTest sanitizers: ${TTF_SANITIZERS}")

gtest_discover_tests(TtfRasterizerTest DISCOVERY_TIMEOUT 60)
```

- [ ] **Step 2: Run it to see it fail**

Run: `cmake -S test -B build/test && cmake --build build/test --target TtfRasterizerTest -j 8`
Expected: `ttf_raster_golden.h: No such file or directory`.

- [ ] **Step 3: Write the shared hash**

Create `bench/ttf_raster_golden.h`. `kBookerlyRegular14pt` is the value the host computed while writing this
plan:

<!-- plan-file: bench/ttf_raster_golden.h -->
```cpp
#pragma once

// What the TTF engine draws for every glyph of Bookerly Regular at 14 pt, as one FNV-1a hash. The host test
// (test/ttf_rasterizer/TtfVsFreeTypeTest.cpp) computes it from the source font, and so does the device bench
// (bench/ttf_raster_main.cpp): equal hashes mean the device draws exactly what the host checked against
// FreeType. A change to the engine's output changes it; update kBookerlyRegular14pt from the host test.

#include <cstdint>

#include "EpdFontData.h"

namespace ttf_golden {

constexpr uint32_t kSeed = 2166136261u;
constexpr uint32_t kBookerlyRegular14pt = 0xDE1AEEA7u;

inline uint32_t mix(const uint32_t hash, const uint8_t byte) { return (hash ^ byte) * 16777619u; }

/// Folds one glyph into `hash`: its index, its metrics (each as 4 little-endian bytes) and its bitmap.
inline uint32_t hashGlyph(uint32_t hash, const uint16_t glyph, const EpdGlyph& metrics, const uint8_t* bitmap) {
  const int32_t fields[] = {glyph, metrics.width, metrics.height, metrics.advanceX, metrics.left, metrics.top};
  for (const int32_t field : fields) {
    for (int shift = 0; shift < 32; shift += 8) hash = mix(hash, static_cast<uint8_t>(field >> shift));
  }
  for (uint32_t i = 0; i < metrics.dataLength; ++i) hash = mix(hash, bitmap[i]);
  return hash;
}

}  // namespace ttf_golden
```

- [ ] **Step 4: Run the host tests**

Run: `cmake --build build/test --target TtfRasterizerTest -j 8 && ./build/test/ttf_rasterizer/TtfRasterizerTest.exe`
Expected: `[  PASSED  ] 94 tests.`

- [ ] **Step 5: Write the bench and its env**

Create `bench/ttf_raster_main.cpp`:

<!-- plan-file: bench/ttf_raster_main.cpp -->
```cpp
// Device bench for the TTF rasterizer (Stage 2b): every glyph of Bookerly Regular at 14 pt (the body size) and
// at 40 pt (a large heading). The font is embedded in the image (board_build.embed_files), so it is read
// through the flash cache, as the glyph store's mapped styles will be. Prints timings, the scratch block, the
// heap and the loop task's stack, and a hash of the output that must equal the host's
// (bench/ttf_raster_golden.h). Ends with "=== done ===".
//
// Build & flash: pio run -e bench_ttf_raster -t upload
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <ttf/TtfRasterizer.h>

#include "ttf_raster_golden.h"

extern const uint8_t kFontStart[] asm("_binary_lib_EpdFont_builtinFonts_source_Bookerly_Bookerly_Regular_ttf_start");
extern const uint8_t kFontEnd[] asm("_binary_lib_EpdFont_builtinFonts_source_Bookerly_Bookerly_Regular_ttf_end");

using namespace ttf;

namespace {

struct Run {
  uint32_t ok = 0;
  uint32_t failed = 0;
  int64_t totalUs = 0;
  int64_t maxUs = 0;
  uint64_t pixels = 0;
  uint32_t hash = ttf_golden::kSeed;
};

Run renderAll(Rasterizer& rasterizer, FontBytes& bytes, const Face& face, const uint32_t ppem26_6) {
  Run run;
  for (uint32_t g = 0; g < face.numGlyphs; ++g) {
    EpdGlyph metrics{};
    const int64_t start = esp_timer_get_time();
    const Status status = rasterizer.render(bytes, face, static_cast<uint16_t>(g), ppem26_6, &metrics);
    const int64_t us = esp_timer_get_time() - start;
    if (status != Status::Ok) {
      ++run.failed;
      continue;
    }
    ++run.ok;
    run.totalUs += us;
    run.maxUs = us > run.maxUs ? us : run.maxUs;
    run.pixels += static_cast<uint64_t>(metrics.width) * metrics.height;
    run.hash = ttf_golden::hashGlyph(run.hash, static_cast<uint16_t>(g), metrics, rasterizer.output());
  }
  return run;
}

void report(const char* name, const Run& run) {
  Serial.printf("BENCH %-20s glyphs=%u failed=%u avg=%lldus max=%lldus px/glyph=%u total=%lldms hash=%08x\n", name,
                static_cast<unsigned>(run.ok), static_cast<unsigned>(run.failed),
                run.ok ? run.totalUs / run.ok : 0LL, run.maxUs, run.ok ? static_cast<unsigned>(run.pixels / run.ok) : 0u,
                run.totalUs / 1000, static_cast<unsigned>(run.hash));
}

unsigned freeHeap() { return static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)); }
unsigned largestBlock() { return static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)); }

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== TTF rasterizer device bench (Stage 2b) ===");

  FontBytes bytes(kFontStart, static_cast<uint32_t>(kFontEnd - kFontStart));
  Face face;
  const FaceStatus parsed = parseFace(bytes, &face);
  Serial.printf("font %u B, parse %s, %u glyphs, %u units per em, maxp %u points %u contours\n",
                static_cast<unsigned>(bytes.size()), parsed == FaceStatus::Ok ? "ok" : "FAILED",
                static_cast<unsigned>(face.numGlyphs), static_cast<unsigned>(face.unitsPerEm),
                static_cast<unsigned>(face.maxPoints), static_cast<unsigned>(face.maxContours));
  if (parsed != FaceStatus::Ok) {
    Serial.println("=== done ===");
    return;
  }

  const uint32_t body = ppemFromPoints(14);
  const uint32_t heading = ppemFromPoints(40);
  Rasterizer rasterizer;
  const unsigned heapBefore = freeHeap();
  if (!rasterizer.allocate(planScratch(face, heading, kPoolBytesFlash))) {
    Serial.println("FATAL: scratch block");
    Serial.println("=== done ===");
    return;
  }
  Serial.printf("scratch %u B (heap %u -> %u B, largest block %u B); output region %u B\n",
                static_cast<unsigned>(rasterizer.scratchBytes()), heapBefore, freeHeap(), largestBlock(),
                static_cast<unsigned>(rasterizer.plan().outputBytes));

  const unsigned stackBefore = static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr));
  const Run text = renderAll(rasterizer, bytes, face, body);
  report("render_14pt_pool4k", text);
  Serial.printf("output hash %08x: %s the host's %08x\n", static_cast<unsigned>(text.hash),
                text.hash == ttf_golden::kBookerlyRegular14pt ? "MATCHES" : "DIFFERS FROM",
                static_cast<unsigned>(ttf_golden::kBookerlyRegular14pt));
  report("render_40pt_pool4k", renderAll(rasterizer, bytes, face, heading));
  Serial.printf("loop task stack: lowest free %u B before rendering, %u B after\n", stackBefore,
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

  // The pool is a speed dial (spec 2): the same glyphs with 2 KB and with the SD mode's 8 KB.
  rasterizer.allocate(planScratch(face, heading, 2048));
  report("render_14pt_pool2k", renderAll(rasterizer, bytes, face, body));
  rasterizer.allocate(planScratch(face, heading, kPoolBytesSd));
  report("render_14pt_pool8k", renderAll(rasterizer, bytes, face, body));
  report("render_40pt_pool8k", renderAll(rasterizer, bytes, face, heading));
  rasterizer.release();
  Serial.printf("after release: heap %u B\n", freeHeap());
  Serial.println("=== done ===");
}

void loop() { delay(30000); }
```

In `platformio.ini`, add the env after `bench_glyph_store`. `board_build.embed_files` links the font into the
image as `_binary_<path with every non-alphanumeric as _>_start`:

<!-- plan-edit: platformio.ini -->
```ini
[env:bench_glyph_store]
; bench_font's build with another sketch.
extends = env:bench_font
build_src_filter = -<*> +<../bench/glyph_store_main.cpp> +<platform/skip_efuse_blk_check.c>
```
with:

```ini
[env:bench_glyph_store]
; bench_font's build with another sketch.
extends = env:bench_font
build_src_filter = -<*> +<../bench/glyph_store_main.cpp> +<platform/skip_efuse_blk_check.c>

[env:bench_ttf_raster]
; bench_font's build with another sketch, and Bookerly Regular embedded in the image for it to render.
extends = env:bench_font
build_src_filter = -<*> +<../bench/ttf_raster_main.cpp> +<platform/skip_efuse_blk_check.c>
board_build.embed_files = lib/EpdFont/builtinFonts/source/Bookerly/Bookerly-Regular.ttf
```

In `docs/contributing/getting-started.md`, the measurement envs paragraph (line 82):

<!-- plan-edit: docs/contributing/getting-started.md -->
```markdown
Four more C3 environments are for measurement, not for daily use: `slim` is a release-style build with serial logging compiled out, and `bench` and `bench_font` flash a standalone benchmark that prints timings over serial (no SD card needed). `bench_glyph_store` is a self-test of the TTF glyph store on the real `spiffs` partition; it erases the `.cpfont` flash cache, which the reader copies back from the SD card the next time it needs it.
```
with:

```markdown
Five more C3 environments are for measurement, not for daily use: `slim` is a release-style build with serial logging compiled out, and `bench` and `bench_font` flash a standalone benchmark that prints timings over serial (no SD card needed). `bench_glyph_store` is a self-test of the TTF glyph store on the real `spiffs` partition; it erases the `.cpfont` flash cache, which the reader copies back from the SD card the next time it needs it. `bench_ttf_raster` renders every glyph of Bookerly with the TTF engine and checks the output against the host tests'.
```

- [ ] **Step 6: Build, flash and capture on the X3**

```bash
export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8
"$HOME/.platformio/penv/Scripts/pio.exe" run -e bench_ttf_raster -t upload
/c/pio/penv/Scripts/python.exe /c/tmp/capture_reset.py COM6 /tmp/ttf-bench.log 120
```

`capture_reset.py` opens the port with DTR and RTS held low, resets the C3 once, and stops at
`=== done ===`. The memory note `serial-log-capture-c3` has the details.

Expected in `/tmp/ttf-bench.log`, as measured on the X3 while writing this plan (timings vary by a few
percent):

```text
font 450048 B, parse ok, 1867 glyphs, 1000 units per em, maxp 192 points 22 contours
scratch 9964 B (heap 264744 -> 254500 B, largest block 118772 B); output region 4096 B
BENCH render_14pt_pool4k   glyphs=1867 failed=0 avg=460us max=2452us px/glyph=389 total=860ms hash=de1aeea7
output hash de1aeea7: MATCHES the host's de1aeea7
BENCH render_40pt_pool4k   glyphs=1867 failed=0 avg=1198us max=5786us px/glyph=3002 total=2237ms hash=5ea2e05b
loop task stack: lowest free 14568 B before rendering, 14488 B after
BENCH render_14pt_pool2k   glyphs=1867 failed=0 avg=512us max=2548us px/glyph=389 total=957ms hash=de1aeea7
BENCH render_14pt_pool8k   glyphs=1867 failed=0 avg=454us max=1170us px/glyph=389 total=847ms hash=de1aeea7
BENCH render_40pt_pool8k   glyphs=1867 failed=0 avg=1080us max=4192us px/glyph=3002 total=2016ms hash=5ea2e05b
after release: heap 264744 B
=== done ===
```

What must hold:
- The hash `MATCHES`, so the C3 draws exactly what the host checked against FreeType.
- `failed=0` on every line, and every pool gives the same hash.
- Rendering takes the loop task's stack less than 100 B below its earlier low.
- The heap is whole again after `release()`.

Quote the speeds in the PR next to step 0's (spec Appendix A: 429 µs per 29 px Bookerly glyph with a 4 KB
pool). Step 0 timed a sample of text glyphs; this bench times all 1,867 glyphs of the face.

The engine's flash cost (spec 6.3 asks for it in the PR): the bench is the first image that links it.

```bash
/c/pio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-nm.exe -C -S --size-sort .pio/build/bench_ttf_raster/firmware.elf \
  | grep -E "ttf::|gray_|ttf_grays|FontBytes" | awk '{s += strtonum("0x" $2)} END {print s " B"}'
```

Expected: about 9,200 B (9,246 B while writing this plan; step 0's prototype was 6.4 KB).

- [ ] **Step 7: Restore the reader**

Run: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default -t upload`

- [ ] **Step 8: Format and commit**

```bash
/c/tmp/cf21/bin/clang-format-21.exe -i bench/ttf_raster_main.cpp bench/ttf_raster_golden.h test/ttf_rasterizer/TtfVsFreeTypeTest.cpp
git add bench/ttf_raster_main.cpp bench/ttf_raster_golden.h test/ttf_rasterizer platformio.ini docs/contributing/getting-started.md
git commit -m "bench(ttf): device bench for the rasterizer, checked against the host's output"
```

---

### Task 9: Docs: the spec and this plan (branch `docs/ttf-fonts-design`)

Done in the docs worktree (`C:/tmp/wt-ttf-docs`), not on `feat/ttf-rasterizer`.

- [ ] **Step 1: Update the spec**

In `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md`:
- **Section 8:** Stage 2b's status becomes the PR's number. Stage 2c's content keeps `FontBytes`' SD mode and drops
  the rest of `FontBytes`.
- **Section 5.1:** the flash mode exists (2b); the SD mode comes in 2c.
- **Section 5.2:**
  - `ttf::Rasterizer` owns the scratch block, with `load()` then `rasterize(dst)` (decisions 2-3).
  - The vendored `ftgrays` defines `FT_INT64` (decision 4).
  - Scaling is FreeType's exact `FT_MulFix`/`FT_DivFix` with the `xMin - lsb` shift (decision 5).
  - The no-float check reads the objects with `nm` (decision 7).
- **Section 5.6 and 11:** add Apple's scaled component offsets, which report `Unsupported`.
- **Section 6.1:** a transient failure is `Status::Transient` and includes "no scratch block yet".
- **Section 9.3:** the gate as built (decision 6), with the measured figures. Every glyph without overlap
  flags is byte-identical at 8, 14 and 40 pt, with 0 box and advance differences. Overlap glyphs differ on
  0.24-1.30% of pixels.
- **Appendix A:** add the bench's device numbers from Task 8.

- [ ] **Step 2: Mark this plan done**

Add a status note at the top of this plan, as the Stage 2a plan has: tasks done, the audit's outcome, and
where the code differs from the task code.

- [ ] **Step 3: Commit and push**

```bash
git -C /c/tmp/wt-ttf-docs add docs/superpowers
git -C /c/tmp/wt-ttf-docs commit -m "docs(ttf): Stage 2b as built"
git -C /c/tmp/wt-ttf-docs push
```
