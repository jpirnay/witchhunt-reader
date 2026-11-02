#pragma once

#include <cstddef>
#include <cstdint>

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

}  // namespace TextShaping
