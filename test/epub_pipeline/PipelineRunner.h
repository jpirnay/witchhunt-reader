#pragma once
// Drives the REAL EPUB compilation pipeline (Epub load/index, CSS compile,
// footnote gather, per-spine Section build) on the host and emits a canonical
// text dump of the resulting layout: per page, every element's position, every
// word's text/x-position/style. Two runs over the same book with the same
// profile must produce byte-identical dumps (determinism), and any layout
// refactor must keep the dump unchanged (golden equivalence). See
// docs/compiled-book-pipeline-plan.md Phase 0.
#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "Epub/content/ContentSink.h"
#include "Epub/content/LayoutSink.h"

class Page;

namespace pipeline_harness {

// A named render-settings profile — the section-cache variant under test.
struct Profile {
  const char* name = "default";
  int fontId = 1;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 460;
  uint16_t viewportHeight = 760;
  bool hyphenationEnabled = false;
  bool embeddedStyle = true;
  bool bionicReadingEnabled = false;
  bool inlineFootnotePreviews = true;
  uint8_t imageRendering = 0;
};

// Called after each spine item's build+dump with its wall-clock cost.
// pages/elapsedUs cover the section build AND the page-by-page dump read-back.
using SpineStatFn = std::function<void(int spineIndex, uint16_t pages, int64_t elapsedUs)>;

// Compile `epubPath` into `cacheDir` under `profile` and stream the canonical
// dump to `out`. Returns false on any pipeline failure (already-logged).
// `cacheDir` should be empty/fresh for a cold run; a second call over the same
// cacheDir exercises the warm (cache-hit) path and must dump identically.
bool runAndDump(const std::string& epubPath, const std::string& cacheDir, const Profile& profile, std::ostream& out,
                const SpineStatFn& spineStat = {});

// Stage-1 driver (Phase 3 step 4): compile every spine of `epubPath` through one
// ContentSink, filling `sink` with the whole book's settings-independent CompiledContent.
// Attaches the sink via Section::setStage1Sink and builds each spine at `profile` (the
// producer is settings-independent, but a build must run to drive it). Returns false on
// any pipeline failure (already-logged to `out`).
bool compileContent(const std::string& epubPath, const std::string& cacheDir, const Profile& profile,
                    compiled::ContentSink& sink, std::ostream& out);

// Increment-D gate: for spine `spineIndex`, build the section-cache file TWO ways — (1) the normal
// parse (createSectionFile), (2) the content.bin read-back (buildSectionFromContentBin) — and return
// true iff the two section files are BYTE-IDENTICAL. This proves the device read-back path produces
// exactly today's cache. content.bin is compiled into the book's cache dir first. `out` collects
// diagnostics. Returns false (with a message) on any build failure OR a byte diff.
// sliced=false drives the run-to-completion buildSectionFromContentBin; sliced=true drives the
// resumable stepReadBackFromContentBin with a tiny (1 ms) budget so the read-back yields many times
// mid-spine — both must produce a section file byte-identical to the parse (Increment E sub-step 2).
bool sectionEquivalence(const std::string& epubPath, const std::string& cacheDir, int spineIndex,
                        const Profile& profile, std::ostream& out, bool sliced = false);

// Increment F tee gate: build the spine's section cache via the TEE (Section::setStage1TeeSink with a
// ContentBinWriter attached), so ONE walk emits both the section cache AND content.bin. Asserts the
// tee section file is byte-identical to a plain parse (pages unaffected by the fan-out) AND that a
// read-back from the tee-emitted content.bin is byte-identical to the parse (content.bin correctly
// emitted). Returns false on any mismatch (logged to out).
bool teeEquivalence(const std::string& epubPath, const std::string& cacheDir, int spineIndex, const Profile& profile,
                    std::ostream& out);

// Step 5 equivalence driver: build every spine of `epubPath` driving a compiled::LayoutSink
// (attached via Section::setStage1Sink) instead of reading the fused section cache, and dump
// the LayoutSink's Page stream in the SAME canonical format as runAndDump. Diffing the two
// dumps is the byte-identical gate. Returns false on any pipeline failure (already-logged).
bool layoutViaSink(const std::string& epubPath, const std::string& cacheDir, const Profile& profile, std::ostream& out);

// #1 (content.bin persistence) driver: Stage-1 compile `epubPath` into a ContentSink, serialize to
// content.bin, read it BACK, then replay the CompiledContent through a LayoutSink (reconstructing
// the walk's BlockSink call order) and dump the pages in the SAME format as runAndDump/layoutViaSink.
// Diffing against layoutViaSink is the read-back byte-identical gate: it proves a settings-change
// relayout from content.bin (no ZIP/XML/CSS) reproduces the layout exactly. Returns false on failure.
bool layoutViaContentBin(const std::string& epubPath, const std::string& cacheDir, const Profile& profile,
                         std::ostream& out);

// Speed-gate split of layoutViaContentBin, so a benchmark can time the two stages independently:
//   compileToContentBin  = the FULL Stage-1 work (ZIP + inflate + Expat + CSS + walk) + serialize.
//   replayFromContentBin = the settings-change FAST PATH: read content.bin + Stage-2 layout only.
// A settings change today re-runs compileToContentBin's work; with content.bin it re-runs only
// replayFromContentBin. The ratio is the Phase-3 ">=3x faster relayout" gate. `out` is the dump
// (discardable for timing). Both return false on failure (logged to out).
bool compileToContentBin(const std::string& epubPath, const std::string& cacheDir, const Profile& profile,
                         std::ostream& out);
bool replayFromContentBin(const std::string& epubPath, const std::string& cacheDir, const Profile& profile,
                          std::ostream& out);

// Dump one already-built page in the canonical PAGE/LINE/IMG/TABLE/HR/FN format (shared by
// runAndDump and layoutViaSink so both sides of the equivalence diff format identically).
void dumpOnePage(std::ostream& out, const Page& page, uint16_t pageIndex, const std::string& cacheDir);

}  // namespace pipeline_harness
