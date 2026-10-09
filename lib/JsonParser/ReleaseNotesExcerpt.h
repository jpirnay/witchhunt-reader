#pragma once

#include <cstddef>

namespace release_notes {

// The opening lines of a GitHub release's notes ("body", Markdown), made fit for the update
// confirmation dialog, which has room for a few lines of plain text and nothing else.
//
// Headings, blank lines, rules ("---") and the "**Full Changelog**" link line are left out, as is
// the " by @author in <pull URL>" tail GitHub's generated notes give every entry. Every bullet
// becomes "- ". Backticks and "**" go, and "[text](url)" keeps only its text. Lines come out
// joined by '\n', at most `maxLines` of them.
//
// `complete` is false when `body` is only the start of the notes, as the update check stops
// reading partway through a long body: the last line is then cut off and is left out, so a
// half-received word or codepoint never shows. A line that does not fit in `out` is cut at a word
// (or else a codepoint) and ends in an ellipsis.
//
// Writes at most `outSize - 1` bytes and a NUL; returns the length. `outSize` must be at least 1.
size_t excerpt(const char* body, size_t len, bool complete, char* out, size_t outSize, size_t maxLines);

}  // namespace release_notes
