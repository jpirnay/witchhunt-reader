# TTF step 0 bench (throwaway)

Measurement code for the on-device TTF design. It is not for merging; it exists so the numbers can
be re-measured if the engine changes.

- Design: `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md` (branch `docs/ttf-fonts-design`).
- Results: that spec's appendix "Step 0 measurements".
- Plan that built it: `docs/superpowers/plans/2026-10-13-ttf-step0-device-benchmark.md`.

## What is here

| File | What it is |
| --- | --- |
| `../ttf_main.cpp` | The device sketch (env `bench_ttf`). One `setup()` that prints every measurement over serial and ends with `=== done ===`. |
| `ittf.c`, `ittf.h` | Integer-only TrueType loader: `cmap` 4/12, `loca`/`glyf`, simple glyphs and xy-offset compounds, `hmtx`. Emits an `FT_Outline` in 26.6 and renders through `ftgrays` straight to 2-bit via the span callback. |
| `ftgrays.c`, `ftgrays.h`, `ftimage.h`, `ftstandalone.h` | FreeType 2.13.3's smooth rasterizer built standalone (FTL licence: credit FreeType if any of it ships). |
| `ftgrays_build.c`, `ftgrays_build_4k.c`, `ftgrays_build_8k.c` | The same rasterizer with a 2, 4 and 8 KB cell pool (`ITTF_POOL_SIZE`), for the pool-size runs. |
| `fastkern.h` | GPOS pair kerning with the `kern` PairPos subtables resolved once per font; binary search in format-2 tables. |
| `kernopts.h` | The other kerning options measured: a RAM pair cache (`CachedKern`) and class tables compiled at install (`KernCompiler`/`CompiledKern`). The lazy flash-store option lives in `ttf_main.cpp`. |
| `kerncheck.cpp` | Host check: `kernopts.h` gives exactly what `fastkern.h` gives for every pair of a font's kerning repertoire. |
| `spancheck.c` | Host check: span-mode 2-bit output equals bitmap-mode output packed afterwards. |
| `mmu_budget.py` | Free MMU pages of an app ELF (union of IROM/DROM page indices, not their sum). |
| `bin2h.py`, `make_cjk_subset.py`, `make_cjk_extents.py`, `make_chapter.py` | Generate the embedded data in `gen/`. |
| `host/` | The engine-selection tools; see "Host comparison tools" below. |

## Generate `gen/` (git-ignored, never commit)

From the repo root, with the PlatformIO penv Python (it has fontTools):

```bash
S=lib/EpdFont/builtinFonts/source
python bench/ttf/bin2h.py $S/Bookerly/Bookerly-Regular.ttf bench/ttf/gen/bookerly.h kBookerly
python bench/ttf/bin2h.py $S/NotoSans/NotoSans-Regular.ttf bench/ttf/gen/notosans.h kNotoSans
python bench/ttf/make_chapter.py test/fixtures/moby-dick.epub OEBPS/3308270851576161417_2701-h-3.htm.html 30000 bench/ttf/gen/chapter.h
# CJK: the measurements used Microsoft YaHei (Windows system font, member 0 of the collection).
# Local benchmark use only: do not commit it or anything generated from it.
python bench/ttf/make_cjk_subset.py C:/Windows/Fonts/msyh.ttc /c/tmp/cjk-subset.ttf 0
python bench/ttf/bin2h.py /c/tmp/cjk-subset.ttf bench/ttf/gen/cjk_subset.h kCjkSubset
python bench/ttf/make_cjk_extents.py C:/Windows/Fonts/msyh.ttc 0 bench/ttf/gen/cjk_extents.h
```

`make_cjk_subset.py` samples every 41st codepoint of U+4E00..U+9FA5; `ttf_main.cpp` uses the same
stride. `cjk_extents.h` records where each sampled glyph's outline lives in the FULL font file, so the
SD runs read real glyph records.

## Build, run, restore

```bash
export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8
"$HOME/.platformio/penv/Scripts/pio.exe" run -e bench_ttf -t upload
```

- **SD part:** copy the same full CJK font to the card as `/bench/cjk.ttf` first (USB file transfer;
  with `tools/serial_transfer.py` from Git Bash, set `MSYS_NO_PATHCONV=1` and pass Windows-style local
  paths). Without it the SD lines print `SKIPPED`. Delete it from the card afterwards.
- **Capture:** open the port with DTR and RTS held low, or opening it resets the C3. The capture script
  is in the Stage 1 plan (`docs/superpowers/plans/2026-10-13-ttf-stage1-glyph-provider.md`, Task 5,
  Step 3); it stops at `=== done ===`. Start it, then press reset.
- **Restore the reader:** `pio run -e default -t upload`.
- **Mapping budget from an ELF:**
  `python bench/ttf/mmu_budget.py <riscv32-esp-elf-readelf> .pio/build/default/firmware.elf`. The device
  sketch prints the same figure at runtime (`spi_flash_mmap_get_free_pages`), which is the one to trust.

## Host checks (Git Bash, UCRT64 gcc)

Compile the C parts with `gcc` and the C++ parts with `g++`; `g++` on the `.c` files fails. `ittf.c`
references all three rasterizer builds (2, 4 and 8 KB pools), so every link needs all four objects.

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
O=/c/tmp/ttfhost; mkdir -p $O
for c in ittf ftgrays_build ftgrays_build_4k ftgrays_build_8k; do gcc -O2 -Ibench/ttf -c bench/ttf/$c.c -o $O/$c.o; done
F=lib/EpdFont/builtinFonts/source/Bookerly/Bookerly-Regular.ttf

g++ -O2 -std=gnu++17 -Ibench/ttf bench/ttf/kerncheck.cpp $O/*.o -o $O/kerncheck.exe && $O/kerncheck.exe $F
gcc -O2 -Ibench/ttf bench/ttf/spancheck.c $O/*.o -o $O/spancheck.exe && $O/spancheck.exe $F 29
```

Expected (checked 2026-10-13): `kerncheck` reports 0 mismatches for both options over 1,245,456 pairs
(classes 197 x 165); `spancheck` reports 1,677 glyphs compared, 0 mismatched.

## Host comparison tools (`host/`)

These produced the engine-selection numbers in the spec (section 2 and the appendix). They were built
in a session scratch directory and are kept here as reference. `gposcheck.cpp` is rebuilt and checked
from this directory (below); `fullcmp.cpp` and `bench_grays.cpp` are not.

| File | Measures |
| --- | --- |
| `fullcmp.cpp` | Loader + `ftgrays` against FreeInkFont's `FtFont` (FreeType, unhinted) over every BMP codepoint a font maps: load failures, cmap/advance/box differences, share of 2-bit pixels that differ. Usage: `fullcmp FONT.ttf PX`. Source of the "2.1-2.8% of pixels" figure. |
| `bench_grays.cpp` | Host µs/glyph and pixel agreement: loader + `ftgrays` against `FtFont` and libschrift. |
| `gposcheck.cpp` | The pre-resolved GPOS reader (the code `fastkern.h` was extracted from) against FreeInkFont's `Gpos.cpp`, over every printable pair, with timings. |

Rebuilding them needs FreeInkFont (`freeink-sdk/libs/font/FreeInkFont`) compiled for the host:
`src/FtFont.cpp`, `src/Gpos.cpp` and, for `fullcmp`/`bench_grays`, the FreeType sources under
`src/freetype/` with the autofit module (the PC `.cpfont` converter uses `FT_LOAD_FORCE_AUTOHINT`).
`bench_grays.cpp` also needs libschrift's `schrift.c`. The exact host build line for FreeType was not
kept. `gposcheck.cpp` needs only `Gpos.cpp` (objects from the host checks above):

```bash
L=freeink-sdk/libs/font/FreeInkFont
g++ -O2 -std=gnu++17 -Ibench/ttf -I$L/include bench/ttf/host/gposcheck.cpp $L/src/Gpos.cpp $O/*.o -o $O/gposcheck.exe
$O/gposcheck.exe $F
```

Expected (2026-10-13): 12 subtables, 0 mismatches against `Gpos.cpp`, about 6.8x faster on the host.
