# TTF Step 0: device benchmark (throwaway). Implementation plan

> **Status: done, 2026-10-13.** The numbers are in the spec's Appendix A. The bench is on branch
> `bench/ttf-step0`; `bench/ttf/README.md` there says how to regenerate its data and re-run it. The
> later runs added pool sizes, a cache-emptied render, a page comparison, the kerning options and SD
> reads of real CJK glyph records beyond what the tasks below describe.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure on the X3 the costs the TTF design's gates depend on (spec 9.1), and record them so
the Stage 2 plans start from device numbers. The numbers:

- µs per glyph, rendered to 2-bit, for Latin and CJK;
- the loop task's stack use;
- flash erase and program times;
- the cost of a hash probe through mapped flash;
- the cost of a GPOS kerning lookup;
- the cost of a random 512 B SD read;
- the mapping budget of the reader image.

**Architecture:**

- **A standalone PlatformIO env, `bench_ttf`,** modelled on `bench_font`. It runs one `setup()` that
  prints results over serial and then idles.
- **The renderer** is the verified prototype: an integer TrueType loader plus FreeType's `ftgrays.c`
  built standalone with a 2 KB cell pool, rendering through the span callback straight into 2-bit.
- **Fonts are embedded as C arrays,** so they are read through the flash cache, exactly as a
  memory-mapped partition would be.
- **The full CJK font is read from the SD card** only for the random-read timing.
- **Everything lives on a local branch that is never pushed or merged.**

**Tech Stack:** PlatformIO (ESP32-C3, Arduino + ESP-IDF APIs: `esp_partition_*`, `esp_timer`,
`spi_flash_mmap_get_free_pages`), FreeType 2.13.3 `ftgrays.c` (FTL), FreeInkFont `Gpos.cpp`, Python 3
with fontTools 4.63 and pyserial (PlatformIO penv).

**Spec:** `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md`, sections 9.1 (measurements and
gates), 4.1 (mapping budget), 5.2 and 5.3 (engine and scratch), on branch `docs/ttf-fonts-design`.

## Global Constraints

- **Throwaway:** branch `bench/ttf-step0`, created from `master`, **never pushed, never merged**. No
  file of this plan lands in `master`.
- **Gates** (spec 9.1, copied verbatim):
  - "If a glyph costs more than about 2 ms all-in (render plus write), or a CJK first chapter would add
    more than about 10 s to its build, pull the deferred metrics-only layout mode (section 11) into
    Stage 2c."
  - "If fewer than about 25 mapping pages are free, shrink the store regions in 4.1."
- **Sizes:** pixels per em = pt × 150 / 72. 14 pt = 29 px/em; 26 pt (the largest rung) = 54 px/em.
- **No float in printouts.** Print integers (ns or µs) only.
- **Toolchain** (Git Bash):
  - `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8` before every `pio`
    command;
  - pio is `"$HOME/.platformio/penv/Scripts/pio.exe"`;
  - host C builds use `/c/msys64/ucrt64/bin` gcc;
  - serial capture uses `/c/pio/penv/Scripts/python.exe` (it has pyserial).
- **Device:** X3 on COM6.
  - The benchmark **replaces the reader firmware** until Task 4 reflashes it.
  - It **erases the first 1 MB of the `spiffs` partition**, which holds the `.cpfont` flash cache. The
    reader re-caches a family the next time it loads one. The language slot (last 64 KB of the
    partition) is never touched.
- **User actions** are marked USER ACTION. A subagent must stop there and ask.

## Review Focus

1. **The optimiser deleting a timed loop.** Every loop folds its result into `sink`, which is printed at
   the end. A loop that times at almost zero is the symptom. Pinned in Task 2's code: every loop writes
   `sink`.
2. **Cold versus warm flash cache.** A glyph met for the first time in a chapter build is read cold.
   The sketch prints both cold (first pass) and warm (mean of 5 more passes) timings, and Task 5's
   all-in estimates use the **cold** figure.
3. **A CJK sample that silently is not TrueType.** A CFF `.otf` has no `glyf` table, so the loader
   would render nothing. `make_cjk_subset.py` exits with an error in that case (Task 2, Step 2), and
   the sketch prints the number of glyphs it actually rendered.
4. **No CJK file on the card.** The SD timing prints `SKIPPED` rather than a number. Task 5 refuses to
   evaluate the CJK gate until it has a real number (Task 4, Step 3).
5. **A mapping-budget formula that is plausible but wrong.** Task 3 computes free pages from the ELF.
   Task 4 checks the same formula against the device's own `spi_flash_mmap_get_free_pages` for the
   bench image. If the two disagree by more than 1 page, the reader-image figure is not trusted.

---

### Task 1: Bench branch, renderer sources and the host span check

**Files:**
- Create: `bench/ttf/ittf.h`, `bench/ttf/ittf.c` (Appendix A, verbatim)
- Create: `bench/ttf/ftstandalone.h`, `bench/ttf/ftgrays_build.c`, `bench/ttf/spancheck.c`
- Create (copied): `bench/ttf/ftgrays.c`, `bench/ttf/ftgrays.h`, `bench/ttf/ftimage.h` (FreeType 2.13.3
  from the SDK, with the pool size patched)

**Interfaces:**
- Produces: `int ittf_init(ITtf*, const uint8_t*, uint32_t)`, `uint32_t ittf_glyph(const ITtf*, uint32_t cp)`,
  and `int ittf_render_2bit(const ITtf*, uint32_t glyph, int32_t ppem26_6, ITtfOutline*, uint8_t* out, uint32_t cap, ITtfBitmap*)`.
  `ittf_render_2bit` returns 1 when it rendered, 0 for an empty glyph, and <0 on error.

- [x] **Step 1: Create the branch and copy FreeType's rasterizer**

```bash
cd /c/_development/witchhunt-reader
git switch master && git switch -c bench/ttf-step0
mkdir -p bench/ttf
FT=freeink-sdk/libs/font/FreeInkFont/third_party/freetype
cp $FT/src/smooth/ftgrays.c $FT/src/smooth/ftgrays.h $FT/include/freetype/ftimage.h bench/ttf/
sed -i -e 's/^#define FT_RENDER_POOL_SIZE  16384L/#define FT_RENDER_POOL_SIZE  2048L/' \
       -e 's/^#define FT_RENDER_POOL_SIZE  4096L/#define FT_RENDER_POOL_SIZE  2048L/' bench/ttf/ftgrays.c
grep -n "^#define FT_RENDER_POOL_SIZE" bench/ttf/ftgrays.c
```

Expected: two lines, both `2048L` (lines 107 and 479).

- [x] **Step 2: Create the standalone shims**

`bench/ttf/ftstandalone.h`:

```c
#pragma once
/* THROWAWAY (TTF step 0): what ftgrays.c defines for its own standalone build, for the files
 * that include ftimage.h / ftgrays.h directly. */
#ifndef STANDALONE_
#define STANDALONE_
#endif
#ifndef ft_grays_raster
#define ft_grays_raster ittf_grays_raster
#endif
#ifndef FT_BEGIN_HEADER
#define FT_BEGIN_HEADER
#define FT_END_HEADER
#endif
#ifndef FT_STATIC_BYTE_CAST
#define FT_STATIC_BYTE_CAST(type, var) (type)(unsigned char)(var)
#endif
```

`bench/ttf/ftgrays_build.c`:

```c
/* THROWAWAY (TTF step 0): builds FreeType's ftgrays.c standalone. */
#define STANDALONE_
#define FALL_THROUGH ((void)0)
#define ft_grays_raster ittf_grays_raster
#include "ftgrays.c"
```

- [x] **Step 3: Create `bench/ttf/ittf.h` and `bench/ttf/ittf.c` from Appendix A, verbatim**

- [x] **Step 4: Write the host check `bench/ttf/spancheck.c`**

It checks that the span path (what the device runs) gives exactly the bitmap path's pixels, packed
with the PC converter's thresholds:

```c
/* THROWAWAY (TTF step 0): span-mode 2-bit output must equal bitmap-mode output packed afterwards. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ittf.h"

static uint8_t q2(uint8_t v) {
  int n = v >> 4;
  return (uint8_t)(n >= 12 ? 3 : n >= 8 ? 2 : n >= 4 ? 1 : 0);
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  FILE* fp = fopen(argv[1], "rb");
  if (!fp) return 2;
  fseek(fp, 0, SEEK_END);
  long len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  uint8_t* d = (uint8_t*)malloc((size_t)len);
  fread(d, 1, (size_t)len, fp);
  fclose(fp);
  const int px = atoi(argv[2]);
  static ITtf f;
  static ITtfOutline o;
  static uint8_t bm8[512 * 512], packed[512 * 512 / 4], span[512 * 512 / 4];
  if (ittf_init(&f, d, (uint32_t)len)) return 2;
  long glyphs = 0, mismatched = 0;
  for (uint32_t cp = 0x20; cp <= 0xFFFF; ++cp) {
    uint32_t g = ittf_glyph(&f, cp);
    if (!g) continue;
    ITtfBitmap a, b;
    if (ittf_render(&f, g, px << 6, &o, bm8, sizeof bm8, &a)) continue;
    int r = ittf_render_2bit(&f, g, px << 6, &o, span, sizeof span, &b);
    if (r < 0) { mismatched++; continue; }
    glyphs++;
    if (a.width != b.width || a.height != b.height || a.left != b.left || a.top != b.top) { mismatched++; continue; }
    const int n = a.width * a.height;
    memset(packed, 0, (size_t)(n + 3) / 4);
    for (int p = 0; p < n; ++p) packed[p >> 2] |= (uint8_t)(q2(a.pixels[p]) << (6 - 2 * (p & 3)));
    if (memcmp(packed, span, (size_t)(n + 3) / 4) != 0) mismatched++;
  }
  printf("%s @%d: %ld glyphs compared, %ld mismatched\n", argv[1], px, glyphs, mismatched);
  return mismatched ? 1 : 0;
}
```

- [x] **Step 5: Run the host check**

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
gcc -O2 -Ibench/ttf bench/ttf/ftgrays_build.c bench/ttf/ittf.c bench/ttf/spancheck.c -o /c/tmp/spancheck.exe
S=lib/EpdFont/builtinFonts/source
for f in $S/Bookerly/Bookerly-Regular.ttf $S/NotoSans/NotoSans-Regular.ttf $S/Inter/Inter-Regular.ttf; do
  for px in 29 54; do /c/tmp/spancheck.exe $f $px; done
done
```

Expected: six lines, each ending in `0 mismatched`. The glyph counts are 1677 for Bookerly, 3004 for
Noto Sans and 2817 for Inter. Any mismatch means the copy or the pool patch went wrong; fix that
before going on.

- [x] **Step 6: Commit (local branch only)**

```bash
git add bench/ttf
git commit -m "bench(ttf step 0): integer TrueType loader + standalone ftgrays, host span check"
```

---

### Task 2: The device sketch, its env and the embedded fonts

**Files:**
- Create: `bench/ttf/bin2h.py`, `bench/ttf/make_cjk_subset.py`, `bench/ttf/gen/.gitignore`
- Create: `bench/ttf_main.cpp`
- Modify: `platformio.ini` (add `[env:bench_ttf]` after `[env:bench_font]`)

**Interfaces:**
- Consumes: `ittf_init`, `ittf_glyph`, `ittf_render_2bit` (Task 1);
  `freeink::font::gpos::PairKernAdjustment(const uint8_t* gpos, size_t gposSize, uint32_t leftGlyph, uint32_t rightGlyph)`
  (FreeInkFont `Gpos.h`, which returns FUnits).
- Produces: the serial report that Tasks 4 and 5 read. Its line prefixes are `render`, `kerning`,
  `erase`, `program`, `index probe`, `free MMU pages`, `SD 512 B read`, and `loop-task stack`.

- [x] **Step 1: USER ACTION. Provide a CJK TrueType font**

Ask the user for a CJK font with **TrueType outlines** (a `glyf` table) and its local path, for example
LXGW WenKai Regular. Also ask them to copy the same file to the SD card as `/bench/cjk.ttf`. The card
must stay in the X3.

- [x] **Step 2: Create the generators and the embedded fonts**

`bench/ttf/bin2h.py`:

```python
"""THROWAWAY (TTF step 0): embed a font file in the bench image as a C array."""
import pathlib
import sys

src, dst, name = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
data = src.read_bytes()
lines = [f"// Generated from {src.name} by bench/ttf/bin2h.py. Do not commit.",
         "#pragma once", "#include <cstdint>",
         f"alignas(4) static const uint8_t {name}[] = {{"]
for i in range(0, len(data), 24):
    lines.append("  " + ",".join(str(b) for b in data[i:i + 24]) + ",")
lines.append("};")
lines.append(f"static constexpr uint32_t {name}Len = {len(data)};")
dst.parent.mkdir(parents=True, exist_ok=True)
dst.write_text("\n".join(lines) + "\n", encoding="ascii")
print(f"{dst}: {len(data)} bytes")
```

`bench/ttf/make_cjk_subset.py`. The sample is every 41st codepoint of U+4E00..U+9FA5, which spreads
510 glyphs over the whole block. The sketch uses the same stride, so per-glyph cost is measured on
real outlines without embedding a 10+ MB font:

```python
"""THROWAWAY (TTF step 0): cut the bench's CJK sample out of a full CJK TrueType font."""
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

src, dst = sys.argv[1], sys.argv[2]
han = list(range(0x4E00, 0x9FA6, 41))
font = TTFont(src)
if "glyf" not in font:
    sys.exit(f"{src} has no 'glyf' table (CFF outlines?): the TTF design reads TrueType outlines only")
opts = subset.Options()
opts.hinting = False
opts.layout_features = []
sub = subset.Subsetter(opts)
sub.populate(unicodes=list(range(0x20, 0x7F)) + han)
sub.subset(font)
font.save(dst)
cmap = font.getBestCmap()
print(f"{dst}: {sum(1 for c in han if c in cmap)} of {len(han)} sampled Han codepoints present")
```

`bench/ttf/gen/.gitignore`:

```
*
!.gitignore
```

Generate the fonts (replace `<CJK.ttf>` with the path from Step 1):

```bash
S=lib/EpdFont/builtinFonts/source
python bench/ttf/bin2h.py $S/Bookerly/Bookerly-Regular.ttf bench/ttf/gen/bookerly.h kBookerly
python bench/ttf/bin2h.py $S/NotoSans/NotoSans-Regular.ttf bench/ttf/gen/notosans.h kNotoSans
python bench/ttf/make_cjk_subset.py "<CJK.ttf>" /c/tmp/cjk-subset.ttf
python bench/ttf/bin2h.py /c/tmp/cjk-subset.ttf bench/ttf/gen/cjk_subset.h kCjkSubset
```

Expected: three `bytes` lines (Bookerly 450048, Noto Sans 629024, the subset under 1 MB), and a subset
line reporting at least 500 of 510 sampled Han codepoints present.

- [x] **Step 3: Create `bench/ttf_main.cpp`**

```cpp
// THROWAWAY: TTF step 0 benchmark. Never merged. Spec: docs/superpowers/specs/2026-10-13-ttf-fonts-design.md
// (section 9.1). Measures what the TTF design's gates depend on, prints integers only, then idles.
//
// Build & flash: pio run -e bench_ttf -t upload
// It ERASES the first 1 MB of the spiffs partition (the .cpfont flash cache; the reader re-caches it).

#include <Arduino.h>
#include <BoardConfig.h>
#include <Gpos.h>
#include <HalGPIO.h>
#include <HalI2cBus.h>
#include <HalPowerManager.h>
#include <HalSpiBus.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <spi_flash_mmap.h>

#include <algorithm>
#include <cstring>

#include "ttf/gen/bookerly.h"
#include "ttf/gen/cjk_subset.h"
#include "ttf/gen/notosans.h"
#include "ttf/ittf.h"

// Every timed loop folds its work into this, and it is printed once at the end, so -O2 cannot
// delete a loop whose result is otherwise unused.
static uint32_t sink = 0;
static int64_t nowUs() { return esp_timer_get_time(); }
static uint32_t lcg(uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  return s;
}

// ---- rendering ------------------------------------------------------------------------------

static ITtf g_font;
static ITtfOutline g_outline;  // ~17 KB in .bss; production sizes this from maxp (spec 5.3)
static uint8_t g_out[4096];

static uint32_t g_latin[189];
static uint32_t g_latinCount = 0;
static uint32_t g_cjk[520];
static uint32_t g_cjkCount = 0;

static void buildCodepointSets() {
  for (uint32_t c = 0x21; c <= 0x7E; ++c) g_latin[g_latinCount++] = c;
  for (uint32_t c = 0xA1; c <= 0xFF; ++c) g_latin[g_latinCount++] = c;
  for (uint32_t c = 0x4E00; c <= 0x9FA5; c += 41) g_cjk[g_cjkCount++] = c;  // same stride as make_cjk_subset.py
}

struct RenderResult {
  uint32_t coldNs = 0;  // first pass: flash cache cold for this font's outlines
  uint32_t warmNs = 0;  // mean of the next 5 passes
  uint32_t rendered = 0;
  uint32_t maxBytes = 0;
};

static RenderResult timeRender(const uint8_t* data, uint32_t len, int px, const uint32_t* cps, uint32_t n) {
  RenderResult r;
  if (ittf_init(&g_font, data, len) != 0) {
    Serial.println("    ittf_init FAILED");
    return r;
  }
  constexpr int kWarmPasses = 5;
  uint64_t warmTotalNs = 0;
  for (int pass = 0; pass <= kWarmPasses; ++pass) {
    uint32_t count = 0;
    const int64_t t0 = nowUs();
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t g = ittf_glyph(&g_font, cps[i]);
      if (!g) continue;
      ITtfBitmap bm;
      const int rc = ittf_render_2bit(&g_font, g, px << 6, &g_outline, g_out, sizeof g_out, &bm);
      if (rc < 0) continue;
      sink += static_cast<uint32_t>(bm.width) + (rc > 0 ? g_out[0] : 0u);
      r.maxBytes = std::max(r.maxBytes, static_cast<uint32_t>(bm.width * bm.height + 3) / 4);
      ++count;
    }
    const uint64_t ns = static_cast<uint64_t>(nowUs() - t0) * 1000u / (count ? count : 1);
    if (pass == 0) {
      r.coldNs = static_cast<uint32_t>(ns);
    } else {
      warmTotalNs += ns;
    }
    r.rendered = count;
  }
  r.warmNs = static_cast<uint32_t>(warmTotalNs / kWarmPasses);
  return r;
}

static void benchRender() {
  struct Case {
    const char* name;
    const uint8_t* data;
    uint32_t len;
    const uint32_t* cps;
    uint32_t n;
  };
  const Case cases[] = {
      {"Bookerly", kBookerly, kBookerlyLen, g_latin, g_latinCount},
      {"NotoSans", kNotoSans, kNotoSansLen, g_latin, g_latinCount},
      {"CJK", kCjkSubset, kCjkSubsetLen, g_cjk, g_cjkCount},
  };
  const unsigned stackBefore = static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr));
  for (const Case& c : cases) {
    for (const int px : {29, 54}) {  // 14 pt and 26 pt at 150 DPI
      const RenderResult r = timeRender(c.data, c.len, px, c.cps, c.n);
      Serial.printf("  render %-8s %2d px/em: cold %6u ns/glyph, warm %6u ns/glyph (%u glyphs, max %u B)\n", c.name,
                    px, (unsigned)r.coldNs, (unsigned)r.warmNs, (unsigned)r.rendered, (unsigned)r.maxBytes);
    }
  }
  const unsigned stackAfter = static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr));
  Serial.printf("  loop-task stack high-water: %u B free before rendering, %u B after\n", stackBefore, stackAfter);
}

// ---- kerning --------------------------------------------------------------------------------

static uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

static uint32_t findTable(const uint8_t* d, uint32_t len, const char* tag, uint32_t* tableLen) {
  const uint32_t n = (uint32_t(d[4]) << 8) | d[5];
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t r = 12 + 16 * i;
    if (r + 16 > len) break;
    if (memcmp(d + r, tag, 4) == 0) {
      *tableLen = be32(d + r + 12);
      return be32(d + r + 8);
    }
  }
  return 0;
}

static void benchKerning(const char* name, const uint8_t* d, uint32_t len) {
  uint32_t gposLen = 0;
  const uint32_t gpos = findTable(d, len, "GPOS", &gposLen);
  if (!gpos) {
    Serial.printf("  kerning %-8s: no GPOS table\n", name);
    return;
  }
  if (ittf_init(&g_font, d, len) != 0) return;
  static const char kChars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,;:'-()";
  constexpr size_t kN = sizeof kChars - 1;
  uint32_t gids[kN];
  for (size_t i = 0; i < kN; ++i) gids[i] = ittf_glyph(&g_font, static_cast<unsigned char>(kChars[i]));
  uint32_t nonzero = 0;
  const int64_t t0 = nowUs();
  for (size_t a = 0; a < kN; ++a) {
    for (size_t b = 0; b < kN; ++b) {
      const int32_t v = freeink::font::gpos::PairKernAdjustment(d + gpos, gposLen, gids[a], gids[b]);
      sink += static_cast<uint32_t>(v);
      nonzero += v != 0;
    }
  }
  const int64_t us = nowUs() - t0;
  Serial.printf("  kerning %-8s: %lld ns/pair (GPOS %u B), %u of %u pairs non-zero\n", name,
                (long long)(us * 1000 / (int64_t)(kN * kN)), (unsigned)gposLen, (unsigned)nonzero,
                (unsigned)(kN * kN));
}

// ---- flash: erase, program, index probe ------------------------------------------------------

constexpr uint32_t kIndexOffset = 512 * 1024;  // inside the erased first 1 MB
constexpr uint32_t kIndexSlots = 4096;         // 32 KB of 8-byte slots
constexpr uint32_t kKeys = 2000;               // ~49% load

static uint32_t hashSlot(uint32_t key) { return (key * 2654435761u) >> (32 - 12); }

static uint32_t makeKey(uint32_t& s) {  // spec 4.3: cp | style << 21 | sizeIndex << 23
  const uint32_t cp = 0x20 + lcg(s) % (0x9FFF - 0x20);
  const uint32_t style = (lcg(s) >> 8) & 3;
  const uint32_t size = (lcg(s) >> 8) & 3;
  return cp | (style << 21) | (size << 23);
}

static void benchFlash() {
  const esp_partition_t* p =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (!p || p->size < (1u << 20) + 0x10000) {
    Serial.println("  spiffs partition missing or too small");
    return;
  }
  Serial.printf("  partition at 0x%06x, %u B; erasing its first 1 MB\n", (unsigned)p->address, (unsigned)p->size);

  int64_t t0 = nowUs();
  esp_partition_erase_range(p, 0, 0x10000);
  Serial.printf("  erase 64 KB: %lld us\n", (long long)(nowUs() - t0));
  t0 = nowUs();
  esp_partition_erase_range(p, 0, 1u << 20);
  Serial.printf("  erase 1 MB: %lld us\n", (long long)(nowUs() - t0));

  static uint8_t rec[252];
  for (size_t i = 0; i < sizeof rec; ++i) rec[i] = static_cast<uint8_t>(i * 37 + 1);
  uint32_t off = 0;
  for (const uint32_t size : {100u, 252u}) {
    t0 = nowUs();
    for (int i = 0; i < 200; ++i) {
      esp_partition_write(p, off, rec, size);
      off += size;
    }
    Serial.printf("  program a %u-byte record: %lld us\n", (unsigned)size, (long long)((nowUs() - t0) / 200));
  }

  static uint8_t occupied[kIndexSlots / 8];
  memset(occupied, 0, sizeof occupied);
  static uint32_t keys[kKeys];
  uint32_t seed = 12345;
  t0 = nowUs();
  for (uint32_t k = 0; k < kKeys; ++k) {
    const uint32_t key = makeKey(seed);
    keys[k] = key;
    uint32_t slot = hashSlot(key);
    while (occupied[slot >> 3] & (1u << (slot & 7))) slot = (slot + 1) & (kIndexSlots - 1);
    occupied[slot >> 3] |= static_cast<uint8_t>(1u << (slot & 7));
    const uint32_t entry[2] = {key, k * 128u};
    esp_partition_write(p, kIndexOffset + slot * 8, entry, sizeof entry);
  }
  Serial.printf("  program an 8-byte index slot: %lld us\n", (long long)((nowUs() - t0) / kKeys));

  const void* mapped = nullptr;
  esp_partition_mmap_handle_t handle;
  if (esp_partition_mmap(p, kIndexOffset, 0x10000, ESP_PARTITION_MMAP_DATA, &mapped, &handle) != ESP_OK) {
    Serial.println("  mmap of the index failed");
    return;
  }
  const uint32_t* slots = static_cast<const uint32_t*>(mapped);
  uint32_t missSeed = 999;
  uint32_t hits = 0;
  constexpr int kProbes = 10000;
  t0 = nowUs();
  for (int i = 0; i < kProbes; ++i) {
    // Odd i: a stored key. Even i: bit 27 is never set by makeKey(), so a guaranteed miss.
    const uint32_t key = (i & 1) ? keys[(i * 7) % kKeys] : (makeKey(missSeed) | (1u << 27));
    uint32_t slot = hashSlot(key);
    for (;;) {
      const uint32_t k = slots[slot * 2];
      if (k == key) {
        ++hits;
        sink += slots[slot * 2 + 1];
        break;
      }
      if (k == 0xFFFFFFFFu) break;
      slot = (slot + 1) & (kIndexSlots - 1);
    }
  }
  const int64_t probeUs = nowUs() - t0;
  esp_partition_munmap(handle);
  Serial.printf("  index probe (half hits): %lld ns per lookup, %u hits of %d\n",
                (long long)(probeUs * 1000 / kProbes), (unsigned)hits, kProbes);
  Serial.printf("  free MMU pages (this bench image, nothing mapped): %u\n",
                (unsigned)spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA));
}

// ---- SD -------------------------------------------------------------------------------------

static bool g_sdReady = false;

static void bringUpSd() {
  // The part of src/main.cpp setup() that SD access depends on, in the same order.
  HalSystem::begin();
  HalSpiBus::begin();
  HalI2cBus::begin();
  gpio.begin();
  HalI2cBus::ensureBusStarted();
  powerManager.begin();
  g_sdReady = Storage.begin();
  Serial.printf("  SD card: %s\n", g_sdReady ? "mounted" : "NOT mounted");
}

static void benchSd() {
  if (!g_sdReady) {
    Serial.println("  SD 512 B read: SKIPPED (no card)");
    return;
  }
  HalFile f = Storage.open("/bench/cjk.ttf");
  if (!f) {
    Serial.println("  SD 512 B read: SKIPPED (/bench/cjk.ttf is not on the card)");
    return;
  }
  const uint32_t size = static_cast<uint32_t>(f.fileSize());
  constexpr int kReads = 500;
  static uint32_t offsets[kReads];
  uint32_t seed = 4242;
  for (int i = 0; i < kReads; ++i) offsets[i] = (lcg(seed) % (size - 512)) & ~511u;
  static uint8_t buf[512];
  for (int sorted = 0; sorted < 2; ++sorted) {
    if (sorted) std::sort(offsets, offsets + kReads);
    const int64_t t0 = nowUs();
    for (int i = 0; i < kReads; ++i) {
      f.seekSet(offsets[i]);
      sink += static_cast<uint32_t>(f.read(buf, sizeof buf)) + buf[7];
    }
    Serial.printf("  SD 512 B read, %s offsets: %lld us each (file %u B)\n", sorted ? "sorted" : "random",
                  (long long)((nowUs() - t0) / kReads), (unsigned)size);
  }
  f.close();
}

// ---- main ---------------------------------------------------------------------------------------

void setup() {
  BoardConfig::holdPowerRails();
  delay(250);
  Serial.begin(115200);
  delay(8000);  // time for capture.py to attach after `pio run -t upload` resets the chip
  Serial.println();
  Serial.println("=== TTF step 0 benchmark ===");
  buildCodepointSets();
  Serial.println("[render]");
  benchRender();
  Serial.println("[kerning]");
  benchKerning("Bookerly", kBookerly, kBookerlyLen);
  benchKerning("NotoSans", kNotoSans, kNotoSansLen);
  Serial.println("[flash]");
  benchFlash();
  Serial.println("[sd]");
  bringUpSd();
  benchSd();
  Serial.printf("(sink=%lu, printed so the optimiser keeps the work above)\n", (unsigned long)sink);
  Serial.println("=== done ===");
  Serial.println("Reflash the reader: pio run -e default -t upload");
}

void loop() {
  delay(30000);
  Serial.println("[idle] TTF benchmark finished; reflash the reader with: pio run -e default -t upload");
}
```

- [x] **Step 4: Add the env**

In `platformio.ini`, after the `[env:bench_font]` block:

```ini
; THROWAWAY (TTF step 0, never merged): docs/superpowers/plans/2026-10-13-ttf-step0-device-benchmark.md.
; Same shape as bench_font: full lib_deps for the HAL, efuse wrap from src, nothing else from src.
[env:bench_ttf]
extends = base, firmware_tuned
build_src_filter = -<*> +<../bench/ttf_main.cpp> +<../bench/ttf/ittf.c> +<../bench/ttf/ftgrays_build.c> +<../freeink-sdk/libs/font/FreeInkFont/src/Gpos.cpp> +<platform/skip_efuse_blk_check.c>
build_flags =
  ${c3.build_flags}
  -DENABLE_SERIAL_LOG
  -DLOG_LEVEL=1
  -DCROSSPOINT_VERSION='"bench"'
  -I$PROJECT_DIR/freeink-sdk/libs/font/FreeInkFont/include
```

- [x] **Step 5: Build it**

```bash
export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8
"$HOME/.platformio/penv/Scripts/pio.exe" run -e bench_ttf > /c/tmp/bench_ttf_build.log 2>&1; tail -5 /c/tmp/bench_ttf_build.log
```

Expected: `[SUCCESS]`. If `Gpos.h` is not found, the `-I$PROJECT_DIR/...` flag did not expand. Use the
absolute path `-IC:/_development/witchhunt-reader/freeink-sdk/libs/font/FreeInkFont/include` instead.
If the `.c` files do not see `STANDALONE_`, check that `ittf.c` includes `ittf.h`, and therefore
`ftstandalone.h`, before `ftgrays.h`. It does in Appendix A.

- [x] **Step 6: Commit (local branch only; `bench/ttf/gen/` is ignored)**

```bash
git add bench/ttf_main.cpp bench/ttf/bin2h.py bench/ttf/make_cjk_subset.py bench/ttf/gen/.gitignore platformio.ini
git commit -m "bench(ttf step 0): device sketch and env"
```

---

### Task 3: Mapping budget from the ELF (host only)

**Files:**
- Create: `bench/ttf/mmu_budget.py`

**Interfaces:**
- Produces: free MMU pages for the reader image, and for the bench image (which Task 4 checks against
  the device).

- [x] **Step 1: Create `bench/ttf/mmu_budget.py`**

```python
"""THROWAWAY (TTF step 0): MMU pages an app image maps on the ESP32-C3.

128 pages of 64 KB are shared by the app's code (IROM, 0x42xxxxxx) and its rodata (DROM,
0x3Cxxxxxx); every runtime mapping takes what is left. A segment occupies every page it touches,
so its count is ceil((vaddr % 64K + size) / 64K)."""
import subprocess
import sys

readelf, elf = sys.argv[1], sys.argv[2]
out = subprocess.run([readelf, "-lW", elf], capture_output=True, text=True, check=True).stdout
pages = 0
for line in out.splitlines():
    parts = line.split()
    if not parts or parts[0] != "LOAD":
        continue
    va, memsz = int(parts[2], 16), int(parts[5], 16)
    if 0x3C000000 <= va < 0x3C800000 or 0x42000000 <= va < 0x42800000:
        n = ((va & 0xFFFF) + memsz + 0xFFFF) // 0x10000
        pages += n
        print(f"  segment 0x{va:08x} size {memsz:>9} -> {n} pages")
print(f"{elf}: {pages} pages mapped by the image, {128 - pages} free for runtime mappings")
```

- [x] **Step 2: Build the reader image and run the budget on both images**

```bash
export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8
"$HOME/.platformio/penv/Scripts/pio.exe" run -e default > /c/tmp/default_build.log 2>&1; tail -3 /c/tmp/default_build.log
RE=/c/pio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-readelf.exe
python bench/ttf/mmu_budget.py $RE .pio/build/default/firmware.elf | tee /c/tmp/mmu_default.txt
python bench/ttf/mmu_budget.py $RE .pio/build/bench_ttf/firmware.elf | tee /c/tmp/mmu_bench.txt
```

Expected: two segment lines per image and a final line each. For the reader image, about 96 pages
mapped and about 31 free (docs/contributing/flash-font-partition.md), which still includes the language
pack's page. Keep both files for Task 5.

- [x] **Step 3: Commit (local branch only)**

```bash
git add bench/ttf/mmu_budget.py
git commit -m "bench(ttf step 0): MMU budget from the ELF"
```

---

### Task 4: Run on the X3 and restore the reader

**Files:** none; output goes to `/c/tmp/`.

- [x] **Step 1: USER ACTION. Confirm before flashing**

Tell the user that this flashes the benchmark over the reader and erases the flash font cache, and
that Step 4 restores the reader. Ask them to connect the X3 with the SD card from Task 2 inserted. Wait
for a yes.

- [x] **Step 2: Flash and capture**

Write the capture script with the Write tool as `/c/tmp/capture.py`. DTR and RTS are held low before
`open()`, because toggling them after `open()` resets the C3.

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
export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8
"$HOME/.platformio/penv/Scripts/pio.exe" run -e bench_ttf -t upload
/c/pio/penv/Scripts/python.exe /c/tmp/capture.py COM6 /c/tmp/bench_ttf_run1.txt
```

The sketch waits 8 s after boot, so starting the capture straight after the upload catches the run from
the top. If the capture file does not start with `=== TTF step 0 benchmark ===`, USER ACTION: press the
X3's reset button with the capture running.

Expected: the file ends with `=== done ===` and contains:

- 6 `render` lines (3 fonts × 2 sizes), each with a non-zero glyph count;
- 2 `kerning` lines;
- the `erase`, `program`, `index probe` and `free MMU pages` lines;
- the `SD card` line and 2 `SD 512 B read` lines (or `SKIPPED`);
- the `loop-task stack` line.

- [x] **Step 3: Check the run is usable**

```bash
grep -c "render " /c/tmp/bench_ttf_run1.txt
grep "SKIPPED\|FAILED\|missing" /c/tmp/bench_ttf_run1.txt
grep "free MMU pages" /c/tmp/bench_ttf_run1.txt; tail -1 /c/tmp/mmu_bench.txt
```

Expected:

- `6`;
- no `SKIPPED`, `FAILED` or `missing` lines. If the SD timing was skipped, have the user copy
  `/bench/cjk.ttf` to the card and repeat Step 2;
- the device's free-page figure and `mmu_bench.txt`'s within 1 page of each other. If they differ by
  more, the reader-image figure from Task 3 is not trusted; say so in Task 5.

Run the sketch a second time (reset the device with the capture running, saving to
`/c/tmp/bench_ttf_run2.txt`), and check that the warm render figures agree within 5%.

- [x] **Step 4: Restore the reader firmware**

```bash
"$HOME/.platformio/penv/Scripts/pio.exe" run -e default -t upload
```

USER ACTION: open a book in an SD-card font once. The reader re-caches the family into the erased
partition, as it does after any family change. Confirm that the book reads normally.

---

### Task 5: Evaluate the gates and record the results

**Files:**
- Create: `C:\Users\jens\.claude\projects\c---development-witchhunt-reader\memory\ttf-step0-results.md`
- Modify: `C:\Users\jens\.claude\projects\c---development-witchhunt-reader\memory\MEMORY.md` (one index
  line)

Per the docs layout convention, measurement journals do not go in `docs/`. (For the handover the
figures were later copied into the spec's Appendix A, as the evidence its decisions rest on.)

- [x] **Step 1: Compute the all-in figures from run 1**

From `/c/tmp/bench_ttf_run1.txt`:

- **Latin all-in per new glyph** = `render Bookerly 29` **cold** ns + `program a 100-byte record` +
  `program an 8-byte index slot`. Use Noto Sans as well and report the larger of the two.
- **CJK all-in per new glyph** = `render CJK 29` **cold** ns + `SD 512 B read, random` +
  `program a 252-byte record` + `program an 8-byte index slot`.
- **CJK first chapter** = 1,500 new glyphs × CJK all-in, as an estimate of a novel's first chapter.
  State that 1,500 is an assumption.
- **Mapping budget for the store** = the free pages in `/c/tmp/mmu_default.txt`.
- **Kerning:** the ns/pair from both `kerning` lines, and whether a pair cache is worth its 2 KB. It is
  worth it if ns/pair × about 2,000 pairs per page exceeds 5 ms.
- **Stack:** the drop between the two `loop-task stack` numbers, against the loop task's 8 KB. The
  render task, at 10 KB, is the roomier of the two that render glyphs, so the loop task is the one that
  matters.
- **Store hit:** the `index probe` ns, against today's built-in glyph lookup of about 1,083 ns on the X3
  (`glyph_lookup`, the measured figure in the `EpdGlyphRef` note in `lib/EpdFont/EpdFontData.h`). A
  probe within about 2× of it needs no further work.

- [x] **Step 2: Apply the gates (spec 9.1)**

- Latin or CJK all-in **> 2 ms** → REVISIT: metrics-only layout into Stage 2c. Otherwise PASS.
- CJK first chapter **> 10 s** → REVISIT: metrics-only layout into Stage 2c. Otherwise PASS.
- Free mapping pages **< 25** → REVISIT: shrink the store regions in spec 4.1. Otherwise PASS.

- [x] **Step 3: Record the results in memory**

Write `ttf-step0-results.md` with frontmatter (`name: ttf-step0-results`, a one-line `description`,
`metadata.type: project`). The body holds:

- the raw lines of run 1 under a heading, and the date;
- the computed figures and each gate's verdict;
- one line on run-to-run agreement;
- `**Why:**` these numbers set Stage 2's choices: the pair cache, metrics-only layout, the store
  geometry;
- `**How to apply:**` quote these numbers in the Stage 2 plans; re-measure if the engine changes;
- links `[[ttf-on-c3-feasibility]]` and `[[ttf-deferred-items]]`.

Add one line to `MEMORY.md`:
`- [TTF step 0 results](ttf-step0-results.md) — device costs + gate verdicts for the TTF design`.

- [x] **Step 4: Report to the user**

A short summary:

- one table of the measured costs and the three gate verdicts;
- what each verdict changes for Stage 2. A PASS changes nothing; each REVISIT names the spec section
  to amend;
- that the reader firmware is restored.

Leave the `bench/ttf-step0` branch local. It is deleted once Stage 2 no longer needs to re-measure.

---

## Appendix A: `bench/ttf/ittf.h` and `bench/ttf/ittf.c`

This is the integer loader as verified on 2026-10-13:

- matched FreeType unhinted on 12,868 glyphs (identical cmap lookups and advances; 5 glyph boxes
  differed);
- the span path matched the bitmap path on 22,494 renders;
- no soft-float on RV32.

The comments name the throwaway status.

`bench/ttf/ittf.h`:

```c
/* THROWAWAY PROTOTYPE (TTF spike): integer TrueType loader + ftgrays. */
#pragma once
#include <limits.h>
#include <stdint.h>

#include "ftstandalone.h"
#include "ftimage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ITTF_MAX_POINTS 1024
#define ITTF_MAX_CONTOURS 128

typedef struct {
  const uint8_t* d;
  uint32_t len;
  uint32_t loca, glyf, hmtx, cmapSub;
  uint16_t upem, numGlyphs, numHMetrics, cmapFmt;
  int16_t longLoca, ascender, descender;
} ITtf;

typedef struct {
  int32_t px[ITTF_MAX_POINTS], py[ITTF_MAX_POINTS];
  FT_Vector vec[ITTF_MAX_POINTS];
  unsigned char tags[ITTF_MAX_POINTS];
  short contours[ITTF_MAX_CONTOURS];
  uint16_t numPoints, numContours;
} ITtfOutline;

typedef struct {
  const uint8_t* pixels;
  int width, height, left, top;
  int32_t advance26_6;
} ITtfBitmap;

int ittf_init(ITtf* f, const uint8_t* d, uint32_t len);
uint32_t ittf_glyph(const ITtf* f, uint32_t cp);
int ittf_advance26_6(const ITtf* f, uint32_t g, int32_t ppem26_6);
int ittf_render(const ITtf* f, uint32_t g, int32_t ppem26_6, ITtfOutline* o, uint8_t* buf, uint32_t cap,
                ITtfBitmap* out);

/* Renders straight into the device's 2-bit stream through the span callback.
 * Returns 1 = rendered, 0 = empty glyph (bm->width == 0), <0 = error / over cap. */
int ittf_render_2bit(const ITtf* f, uint32_t g, int32_t ppem26_6, ITtfOutline* o, uint8_t* out2, uint32_t cap,
                     ITtfBitmap* bm);

#ifdef __cplusplus
}
#endif
```

`bench/ttf/ittf.c`:

```c
/* THROWAWAY PROTOTYPE (TTF spike, 2026-10-13): an integer-only TrueType glyph
 * loader feeding FreeType's standalone anti-aliasing rasterizer (ftgrays.c).
 * No floating point anywhere: font units are scaled to 26.6 with integer
 * multiply/divide, compound transforms use F2Dot14. Not production code. */
#include "ittf.h"

#include <string.h>

#include "ftgrays.h"

static uint16_t u16(const ITtf* f, uint32_t o) { return o + 2 <= f->len ? (uint16_t)((f->d[o] << 8) | f->d[o + 1]) : 0; }
static int16_t i16(const ITtf* f, uint32_t o) { return (int16_t)u16(f, o); }
static uint32_t u32(const ITtf* f, uint32_t o) {
  return o + 4 <= f->len ? ((uint32_t)f->d[o] << 24) | ((uint32_t)f->d[o + 1] << 16) | ((uint32_t)f->d[o + 2] << 8) | f->d[o + 3] : 0;
}

static uint32_t table(const ITtf* f, const char* tag) {
  uint16_t n = u16(f, 4);
  for (uint16_t i = 0; i < n; ++i) {
    uint32_t r = 12 + 16u * i;
    if (r + 16 <= f->len && memcmp(f->d + r, tag, 4) == 0) return u32(f, r + 8);
  }
  return 0;
}

int ittf_init(ITtf* f, const uint8_t* d, uint32_t len) {
  memset(f, 0, sizeof *f);
  f->d = d; f->len = len;
  uint32_t head = table(f, "head"), hhea = table(f, "hhea"), maxp = table(f, "maxp");
  f->loca = table(f, "loca"); f->glyf = table(f, "glyf"); f->hmtx = table(f, "hmtx");
  uint32_t cmap = table(f, "cmap");
  if (!head || !hhea || !maxp || !f->loca || !f->glyf || !f->hmtx || !cmap) return -1;
  f->upem = u16(f, head + 18);
  f->longLoca = i16(f, head + 50);
  f->numGlyphs = u16(f, maxp + 4);
  f->numHMetrics = u16(f, hhea + 34);
  f->ascender = i16(f, hhea + 4);
  f->descender = i16(f, hhea + 6);
  /* prefer format 12 (3,10), else format 4 (3,1) */
  uint16_t n = u16(f, cmap + 2);
  for (uint16_t i = 0; i < n; ++i) {
    uint32_t r = cmap + 4 + 8u * i;
    uint16_t pid = u16(f, r), eid = u16(f, r + 2);
    uint32_t sub = cmap + u32(f, r + 4);
    uint16_t fmt = u16(f, sub);
    if (pid == 3 && eid == 10 && fmt == 12) { f->cmapSub = sub; f->cmapFmt = 12; break; }
    if (pid == 3 && eid == 1 && fmt == 4 && !f->cmapSub) { f->cmapSub = sub; f->cmapFmt = 4; }
  }
  return f->cmapSub ? 0 : -1;
}

uint32_t ittf_glyph(const ITtf* f, uint32_t cp) {
  uint32_t s = f->cmapSub;
  if (f->cmapFmt == 12) {
    uint32_t n = u32(f, s + 12), lo = 0, hi = n;
    while (lo < hi) {
      uint32_t m = (lo + hi) / 2, g = s + 16 + 12 * m;
      if (cp < u32(f, g)) hi = m; else if (cp > u32(f, g + 4)) lo = m + 1;
      else return u32(f, g + 8) + (cp - u32(f, g));
    }
    return 0;
  }
  if (cp > 0xFFFF) return 0;
  uint16_t segX2 = u16(f, s + 6);
  uint32_t ends = s + 14, starts = ends + segX2 + 2, deltas = starts + segX2, ranges = deltas + segX2;
  uint32_t lo = 0, hi = segX2 / 2;
  while (lo < hi) {
    uint32_t m = (lo + hi) / 2;
    if (cp > u16(f, ends + 2 * m)) lo = m + 1; else hi = m;
  }
  if (lo >= segX2 / 2u || cp < u16(f, starts + 2 * lo)) return 0;
  uint16_t ro = u16(f, ranges + 2 * lo);
  if (!ro) return (uint16_t)(cp + u16(f, deltas + 2 * lo));
  uint16_t g = u16(f, ranges + 2 * lo + ro + 2 * (cp - u16(f, starts + 2 * lo)));
  return g ? (uint16_t)(g + u16(f, deltas + 2 * lo)) : 0;
}

static uint32_t glyphOffset(const ITtf* f, uint32_t g, uint32_t* end) {
  if (g >= f->numGlyphs) return 0;
  uint32_t a, b;
  if (f->longLoca) { a = u32(f, f->loca + 4 * g); b = u32(f, f->loca + 4 * g + 4); }
  else { a = 2u * u16(f, f->loca + 2 * g); b = 2u * u16(f, f->loca + 2 * g + 2); }
  *end = f->glyf + b;
  return a == b ? 0 : f->glyf + a;
}

/* font units -> 26.6, rounded, integer only */
static int32_t scale(int32_t v, int32_t ppem26_6, int32_t upem) {
  int64_t p = (int64_t)v * ppem26_6;
  return (int32_t)(p >= 0 ? (p + upem / 2) / upem : -((-p + upem / 2) / upem));
}

typedef struct { int32_t xx, xy, yx, yy, dx, dy; } Xf; /* 2.14 matrix, offsets in font units */

static int loadGlyph(const ITtf* f, uint32_t g, ITtfOutline* o, Xf xf, int depth) {
  uint32_t end, p = glyphOffset(f, g, &end);
  if (!p) return 0; /* empty glyph (space) */
  int16_t nc = i16(f, p);
  if (nc >= 0) {
    if (o->numContours + nc > ITTF_MAX_CONTOURS) return -1;
    uint32_t ec = p + 10;
    uint16_t nPts = (uint16_t)(u16(f, ec + 2 * (nc - 1)) + 1);
    if (nc == 0) return 0;
    if (o->numPoints + nPts > ITTF_MAX_POINTS) return -1;
    uint16_t base = o->numPoints;
    for (int16_t c = 0; c < nc; ++c) o->contours[o->numContours + c] = (int16_t)(base + u16(f, ec + 2 * c));
    uint32_t q = ec + 2u * nc;
    q += 2 + u16(f, q); /* skip instructions */
    /* flags */
    for (uint16_t i = 0; i < nPts;) {
      uint8_t fl = q < f->len ? f->d[q++] : 0;
      o->tags[base + i++] = fl;
      if (fl & 8) { uint8_t r = q < f->len ? f->d[q++] : 0; while (r-- && i < nPts) o->tags[base + i++] = fl; }
    }
    int32_t v = 0;
    for (uint16_t i = 0; i < nPts; ++i) { /* x */
      uint8_t fl = o->tags[base + i];
      if (fl & 2) { uint8_t dv = q < f->len ? f->d[q++] : 0; v += (fl & 16) ? dv : -dv; }
      else if (!(fl & 16)) { v += i16(f, q); q += 2; }
      o->px[base + i] = v;
    }
    v = 0;
    for (uint16_t i = 0; i < nPts; ++i) { /* y */
      uint8_t fl = o->tags[base + i];
      if (fl & 4) { uint8_t dv = q < f->len ? f->d[q++] : 0; v += (fl & 32) ? dv : -dv; }
      else if (!(fl & 32)) { v += i16(f, q); q += 2; }
      o->py[base + i] = v;
    }
    for (uint16_t i = 0; i < nPts; ++i) { /* transform in font units (2.14) then keep */
      int32_t x = o->px[base + i], y = o->py[base + i];
      o->px[base + i] = ((xf.xx * x + xf.yx * y + 8192) >> 14) + xf.dx;
      o->py[base + i] = ((xf.xy * x + xf.yy * y + 8192) >> 14) + xf.dy;
      o->tags[base + i] = (o->tags[base + i] & 1) ? FT_CURVE_TAG_ON : FT_CURVE_TAG_CONIC;
    }
    o->numPoints += nPts;
    o->numContours += nc;
    return 0;
  }
  if (depth > 4) return -1;
  uint32_t q = p + 10;
  uint16_t flags;
  do {
    flags = u16(f, q); uint16_t gi = u16(f, q + 2); q += 4;
    int32_t a1, a2;
    if (flags & 1) { a1 = i16(f, q); a2 = i16(f, q + 2); q += 4; }
    else { a1 = (int8_t)f->d[q]; a2 = (int8_t)f->d[q + 1]; q += 2; }
    Xf c = {16384, 0, 0, 16384, 0, 0};
    if (flags & 8) { c.xx = c.yy = i16(f, q); q += 2; }
    else if (flags & 0x40) { c.xx = i16(f, q); c.yy = i16(f, q + 2); q += 4; }
    else if (flags & 0x80) { c.xx = i16(f, q); c.xy = i16(f, q + 2); c.yx = i16(f, q + 4); c.yy = i16(f, q + 6); q += 8; }
    if (flags & 2) { c.dx = a1; c.dy = a2; } /* ARGS_ARE_XY_VALUES; point matching unsupported in spike */
    /* compose child into parent: parent(child(p)) */
    Xf m = {(xf.xx * c.xx + xf.yx * c.xy + 8192) >> 14, (xf.xy * c.xx + xf.yy * c.xy + 8192) >> 14,
            (xf.xx * c.yx + xf.yx * c.yy + 8192) >> 14, (xf.xy * c.yx + xf.yy * c.yy + 8192) >> 14,
            ((xf.xx * c.dx + xf.yx * c.dy + 8192) >> 14) + xf.dx, ((xf.xy * c.dx + xf.yy * c.dy + 8192) >> 14) + xf.dy};
    if (loadGlyph(f, gi, o, m, depth + 1) < 0) return -1;
  } while (flags & 0x20);
  return 0;
}

int ittf_advance26_6(const ITtf* f, uint32_t g, int32_t ppem26_6) {
  uint32_t i = g < f->numHMetrics ? g : (uint32_t)f->numHMetrics - 1;
  return scale(u16(f, f->hmtx + 4 * i), ppem26_6, f->upem);
}

static void* s_raster;

/* Span sink for FT_RASTER_FLAG_DIRECT: packs coverage straight into the device's 2-bit,
 * MSB-first, row-continuous stream with the PC converter's thresholds. y is bottom-up. */
typedef struct { uint8_t* out; int w, h; } SpanSink;
static void packSpans(int y, int count, const FT_Span* spans, void* user) {
  SpanSink* s = (SpanSink*)user;
  const int row = s->h - 1 - y;
  if (row < 0 || row >= s->h) return;
  for (int i = 0; i < count; ++i) {
    const int n = spans[i].coverage >> 4;
    const uint8_t q = (uint8_t)(n >= 12 ? 3 : n >= 8 ? 2 : n >= 4 ? 1 : 0);
    if (!q) continue;
    for (int x = spans[i].x; x < spans[i].x + spans[i].len; ++x) {
      if (x < 0 || x >= s->w) continue;
      const int p = row * s->w + x;
      s->out[p >> 2] |= (uint8_t)(q << (6 - 2 * (p & 3)));
    }
  }
}

static int loadScaled(const ITtf* f, uint32_t g, int32_t ppem26_6, ITtfOutline* o, ITtfBitmap* out,
                      int32_t* xMinOut, int32_t* yMinOut);

int ittf_render_2bit(const ITtf* f, uint32_t g, int32_t ppem26_6, ITtfOutline* o, uint8_t* out2, uint32_t cap,
                     ITtfBitmap* bm) {
  int32_t xMin, yMin;
  int r = loadScaled(f, g, ppem26_6, o, bm, &xMin, &yMin);
  if (r <= 0) return r; /* <0 error, 0 empty glyph (bm->width == 0) */
  const uint32_t bytes = (uint32_t)(bm->width * bm->height + 3) / 4;
  if (bytes > cap) return -1;
  memset(out2, 0, bytes);
  FT_Outline ol;
  ol.n_contours = (short)o->numContours; ol.n_points = (short)o->numPoints;
  ol.points = o->vec; ol.tags = (char*)o->tags; ol.contours = o->contours; ol.flags = FT_OUTLINE_NONE;
  SpanSink sink = {out2, bm->width, bm->height};
  if (!s_raster) ft_grays_raster.raster_new(NULL, (FT_Raster*)&s_raster);
  FT_Raster_Params params;
  memset(&params, 0, sizeof params);
  params.source = &ol;
  params.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
  params.gray_spans = packSpans;
  params.user = &sink;
  params.clip_box.xMin = 0; params.clip_box.yMin = 0;
  params.clip_box.xMax = bm->width; params.clip_box.yMax = bm->height;
  if (ft_grays_raster.raster_render((FT_Raster)s_raster, &params)) return -1;
  bm->pixels = out2;
  return 1;
}

/* Loads, scales and translates the outline so the cbox starts at (0,0). Returns 1 with a
 * non-empty box, 0 for an empty glyph, <0 on error. */
static int loadScaled(const ITtf* f, uint32_t g, int32_t ppem26_6, ITtfOutline* o, ITtfBitmap* out,
                      int32_t* xMinOut, int32_t* yMinOut) {
  memset(out, 0, sizeof *out);
  o->numPoints = 0; o->numContours = 0;
  Xf id = {16384, 0, 0, 16384, 0, 0};
  if (loadGlyph(f, g, o, id, 0) < 0) return -1;
  out->advance26_6 = ittf_advance26_6(f, g, ppem26_6);
  if (!o->numPoints) return 0;
  int32_t xMin = INT32_MAX, yMin = INT32_MAX, xMax = INT32_MIN, yMax = INT32_MIN;
  for (uint16_t i = 0; i < o->numPoints; ++i) {
    int32_t x = scale(o->px[i], ppem26_6, f->upem), y = scale(o->py[i], ppem26_6, f->upem);
    o->vec[i].x = x; o->vec[i].y = y;
    if (x < xMin) xMin = x; if (x > xMax) xMax = x;
    if (y < yMin) yMin = y; if (y > yMax) yMax = y;
  }
  xMin &= -64; yMin &= -64; xMax = (xMax + 63) & -64; yMax = (yMax + 63) & -64;
  int w = (xMax - xMin) >> 6, h = (yMax - yMin) >> 6;
  if (w <= 0 || h <= 0) return 0;
  for (uint16_t i = 0; i < o->numPoints; ++i) { o->vec[i].x -= xMin; o->vec[i].y -= yMin; }
  out->width = w; out->height = h; out->left = xMin >> 6; out->top = yMax >> 6;
  *xMinOut = xMin; *yMinOut = yMin;
  return 1;
}

int ittf_render(const ITtf* f, uint32_t g, int32_t ppem26_6, ITtfOutline* o, uint8_t* buf, uint32_t cap,
                ITtfBitmap* out) {
  memset(out, 0, sizeof *out);
  o->numPoints = 0; o->numContours = 0;
  Xf id = {16384, 0, 0, 16384, 0, 0};
  if (loadGlyph(f, g, o, id, 0) < 0) return -1;
  out->advance26_6 = ittf_advance26_6(f, g, ppem26_6);
  if (!o->numPoints) return 0;
  int32_t xMin = INT32_MAX, yMin = INT32_MAX, xMax = INT32_MIN, yMax = INT32_MIN;
  for (uint16_t i = 0; i < o->numPoints; ++i) {
    int32_t x = scale(o->px[i], ppem26_6, f->upem), y = scale(o->py[i], ppem26_6, f->upem);
    o->vec[i].x = x; o->vec[i].y = y;
    if (x < xMin) xMin = x; if (x > xMax) xMax = x;
    if (y < yMin) yMin = y; if (y > yMax) yMax = y;
  }
  xMin &= -64; yMin &= -64; xMax = (xMax + 63) & -64; yMax = (yMax + 63) & -64;
  int w = (xMax - xMin) >> 6, h = (yMax - yMin) >> 6;
  if (w <= 0 || h <= 0 || (uint32_t)(w * h) > cap) return -1;
  for (uint16_t i = 0; i < o->numPoints; ++i) { o->vec[i].x -= xMin; o->vec[i].y -= yMin; }
  FT_Outline ol;
  ol.n_contours = (short)o->numContours; ol.n_points = (short)o->numPoints;
  ol.points = o->vec; ol.tags = (char*)o->tags; ol.contours = o->contours; ol.flags = FT_OUTLINE_NONE;
  FT_Bitmap bm;
  memset(&bm, 0, sizeof bm);
  bm.rows = (unsigned)h; bm.width = (unsigned)w; bm.pitch = w; bm.buffer = buf;
  bm.num_grays = 256; bm.pixel_mode = FT_PIXEL_MODE_GRAY;
  memset(buf, 0, (size_t)w * h);
  if (!s_raster) ft_grays_raster.raster_new(NULL, (FT_Raster*)&s_raster);
  FT_Raster_Params params;
  memset(&params, 0, sizeof params);
  params.target = &bm; params.source = &ol; params.flags = FT_RASTER_FLAG_AA;
  if (ft_grays_raster.raster_render((FT_Raster)s_raster, &params)) return -1;
  out->pixels = buf; out->width = w; out->height = h;
  out->left = xMin >> 6; out->top = yMax >> 6;
  return 0;
}
```
