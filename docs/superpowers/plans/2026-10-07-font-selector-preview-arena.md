# Font Selector Preview Arena Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The font selector loads preview fonts into the lent secondary framebuffer instead of the heap, so it stays at ≥ 20 KB free (biggest block ≥ 16 KB) while it builds previews, from Settings and from a book's reader menu.

**Architecture:**
- `SdCardFont` gains an optional `BuildArena`: in arena mode every array comes from it and nothing is freed individually.
- `SdCardFontSystem` → `SdCardFontManager` pass that arena through for preview loads.
- `FontSelectionActivity` borrows the secondary framebuffer for its whole lifetime (Home's lend sequence). It loads each uncached preview font inside one arena block, which is rewound after the font is unloaded.
- When nothing can be borrowed, everything runs on the heap as today.

**Tech Stack:** C++20 (firmware `-std=gnu++2a`, ESP32-C3 / PlatformIO), `BuildArena` (`lib/Memory`), GoogleTest host suite (CMake + Ninja, MSYS2 UCRT64).

**Spec:** `docs/superpowers/specs/2026-10-07-font-selector-preview-arena-design.md` (approved 2026-10-07, corrected while planning in commit 2aacebcfe).

## Global Constraints

- **Heap mode is unchanged byte for byte:** same allocation sizes, same `new (std::nothrow)` / `delete[]`, same zero-initialisation of the kern scratch. The reader never sets an arena.
- **Arena fixed for life:** `SdCardFont::useArena()` is called only before `load()` / `loadFromMmap()` and never changed afterwards.
- **Unload before rewind:** a font loaded into an arena block is unloaded before that block is released.
- **The selector draws nothing between shipping the exit busy indicator and returning the buffer** (`onExit`: drop the font, return the buffer, nothing else first).
- No function-local statics; no bare `new` / `new[]`. Use `makeUniqueNoThrow` (`lib/Memory/Memory.h`).
- Run clang-format on every touched file before each commit: `"/c/Program Files/LLVM/bin/clang-format.exe" -i <files>`.
- Host tests are built and run from **Git Bash** only.
- Firmware builds run from **PowerShell** with `$env:PLATFORMIO_CORE_DIR='C:\pio'; & C:\pio\penv\Scripts\pio.exe run -e default`, run in the background (a cold build takes over 10 min).
- Comments explain *why*, at the density of the file they sit in.

## Review Focus

1. **Leaving the selector mid-warm-up** (Back while "Creating font previews" is showing). The font must be unloaded and the buffer returned, with no glyph drawn from a rewound block. Pinned by: Task 2's `onExit` order, and Task 3, device step 3.
2. **Opened from a book while the reader's background chapter build holds the buffer.** The borrow returns null; previews load on the heap exactly as before, and the warm-up floor check still guards that path. Pinned by: Task 2 `lendPreviewArena()` null path, and Task 3, device step 4.
3. **A preview family whose file is broken** (e.g. a truncated `.cpfont`). The arena attempt fails, the block is rewound, and one heap retry fails the same way. No crash, and the row shows no preview, as today. Pinned by: Task 2 `loadPreviewFont()`, and Task 3, device step 5.
4. **Glyphs outside the sample sentence**, drawn through the glyph-miss overflow ring. In arena mode those bitmaps stay in the block until the preview is dropped. The sample is fixed, so this is bounded. Pinned by: Task 1 (`onGlyphMiss` goes through `allocArray` / `freeArray`).
5. **X4 refresh quality while lent, and the first screen after returning.** FAST must not degrade to HALF or leave ghosting. `returnSecondaryBuffer()` arms the RED baseline for the next refresh. Pinned by: Task 3, device step 6 (needs an X4).

---

### Task 1: `SdCardFont` allocator seam

**Files:**
- Modify: `lib/EpdFont/SdCardFont.h`, `lib/EpdFont/SdCardFont.cpp`
- Modify: `test/sd_font_intervals/CMakeLists.txt` (add `lib/Memory` to the include path)
- Create: `test/sd_font_arena/CMakeLists.txt`, `test/sd_font_arena/SdFontArenaTest.cpp`
- Modify: `test/CMakeLists.txt` (add the subdirectory)

**Interfaces:**
- Produces: `void SdCardFont::useArena(BuildArena* arena)` (public).
- Produces, private: `allocArray<T>(size_t)`, `freeArray<T>(T*)`, `makeScratch<T>(size_t)` returning `Scratch<T>`, `uint32_t maxAllocatable() const`, and the member `BuildArena* arena_`.

- [ ] **Step 1: Branch check**

```bash
cd /c/_development/witchhunt-reader && git switch feat/font-selector-preview-arena && git log --oneline -3
```

Expected: the two spec commits on top of master's `Merge pull request #404`.

- [ ] **Step 2: Write the failing tests**

Create `test/sd_font_arena/CMakeLists.txt`:

```cmake
add_executable(SdFontArenaTest
  SdFontArenaTest.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(SdFontArenaTest PRIVATE
  ${REPO_ROOT}/test/zip_entry_reader  # stdio-backed HalStorage.h; shadows test/shims stub
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/test/shims
)

target_link_libraries(SdFontArenaTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SdFontArenaTest)
```

In `test/CMakeLists.txt`, after the line `add_subdirectory(sd_font_intervals)`, add:

```cmake
add_subdirectory(sd_font_arena)
```

Create `test/sd_font_arena/SdFontArenaTest.cpp`:

```cpp
// SdCardFont::useArena(): every array a font allocates comes from a caller's BuildArena, so the font
// selector can load preview fonts into the lent secondary framebuffer instead of the heap. These
// tests pin the contract: the same glyphs, bitmaps and kerning as a heap load; no array allocation
// on the heap; nothing freed into the arena, so rewinding it after the font is gone is clean; a
// prewarm that does not fit whole still loads a prefix sized from the arena's room; and an arena too
// small for the load fails cleanly.
#include <gtest/gtest.h>

#include <Arduino.h>
#include <BuildArena.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

#include "EpdFontData.h"
#include "SdCardFont.h"

namespace {

bool countingArrays = false;
int arrayAllocs = 0;

}  // namespace

void* operator new[](const size_t size) {
  if (countingArrays) ++arrayAllocs;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](const size_t size, const std::nothrow_t&) noexcept {
  if (countingArrays) ++arrayAllocs;
  return std::malloc(size ? size : 1);
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {

constexpr uint32_t HEADER = 32;
constexpr uint32_t TOC_ENTRY = 32;
constexpr uint32_t FIRST_CP = 'A';
constexpr uint32_t LETTERS = 4;           // A..D
constexpr uint32_t GLYPHS = LETTERS + 1;  // + U+FFFD, which prewarm() always resolves
constexpr uint32_t INTERVALS = 2;

void putU16(std::vector<uint8_t>& b, const size_t at, const uint16_t v) {
  b[at] = v & 0xFF;
  b[at + 1] = v >> 8;
}
void putU32(std::vector<uint8_t>& b, const size_t at, const uint32_t v) {
  for (int i = 0; i < 4; i++) b[at + i] = (v >> (8 * i)) & 0xFF;
}

// A one-style v4 .cpfont: glyphs A-D and U+FFFD, each with `bitmapBytes` of a distinct byte pattern.
// With `kerning`, A and B are left classes 1 and 2, B and C right classes 1 and 2, and the 2x2
// matrix holds {-3, 1, 2, -1}, so prewarm builds a mini kern matrix through the SD chunk path.
std::vector<uint8_t> buildFont(const uint16_t bitmapBytes, const bool kerning) {
  const uint16_t kernLeft = kerning ? 2 : 0;
  const uint16_t kernRight = kerning ? 2 : 0;
  const uint8_t leftClasses = kerning ? 2 : 0;
  const uint8_t rightClasses = kerning ? 2 : 0;
  const uint32_t dataStart = HEADER + TOC_ENTRY;
  const uint32_t intervalsAt = dataStart;
  const uint32_t glyphsAt = intervalsAt + INTERVALS * sizeof(EpdUnicodeInterval);
  const uint32_t kernLeftAt = glyphsAt + GLYPHS * sizeof(EpdGlyph);
  const uint32_t kernRightAt = kernLeftAt + kernLeft * sizeof(EpdKernClassEntry);
  const uint32_t matrixAt = kernRightAt + kernRight * sizeof(EpdKernClassEntry);
  const uint32_t bitmapsAt = matrixAt + leftClasses * rightClasses;  // no ligatures
  std::vector<uint8_t> b(bitmapsAt + GLYPHS * bitmapBytes, 0);

  const char magic[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
  std::copy(magic, magic + 8, b.begin());
  putU16(b, 8, 4);   // version
  putU16(b, 10, 0);  // 1-bit
  b[12] = 1;         // one style

  const size_t toc = HEADER;
  b[toc] = 0;  // style id
  putU32(b, toc + 4, INTERVALS);
  putU32(b, toc + 8, GLYPHS);
  b[toc + 12] = 20;                                 // advanceY
  putU16(b, toc + 13, 16);                          // ascender
  putU16(b, toc + 15, static_cast<uint16_t>(-4));   // descender
  putU16(b, toc + 17, kernLeft);
  putU16(b, toc + 19, kernRight);
  b[toc + 21] = leftClasses;
  b[toc + 22] = rightClasses;
  putU32(b, toc + 24, dataStart);

  // A-D from glyph 0, U+FFFD at glyph 4.
  putU32(b, intervalsAt + 0, FIRST_CP);
  putU32(b, intervalsAt + 4, FIRST_CP + LETTERS - 1);
  putU32(b, intervalsAt + 8, 0);
  putU32(b, intervalsAt + 12, 0xFFFD);
  putU32(b, intervalsAt + 16, 0xFFFD);
  putU32(b, intervalsAt + 20, LETTERS);

  for (uint32_t g = 0; g < GLYPHS; g++) {
    const size_t at = glyphsAt + g * sizeof(EpdGlyph);
    b[at] = 8;                            // width
    b[at + 1] = 8;                        // height
    putU16(b, at + 2, 10 << 4);           // advanceX, 12.4 fixed point
    putU16(b, at + 6, 8);                 // top
    putU16(b, at + 8, bitmapBytes);       // dataLength
    putU32(b, at + 12, g * bitmapBytes);  // dataOffset
    for (uint32_t k = 0; k < bitmapBytes; k++) b[bitmapsAt + g * bitmapBytes + k] = static_cast<uint8_t>(g * 31 + k);
  }
  if (kerning) {
    putU16(b, kernLeftAt, 'A');
    b[kernLeftAt + 2] = 1;
    putU16(b, kernLeftAt + 3, 'B');
    b[kernLeftAt + 5] = 2;
    putU16(b, kernRightAt, 'B');
    b[kernRightAt + 2] = 1;
    putU16(b, kernRightAt + 3, 'C');
    b[kernRightAt + 5] = 2;
    const int8_t matrix[4] = {-3, 1, 2, -1};
    std::memcpy(&b[matrixAt], matrix, sizeof(matrix));
  }
  return b;
}

// Named after the running test: ctest -j runs each test in its own process.
std::string writeTemp(const std::vector<uint8_t>& bytes) {
  const auto path =
      std::filesystem::temp_directory_path() /
      (std::string("sd_font_arena_") + testing::UnitTest::GetInstance()->current_test_info()->name() + ".cpfont");
  FILE* f = std::fopen(path.string().c_str(), "wb");
  EXPECT_NE(f, nullptr);
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  return path.string();
}

uint32_t glyphCount(const EpdFontData& d) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < d.intervalCount; i++) n += d.intervals[i].last - d.intervals[i].first + 1;
  return n;
}

void expectSameGlyphs(const EpdFontData& heap, const EpdFontData& arena) {
  ASSERT_EQ(heap.intervalCount, arena.intervalCount);
  ASSERT_EQ(0, std::memcmp(heap.intervals, arena.intervals, heap.intervalCount * sizeof(EpdUnicodeInterval)));
  const uint32_t n = glyphCount(heap);
  ASSERT_EQ(0, std::memcmp(heap.glyph, arena.glyph, n * sizeof(EpdGlyph)));
  for (uint32_t g = 0; g < n; g++) {
    ASSERT_EQ(0, std::memcmp(heap.bitmap + heap.glyph[g].dataOffset, arena.bitmap + arena.glyph[g].dataOffset,
                             heap.glyph[g].dataLength))
        << "glyph " << g;
  }
}

void expectSameKerning(const EpdFontData& heap, const EpdFontData& arena) {
  ASSERT_NE(nullptr, heap.kernMatrix) << "the font's kerning was not exercised";
  ASSERT_EQ(heap.kernLeftEntryCount, arena.kernLeftEntryCount);
  ASSERT_EQ(heap.kernRightEntryCount, arena.kernRightEntryCount);
  ASSERT_EQ(heap.kernLeftClassCount, arena.kernLeftClassCount);
  ASSERT_EQ(heap.kernRightClassCount, arena.kernRightClassCount);
  EXPECT_EQ(0, std::memcmp(heap.kernLeftClasses, arena.kernLeftClasses,
                           heap.kernLeftEntryCount * sizeof(EpdKernClassEntry)));
  EXPECT_EQ(0, std::memcmp(heap.kernRightClasses, arena.kernRightClasses,
                           heap.kernRightEntryCount * sizeof(EpdKernClassEntry)));
  EXPECT_EQ(0, std::memcmp(heap.kernMatrix, arena.kernMatrix, heap.kernLeftClassCount * heap.kernRightClassCount));
}

class CountingArrays {
 public:
  CountingArrays() {
    arrayAllocs = 0;
    countingArrays = true;
  }
  ~CountingArrays() { countingArrays = false; }
};

TEST(SdFontArena, ArenaLoadMatchesHeapLoad) {
  const std::string path = writeTemp(buildFont(64, /*kerning=*/true));
  SdCardFont heap;
  ASSERT_TRUE(heap.load(path.c_str()));
  ASSERT_EQ(0, heap.prewarm("ABCD", 0x01, /*metadataOnly=*/false, /*loadKernLigatureData=*/true));

  BuildArena arena(64 * 1024);
  ASSERT_TRUE(arena.valid());
  SdCardFont inArena;
  inArena.useArena(&arena);
  ASSERT_TRUE(inArena.load(path.c_str()));
  ASSERT_EQ(0, inArena.prewarm("ABCD", 0x01, false, true));
  EXPECT_GT(arena.used(), 0u);

  expectSameGlyphs(*heap.getEpdFont(0)->data, *inArena.getEpdFont(0)->data);
  expectSameKerning(*heap.getEpdFont(0)->data, *inArena.getEpdFont(0)->data);
}

TEST(SdFontArena, ArenaModeAllocatesNoHeapArraysAndRewindsClean) {
  const std::string path = writeTemp(buildFont(64, /*kerning=*/true));
  BuildArena arena(64 * 1024);  // its own buffer is a heap array, so it is made before counting
  ASSERT_TRUE(arena.valid());
  BuildArena::Block block = arena.reserveBlock();
  {
    CountingArrays counting;
    SdCardFont font;
    font.useArena(&arena);
    ASSERT_TRUE(font.load(path.c_str()));
    ASSERT_EQ(0, font.prewarm("ABCD", 0x01, false, true));
    EXPECT_EQ(0, arrayAllocs) << "an array came from the heap in arena mode";
  }  // the font is destroyed here: freeing an arena array must be a no-op, never delete[]
  EXPECT_GT(arena.highWater(), 0u);
  EXPECT_TRUE(arena.release(block));
  EXPECT_EQ(0u, arena.used());
}

TEST(SdFontArena, PrewarmRetrySizesFromTheArenaRoom) {
  // 2000-byte bitmaps: big enough that the retry's 4 KB headroom still leaves room for one glyph,
  // which is what tells arena-based sizing apart from heap-based sizing.
  constexpr uint16_t kBitmap = 2000;
  const std::string path = writeTemp(buildFont(kBitmap, /*kerning=*/false));

  size_t fullUse = 0;  // what a whole prewarm takes in an arena
  {
    BuildArena probe(64 * 1024);
    ASSERT_TRUE(probe.valid());
    SdCardFont font;
    font.useArena(&probe);
    ASSERT_TRUE(font.load(path.c_str()));
    ASSERT_EQ(0, font.prewarm("ABCD", 0x01));
    fullUse = probe.used();
  }

  // Leave 7,500 bytes where the five bitmaps (10,000) go: the first attempt cannot fit, and
  // (7,500 - 4,096) / 2,000 = 1 glyph fits the retry.
  BuildArena arena(fullUse - GLYPHS * kBitmap + 7500);
  ASSERT_TRUE(arena.valid());
  SdCardFont font;
  font.useArena(&arena);
  ASSERT_TRUE(font.load(path.c_str()));
  ESP.setMaxAllocHeap(0);  // the heap has nothing: a heap-sized retry would load no glyph at all
  const int missed = font.prewarm("ABCD", 0x01);
  ESP.setMaxAllocHeap(100 * 1024);
  EXPECT_GT(missed, 0) << "the first attempt was meant not to fit";
  EXPECT_LT(missed, static_cast<int>(GLYPHS)) << "the retry loaded nothing";
}

TEST(SdFontArena, ArenaTooSmallForTheLoadFailsCleanly) {
  const std::string path = writeTemp(buildFont(64, /*kerning=*/true));
  BuildArena arena(16);  // smaller than the 24-byte interval table
  ASSERT_TRUE(arena.valid());
  SdCardFont font;
  font.useArena(&arena);
  EXPECT_FALSE(font.load(path.c_str()));
  EXPECT_GT(arena.failedAllocSize(), 0u);
}

}  // namespace
```

- [ ] **Step 3: Run the tests to verify they fail**

```bash
cd /c/_development/witchhunt-reader && cmake -S test -B build/test -G Ninja > /dev/null && cmake --build build/test -j 8 --target SdFontArenaTest 2>&1 | grep -E " error" | head -3
```

Expected: `error: 'class SdCardFont' has no member named 'useArena'`.

- [ ] **Step 4: `SdCardFont.h`: the seam**

Add `#include <memory>` after `#include <cstdint>`. Add `class BuildArena;` after `class HalFile;`.

After the `loadFromMmap` declaration (`bool loadFromMmap(const uint8_t* base, size_t size, const char* sdPath);`), add:

```cpp

  // Serve every array this font allocates from `arena` instead of the heap. Nothing is freed one
  // array at a time; the caller reclaims it all by rewinding the arena AFTER this font is destroyed.
  // Call before load()/loadFromMmap() and never change it afterwards: an array has to be freed the
  // way it was allocated. The font selector's previews use it (FontSelectionActivity::
  // loadPreviewFont); the reader never does.
  void useArena(BuildArena* arena) { arena_ = arena; }
```

In the private section, directly after `const uint8_t* mmapDataBase_ = nullptr;` and its comment block, add:

```cpp

  // See useArena(). Null: every allocation below is the plain heap call it replaced.
  BuildArena* arena_ = nullptr;
  template <typename T>
  T* allocArray(size_t count);
  template <typename T>
  void freeArray(T* p);
  // A buffer that lives for one call. On the heap it is freed when the scope ends; in an arena it
  // stays until the caller rewinds the font's block, so the deleter does nothing.
  template <typename T>
  struct ScratchDeleter {
    bool heap = true;
    void operator()(T* p) const {
      if (heap) delete[] p;
    }
  };
  template <typename T>
  using Scratch = std::unique_ptr<T[], ScratchDeleter<T>>;
  template <typename T>
  Scratch<T> makeScratch(size_t count);
  // The biggest single allocation the prewarm bitmap retry can make: the arena's remaining room,
  // or the heap's largest free block.
  uint32_t maxAllocatable() const;
```

- [ ] **Step 5: `SdCardFont.cpp`: helpers and the four scratch buffers**

(a) Add `#include <BuildArena.h>` after `#include <Arduino.h>  // ESP.getMaxAllocHeap() ...`.

(b) Directly before `SdCardFont::~SdCardFont() { freeAll(); }`, add:

```cpp
// --- Allocation (see useArena) ---

template <typename T>
T* SdCardFont::allocArray(const size_t count) {
  if (arena_) return arena_->allocArray<T>(count);
  return new (std::nothrow) T[count];
}

template <typename T>
void SdCardFont::freeArray(T* p) {
  if (!arena_) delete[] p;
}

template <typename T>
SdCardFont::Scratch<T> SdCardFont::makeScratch(const size_t count) {
  return Scratch<T>(allocArray<T>(count), ScratchDeleter<T>{arena_ == nullptr});
}

uint32_t SdCardFont::maxAllocatable() const {
  if (arena_) return static_cast<uint32_t>(arena_->capacity() - arena_->used());
  return ESP.getMaxAllocHeap();
}

```

(c) Replace the four `unique_ptr` scratch buffers:

`buildMiniKernMatrix`, kern scratch, currently

```cpp
  std::unique_ptr<uint8_t[]> scratch(new (std::nothrow) uint8_t[6 * 256]());
```

becomes

```cpp
  auto scratch = makeScratch<uint8_t>(6 * 256);
  if (scratch) memset(scratch.get(), 0, 6 * 256);
```

`buildMiniKernMatrix`, SD chunk path, currently

```cpp
    std::unique_ptr<int8_t[]> chunkBuf(new (std::nothrow) int8_t[KERN_CHUNK_BYTES]);
```

becomes

```cpp
    auto chunkBuf = makeScratch<int8_t>(KERN_CHUNK_BYTES);
```

`buildMiniKernMatrix`, per-row fallback, currently

```cpp
      std::unique_ptr<int8_t[]> rowBuf(new (std::nothrow) int8_t[rowBytes]);
```

becomes

```cpp
      auto rowBuf = makeScratch<int8_t>(rowBytes);
```

`prewarm()`, currently

```cpp
  std::unique_ptr<uint32_t[]> codepoints(new (std::nothrow) uint32_t[MAX_PAGE_GLYPHS]);
```

becomes

```cpp
  auto codepoints = makeScratch<uint32_t>(MAX_PAGE_GLYPHS);
```

(d) In `prewarm()`'s bitmap retry, replace

```cpp
      const uint32_t maxAlloc = ESP.getMaxAllocHeap();
```

with

```cpp
      // The arena's room when there is one: sized from the heap, every failed attempt would leave
      // its tables in the arena and the retry would never fit.
      const uint32_t maxAlloc = maxAllocatable();
```

- [ ] **Step 6: `SdCardFont.cpp`: the remaining allocation and free sites**

Apply two rules to every remaining occurrence in `lib/EpdFont/SdCardFont.cpp`, outside the helper definitions from Step 5(b):
- `new (std::nothrow) T[n]` → `allocArray<T>(n)`
- `delete[] x;` → `freeArray(x);`

Inside the static `onGlyphMiss`, prefix both with `self->`: `self->allocArray<uint8_t>(tempGlyph.dataLength)`, `self->freeArray(tempBitmap);`, `self->freeArray(self->overflow_[slot].bitmap);`.

The sites, by line number before this task:
- **Allocations:** 215, 308, 309, 343, 468, 469, 470, 812, 971, 1005, 1308, 1450, 1473, 1486, 1531, 1627, 1640, 1881.
- **Frees:** 50, 52, 54, 106, 107, 111, 126, 128, 130, 142, 180, 248, 312, 313, 321, 322, 331, 332, 350, 357, 1367, 1427, 1453–1455, 1476–1478, 1489–1491, 1520, 1521, 1534, 1535, 1554–1556, 1580–1582, 1589, 1590, 1617, 1631, 1644, 1671, 1672, 1681, 1686, 1889, 1895, 1907.

`CpGlyphMapping` is a local struct inside `prewarmStyle`. `allocArray<CpGlyphMapping>(mappingCapacity)` is valid C++ (local types are allowed as template arguments).

Grep gate:

```bash
cd /c/_development/witchhunt-reader && grep -n "new (std::nothrow)" lib/EpdFont/SdCardFont.cpp; grep -n "delete\[\]" lib/EpdFont/SdCardFont.cpp | grep -v "^\s*[0-9]*:\s*//"; grep -n "unique_ptr<" lib/EpdFont/SdCardFont.cpp
```

Expected: one `new (std::nothrow)` line (inside `allocArray`), one `delete[]` code line (inside `freeArray`; the comment at ~line 949 is filtered out), and no `unique_ptr<` lines.

- [ ] **Step 7: Give the existing SD font test the new include path**

In `test/sd_font_intervals/CMakeLists.txt`, after `${REPO_ROOT}/lib/EpdFont`, add:

```cmake
  ${REPO_ROOT}/lib/Memory
```

- [ ] **Step 8: Run the tests to verify they pass**

```bash
cd /c/_development/witchhunt-reader && "/c/Program Files/LLVM/bin/clang-format.exe" -i lib/EpdFont/SdCardFont.h lib/EpdFont/SdCardFont.cpp test/sd_font_arena/SdFontArenaTest.cpp && cmake -S test -B build/test -G Ninja > /dev/null && cmake --build build/test -j 8 --target SdFontArenaTest SdFontIntervalsTest 2>&1 | grep -E " error" | head -3; ctest --test-dir build/test --output-on-failure -R "SdFontArena|SdFontIntervals"
```

Expected: `100% tests passed out of 9` (4 new + 5 existing).

If `ArenaLoadMatchesHeapLoad` fails on the `heap.kernMatrix` precondition, kerning was not built for this font. Check how `prewarmStyle` decides to call `buildMiniKernMatrix` and fix the *test font* (not the production code) until the heap load builds a matrix. Then rerun.

- [ ] **Step 9: Full host suite and firmware build**

```bash
cd /c/_development/witchhunt-reader && cmake --build build/test -j 8 -- -k 0 > /tmp/t1-build.log 2>&1; grep -E "FAILED:" /tmp/t1-build.log | grep -v epub_build_inventory | head -3; ctest --test-dir build/test -j 8 2>&1 | grep -E "tests passed|tests failed"
```

Expected: no build failures apart from `epub_build_inventory`; `100% tests passed out of 1710` (1706 on master + 4).

Then the firmware build (PowerShell, in the background), per the Global Constraints. Expected: `[SUCCESS]`, RAM unchanged at 53,928 B.

- [ ] **Step 10: Commit**

```bash
cd /c/_development/witchhunt-reader && git add lib/EpdFont/SdCardFont.h lib/EpdFont/SdCardFont.cpp test/CMakeLists.txt test/sd_font_intervals/CMakeLists.txt test/sd_font_arena && git commit -m "feat(fonts): SdCardFont can allocate from a caller's arena (useArena)

Every array goes through allocArray/freeArray. With an arena set, nothing is
freed one array at a time and the caller rewinds the arena after the font is
gone; without one, every call is the heap call it replaced. The prewarm bitmap
retry sizes from the arena's room in arena mode."
```

---

### Task 2: Font selector loads previews into the lent framebuffer

**Files:**
- Modify: `lib/EpdFont/SdCardFontManager.h`, `lib/EpdFont/SdCardFontManager.cpp`
- Modify: `src/SdCardFontSystem.h`, `src/SdCardFontSystem.cpp`
- Modify: `src/activities/settings/FontSelectionActivity.h`, `src/activities/settings/FontSelectionActivity.cpp`
- Modify: `docs/memory-allocation-strategy.md` (§9.1)

**Interfaces:**
- Consumes (Task 1): `SdCardFont::useArena(BuildArena*)`.
- Produces:
  - `bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo&, GfxRenderer&, uint8_t, const std::function<void()>& = {}, FlashCachePolicy = ReadWrite, BuildArena* arena = nullptr)`
  - `void SdCardFontSystem::ensureLoaded(GfxRenderer&, const char*, uint8_t, const std::function<void()>& = {}, FlashCachePolicy = ReadWrite, BuildArena* arena = nullptr)`
  - `void SdCardFontSystem::ensureLoadedForPreview(GfxRenderer&, const char*, uint8_t, BuildArena* arena = nullptr)`
  - private in `FontSelectionActivity`: `lendPreviewArena()`, `returnPreviewArena()`, `loadPreviewFont(const SdCardFontFamilyInfo&)`, `dropPreviewFont()`.

- [ ] **Step 1: Plumbing through the manager**

`lib/EpdFont/SdCardFontManager.h`:
- Add `class BuildArena;` after `class GfxRenderer;`.
- Change the `loadFamily` declaration's last line from `const std::function<void()>& onColdLoad = {}, FlashCachePolicy policy = FlashCachePolicy::ReadWrite);` to:

```cpp
                  const std::function<void()>& onColdLoad = {}, FlashCachePolicy policy = FlashCachePolicy::ReadWrite,
                  BuildArena* arena = nullptr);
```

- Above the declaration, append to its comment block:

```cpp
  // `arena`, when set, is handed to the new SdCardFont (SdCardFont::useArena): the caller owns the
  // block and must unload this family before rewinding it. Only font previews pass one.
```

`lib/EpdFont/SdCardFontManager.cpp`:
- Change the definition's second line to `const std::function<void()>& onColdLoad, const FlashCachePolicy policy, BuildArena* const arena) {`.
- After the `if (!font) { ... return false; }` block that follows `auto* font = new (std::nothrow) SdCardFont();`, add:

```cpp
  font->useArena(arena);
```

- [ ] **Step 2: Plumbing through the font system**

`src/SdCardFontSystem.h`:
- Change the `ensureLoaded(GfxRenderer& renderer, const char* familyName, uint8_t pointSize, …)` declaration's last line to:

```cpp
                    FlashCachePolicy policy = FlashCachePolicy::ReadWrite, BuildArena* arena = nullptr);
```

- Replace `ensureLoadedForPreview` with:

```cpp
  void ensureLoadedForPreview(GfxRenderer& renderer, const char* familyName, uint8_t pointSize,
                              BuildArena* arena = nullptr) {
    ensureLoaded(renderer, familyName, pointSize, {}, FlashCachePolicy::ReadOnly, arena);
  }
```

`src/SdCardFontSystem.cpp`:
- Change the definition `void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer, const char* wantedFamily, const uint8_t pointSize,` / `const std::function<void()>& onColdLoad, const FlashCachePolicy policy) {` so its second line ends `const FlashCachePolicy policy, BuildArena* const arena) {`.
- Change `if (!manager_.loadFamily(*family, renderer, targetPt, onColdLoad, policy)) {` to `if (!manager_.loadFamily(*family, renderer, targetPt, onColdLoad, policy, arena)) {`.

`SdCardFontManager.h`'s forward declaration reaches `SdCardFontSystem.h` through its include.

- [ ] **Step 3: The selector's members**

In `src/activities/settings/FontSelectionActivity.h`, add `#include <BuildArena.h>` and `#include <memory>` with the other includes. After `void drawPreviewFrame() const;`, add:

```cpp
  // The secondary framebuffer, lent for as long as the selector is open. Preview fonts load into it
  // (loadPreviewFont) instead of the heap, which is at its lowest here: ~31 KB free with the Settings
  // screens underneath, and a preview font takes ~12 KB plus transients (X3, 2026-10-07). Null when
  // nothing could be borrowed -- a reader's background build holds it, or the board has none -- and
  // previews then load on the heap as before.
  void lendPreviewArena();
  void returnPreviewArena();
  // The only way the selector loads or drops a preview font. Unloading before the block is rewound
  // is what makes the arena safe, and dropping first keeps one family in it at a time.
  void loadPreviewFont(const SdCardFontFamilyInfo& family);
  void dropPreviewFont();
  std::unique_ptr<BuildArena> previewArena_;
  BuildArena::Block previewBlock_;
```

- [ ] **Step 4: Lend, return, load and drop**

In `src/activities/settings/FontSelectionActivity.cpp`, add `#include <Memory.h>` with the other angle-bracket includes. After `FontSelectionActivity::onExit()`, add:

```cpp
void FontSelectionActivity::lendPreviewArena() {
  if (previewArena_ || !renderer.hasSecondaryBuffer()) return;
  // HomeActivity's lend sequence. Safe with the transition's busy-indicator waveform still running:
  // the copy only reads the displayed frame, and the RED seed and the borrow both drain it first.
  renderer.syncWriteBufferFromDisplayed();
  if (!renderer.isX3()) renderer.syncRedRamFromFrameBuffer();
  size_t lentSize = 0;
  uint8_t* lent = renderer.borrowSecondaryBuffer(&lentSize);
  if (!lent) return;
  previewArena_ = makeUniqueNoThrow<BuildArena>(lent, lentSize);
  if (!previewArena_ || !previewArena_->valid()) {
    previewArena_.reset();
    renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
    return;
  }
  // The selector draws only plain black-and-white frames, so FAST keeps diffing against the seeded
  // RED baseline instead of degrading to HALF on the X4. No-op on X3.
  renderer.setSingleBufferFastDiff(true);
  LOG_DBG("FPRV", "Lent secondary framebuffer for previews (%u bytes)", static_cast<unsigned>(lentSize));
}

void FontSelectionActivity::returnPreviewArena() {
  if (!previewArena_) return;
  previewArena_.reset();
  renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
  renderer.setSingleBufferFastDiff(false);
}

void FontSelectionActivity::loadPreviewFont(const SdCardFontFamilyInfo& family) {
  dropPreviewFont();
  if (previewArena_) {
    previewBlock_ = previewArena_->reserveBlock();
    sdFontSystem.ensureLoadedForPreview(renderer, family.name.c_str(), selectedFontSize(), previewArena_.get());
    if (sdFontSystem.resolveFontId(family.name.c_str(), selectedFontSize()) != 0) return;
    LOG_ERR("FPRV", "%s did not load into the lent buffer (%u of %u bytes used); trying the heap",
            family.name.c_str(), static_cast<unsigned>(previewArena_->used()),
            static_cast<unsigned>(previewArena_->capacity()));
    dropPreviewFont();
  }
  sdFontSystem.ensureLoadedForPreview(renderer, family.name.c_str(), selectedFontSize());
}

void FontSelectionActivity::dropPreviewFont() {
  sdFontSystem.unload(renderer);
  // Only after the unload: the font's arrays live in this block (SdCardFont::useArena).
  if (previewArena_ && previewBlock_.valid()) previewArena_->release(previewBlock_);
}
```

- [ ] **Step 5: Route the existing load and unload sites through them**

In `FontSelectionActivity.cpp`:
- **`onEnter()`:** directly after `RenderLock lock(*this);`, add `lendPreviewArena();`. It must precede `UiListActivity::onEnter();` and `updatePreviewFontLocked(selectedIndex);`.
- **`onExit()`:** replace the body's `sdFontSystem.unload(renderer);` with:

```cpp
  // Nothing may be drawn before the return: the exit's busy indicator may still be finishing, and
  // with the buffer lent its finish reads the write buffer (see returnPreviewArena).
  dropPreviewFont();
  returnPreviewArena();
```

- **`prepareSdPreview()`:**
  - cached-strip branch: `sdFontSystem.unload(renderer);` → `dropPreviewFont();`
  - load: `sdFontSystem.ensureLoadedForPreview(renderer, family.name.c_str(), selectedFontSize());` → `loadPreviewFont(family);`
- **`updatePreviewFontLocked()`, built-in branch:** `sdFontSystem.unload(renderer);` → `dropPreviewFont();`
- **`advanceWarmup()`:** `sdFontSystem.unload(renderer);` → `dropPreviewFont();`, and change the floor check's condition from `if (largest < WARMUP_MIN_CONTIGUOUS) {` to:

```cpp
  // The brake only matters on the heap path: with the buffer lent, previews never touch the heap.
  if (!previewArena_ && largest < WARMUP_MIN_CONTIGUOUS) {
```

Grep gate:

```bash
cd /c/_development/witchhunt-reader && grep -n "sdFontSystem\.\(unload\|ensureLoadedForPreview\)" src/activities/settings/FontSelectionActivity.cpp
```

Expected: exactly three lines, all inside `loadPreviewFont` / `dropPreviewFont`: one `ensureLoadedForPreview` with the arena, one without, and one `unload`. Any other hit is a missed site.

- [ ] **Step 6: Docs**

In `docs/memory-allocation-strategy.md` §9.1, in the "Borrow" list, after the bullet that ends `the file browser's cover work (`FileBrowserActivity::lendForBackgroundWork`).`, add:

```markdown
- The font selector's previews (`FontSelectionActivity::lendPreviewArena`): each uncached preview
  loads its SD font into the lent region (`SdCardFont::useArena`), one block per font, rewound
  after the font is unloaded. With nothing to lend, previews load on the heap.
```

- [ ] **Step 7: Build and run everything**

```bash
cd /c/_development/witchhunt-reader && "/c/Program Files/LLVM/bin/clang-format.exe" -i lib/EpdFont/SdCardFontManager.h lib/EpdFont/SdCardFontManager.cpp src/SdCardFontSystem.h src/SdCardFontSystem.cpp src/activities/settings/FontSelectionActivity.h src/activities/settings/FontSelectionActivity.cpp && cmake --build build/test -j 8 -- -k 0 > /tmp/t2-build.log 2>&1; grep -E "FAILED:" /tmp/t2-build.log | grep -v epub_build_inventory | head -3; ctest --test-dir build/test -j 8 2>&1 | grep -E "tests passed|tests failed"
```

Expected: `100% tests passed out of 1710`. Then the firmware build, expected `[SUCCESS]`. No new host test is added here: the selector and the font system need a `GfxRenderer` and a display, which the host suite cannot provide. Task 3 covers them on the device.

- [ ] **Step 8: Commit**

```bash
cd /c/_development/witchhunt-reader && git add lib/EpdFont/SdCardFontManager.h lib/EpdFont/SdCardFontManager.cpp src/SdCardFontSystem.h src/SdCardFontSystem.cpp src/activities/settings/FontSelectionActivity.h src/activities/settings/FontSelectionActivity.cpp docs/memory-allocation-strategy.md && git commit -m "feat(fonts): font selector loads previews into the lent framebuffer

The selector borrows the secondary framebuffer while it is open (Home's lend
sequence, so the X4 keeps FAST) and loads each uncached preview font into
one arena block, rewound after the font is unloaded. The ~12 KB plus
transients a preview cost the heap now leave it alone; with nothing to
borrow, previews load on the heap as before."
```

---

### Task 3: Device check, cleanup, PR

The user flashes and runs these; the agent watches the serial log (`serial-log-capture-c3` memory: DTR/RTS low before open, stop the logger before any flash).

- [ ] **Step 1: Flash the Task 2 build.** Start the logger after the device boots.

- [ ] **Step 2: Settings path, uncached previews.** Change Font Size to a size with no stored previews (for example 18 pt), then open Settings › Reader › EPUB Font › Font Family. Expected in the log:
  - `[FPRV] Lent secondary framebuffer for previews (52272 bytes)`;
  - each `[FPRV] font=N free=A->B` moving by at most 3 KB;
  - `[MEM] Min Free` at least 20 KB;
  - no `Stopping warm-up`.

- [ ] **Step 3: Leave mid-warm-up.** Re-enter at another uncached size and press Back while "Creating font previews" shows. Expected: a clean return to the submenu, no `[ERR]`, no corrupted screen.

- [ ] **Step 4: Reader-menu path.** Open a book and, while the log still shows `Background-C: building` (just after opening), open the reader menu › Font Family. Expected: either the lend line, or none and previews still working on the heap. No crash; the book renders correctly afterwards.

- [ ] **Step 5: Broken file.** With a truncated `.cpfont` family on the card (like the earlier `Vollkorn_16.cpfont`), move the cursor onto it. Expected: `did not load into the lent buffer … trying the heap`, then the existing load error; no crash.

- [ ] **Step 6 (needs an X4): refresh quality.** Navigate the selector list and exit. Expected: fast refreshes (about 500 ms `displayBuffer … FAST`), no HALF downgrade, no ghosting on the next screen.

- [ ] **Step 7: Byte-identical strip.** Copy `/.crosspoint/fontprev/<Family>_<size>_468x190_0.fpv`, built by this firmware. Delete it and rebuild it on master's firmware, then compare the two files byte for byte (`cmp`). Expected: identical.

- [ ] **Step 8: Remove the working docs and open the PR**

```bash
cd /c/_development/witchhunt-reader && git rm -q docs/superpowers/specs/2026-10-07-font-selector-preview-arena-design.md docs/superpowers/plans/2026-10-07-font-selector-preview-arena.md && git commit -q -m "docs: drop the working spec and plan (design recorded in memory-allocation-strategy §9.1)" && git push -u origin feat/font-selector-preview-arena
```

Then `gh pr create --base master` with the repo template: goal, the measurements before and after, the device results from Steps 2–7, and AI usage YES.
