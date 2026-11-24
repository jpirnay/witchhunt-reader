#pragma once

#include <cstddef>
#include <cstdint>

// Small ASCII parsers in place of sscanf. One sscanf call links newlib's whole scanf engine and the float parser
// behind it, about 16 KB of flash on the X3.
namespace ascii_parse {

// The value of one hex digit of either case, or -1.
int hexNibble(char c);

// Decodes exactly 2 * count hex digits from `hex` into `out`. False when any of them is not a hex digit (a
// terminator within the range included); `out` is then left unchanged.
bool hexBytes(const char* hex, uint8_t* out, size_t count);

// The whole of text[0, length) as a dotted quad: four decimal numbers 0..255 joined by '.', nothing before, after or
// in between. `out` is left unchanged when it is not one.
bool ipv4(const char* text, size_t length, uint8_t out[4]);

}  // namespace ascii_parse
