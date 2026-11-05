#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "IndicScripts.h"

// Ours (this repo's): the Indic shaping module's text rules. The run scan is ported from crosspoint-reader PR #3787
// ("feat: add Indic language support without HarfBuzz", SSafayet / @SSafayet).
namespace IndicShaper {

// One run of a token, upstream's scanRun: one script's letters with the shared code points around them (dandas, Vedic
// signs, joiners, the dotted circle), bytes [begin, end); `script` null for shared code points alone.
struct Run {
  uint16_t begin;
  uint16_t end;
  const indic::ScriptInfo* script;
};

// The most runs a token of `bytes` bytes can hold: a run takes 3 bytes at least, and two runs of different scripts
// abut with no byte between them.
constexpr size_t maxRuns(size_t bytes) { return bytes / 3 + 1; }

// The token's runs, in order, into `out` (room for maxRuns(text.size())); text outside them is not listed. A token of
// more than UINT16_MAX bytes has none listed.
size_t scanRuns(std::string_view text, Run* out);

// Whether `text` holds a byte sequence of U+0900-0DFF (indic::startsIndic), bounded by the view.
bool holdsIndic(std::string_view text);

}  // namespace IndicShaper
