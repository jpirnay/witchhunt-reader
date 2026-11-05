#include "IndicShaper.h"

#include "TextShaping.h"

// Ours (this repo's), with scanRuns ported from crosspoint-reader PR #3787 ("feat: add Indic language support without
// HarfBuzz", SSafayet / @SSafayet): scanRun in lib/EpdFont/ComplexShaper.cpp, over a bounded view, listing every run.
namespace IndicShaper {

size_t scanRuns(std::string_view text, Run* out) {
  if (text.size() > UINT16_MAX) return 0;
  const size_t room = maxRuns(text.size());
  size_t count = 0;
  size_t pos = 0;
  while (pos < text.size() && count < room) {
    size_t next = pos;
    const uint32_t first = TextShaping::decodeAt(text, next);
    if (indic::scriptOf(first) == nullptr && !indic::isShared(first)) {
      pos = next;
      continue;
    }
    const indic::ScriptInfo* script = nullptr;
    const size_t begin = pos;
    while (pos < text.size()) {
      next = pos;
      const uint32_t cp = TextShaping::decodeAt(text, next);
      const indic::ScriptInfo* cpScript = indic::scriptOf(cp);
      if (cpScript == nullptr && !indic::isShared(cp)) break;
      if (cpScript != nullptr) {
        if (script != nullptr && cpScript != script) break;
        script = cpScript;
      }
      pos = next;
    }
    out[count++] = Run{static_cast<uint16_t>(begin), static_cast<uint16_t>(pos), script};
  }
  return count;
}

bool holdsIndic(std::string_view text) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
  for (size_t i = 0; i + 3 <= text.size(); i++) {
    if (indic::startsIndic(bytes + i)) return true;
  }
  return false;
}

}  // namespace IndicShaper
