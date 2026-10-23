#pragma once

#include <Epub/TypographyProfile.h>

#include <cstdint>

// The free heap an in-place section build (the X4's resident build: the secondary framebuffer stays where it is, and
// the build runs on the owned 10 KB heap arena) needs at its start, EpubReaderActivity::heapAllowsInPlaceBuild.
namespace InPlaceBuildGate {

// The larger of the build's two phases' peaks, never their sum: runBuildParse frees the inflate ring before the parse
// begins. Extraction holds the ring, `extractBaseBytes` + `ringBytes`; the parse holds the layout's working set,
// `parseBaseBytes`, and what the TrueType misses take from the heap, `ttfMissBytes` (ttfMissBytesFor()).
constexpr uint32_t minFreeHeap(const uint32_t parseBaseBytes, const uint32_t extractBaseBytes, const uint32_t ringBytes,
                               const uint32_t ttfMissBytes) {
  const uint32_t parse = parseBaseBytes + ttfMissBytes;
  const uint32_t extract = extractBaseBytes + ringBytes;
  return parse > extract ? parse : extract;
}

// What the parse phase must leave for the misses of a loaded TrueType set, whose memory for one is `missPlanBytes`
// (TtfFontSet::missMemoryPlanBytes: the scratch block, in SD mode the page cache too). A miss takes it from the region
// lent to the misses, and a resident build lends none (Section::stepSectionBuild), so it comes from the heap and stays
// until the slice gives RenderLock back (docs/memory-allocation-strategy.md §9.7).
//
// Counted for a book in Chinese, Japanese or Korean, where misses are the rule: thousands of characters, so a chapter
// the glyph store has not seen at this size and family brings glyphs it lacks. X4, 2026-10-23, CJK chapters of
// 1.5-9.6 KB, store empty: admitted at about 67.7 KB free, the first miss took 22,792 B for a CFF family's 17,408 +
// 4,204 B, and the builds fell to 8-20 KB, under the floors a build is stopped at, and were rebuilt borrowed. Not for
// a book set between spaces: a style's hundred-odd glyphs are stored within its first pages, and a small chapter's
// resident build held a miss and finished (two TrueType families, 0.3 KB chapters, store empty and full), where
// borrowing would cost it the indexing popup the resident build defers (about 0.54 s).
inline uint32_t ttfMissBytesFor(const TypographyProfile& typography, const uint32_t missPlanBytes) {
  return typography.breaking == TypographyProfile::Breaking::AtSpaces ? 0 : missPlanBytes;
}

}  // namespace InPlaceBuildGate
