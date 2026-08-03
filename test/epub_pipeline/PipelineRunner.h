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
#include <ostream>
#include <string>

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
  bool fontSizeNormalization = true;
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

}  // namespace pipeline_harness
