#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// The shaping layer: turns one token's logical UTF-8 text into the code points that are drawn, when they differ
// from the text's own. Each script has a module (HebrewShaper today); shape() picks the modules a token needs.
namespace TextShaping {

// One drawn code point and the byte offset, in the token's logical UTF-8 text, of the source code point
// (or the first one of the matched span) it came from. HarfBuzz calls this offset a cluster.
struct Unit {
  uint32_t cp;
  uint16_t cluster;
};

// Shapes one token's logical UTF-8 text (len bytes, need not be NUL-terminated). Returns true and sets
// units[0..count) when what is drawn differs from the text's own code points. Returns false, leaving count and
// units untouched, when the text is drawn as it is, or when the result would need more than `capacity` units.
bool shape(const char* text, size_t len, Unit* units, size_t capacity, size_t& count);

// The code point at text[pos], never reading past the view, and pos moved past it. A bad or truncated sequence is
// U+FFFD and consumes what utf8NextCodepoint would; a NUL inside the view is U+0000, one byte. pos < text.size().
uint32_t decodeAt(std::string_view text, size_t& pos);

// One token of a group of glued tokens (no space and no break between them), in logical order.
struct GroupToken {
  std::string_view text;  // logical UTF-8
  uint32_t styleKey;      // equal keys: the same font, style, size and scale
};

// Whether `token`, glued after `previous` (a token merged so far, when earlier merges made it), joins it before
// anything is measured or shaped:
// - deviation 7, when `indicGroup` (the group holds a letter of U+0900-0DFF and its paragraph shapes): the two share
//   a style key, or the glue falls inside a syllable (indic::syllableBreakAllowed false across it);
// - the Hebrew name, always: `token` holds transparent marks only, and `previous` ends with the divine name.
bool mergesWithPrevious(const GroupToken& previous, const GroupToken& token, bool indicGroup);

// Frees the build's heap block for shaping calls (IndicShaper::CallMemory). A section build's end calls it, as it
// calls BidiLine::releaseBuildMemory(); the next call makes it again.
void releaseBuildMemory();
bool buildMemoryHeld();  // for tests

// The layer's own, for IndicShaper::CallMemory when the arena lends no room: the build's heap block of at least
// `bytes`, made on first use and grown nothrow (the smaller one freed first), held until releaseBuildMemory(). Lent to
// one call at a time: null while it is lent, until returnBuildHeapBlock(). Null, logged once, when there is no room.
// Builds and the dictionary's pages run under RenderLock, one call at a time.
void* buildHeapBlock(size_t bytes);
void returnBuildHeapBlock();

}  // namespace TextShaping
