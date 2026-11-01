#pragma once

#include <cstddef>
#include <cstdint>

// How a character is set in a vertical column: UAX #50's Vertical_Orientation (Unicode 16.0), a property of the
// character. R sideways, U upright, Tu and Tr upright or sideways with a vertical form of their own where the font
// has one.
enum class VerticalOrientation : uint8_t { R = 0, U = 1, Tu = 2, Tr = 3 };

VerticalOrientation verticalOrientationOf(uint32_t cp);

// The one place a class becomes a drawing: U and Tu upright in the reader's frame, R and Tr sideways through the
// turned frame. No vertical forms; uprightPlacementOf (below) says which upright marks the draw moves within their
// cell: 、。，． and the small kana.
bool drawsUpright(uint32_t cp);

// How an upright character is placed inside its cell from its glyph's ink box. Reflect: the comma and full stop,
// which sit at the lower left of a horizontal cell and go to the upper right of a vertical one. SmallKana: the small
// kana, which move up and right the same way. None: set as step 3 does.
enum class UprightPlacement : uint8_t { None, Reflect, SmallKana };

UprightPlacement uprightPlacementOf(uint32_t cp);

// The next piece the vertical draw path sets as one unit: a single upright character, or the longest run of sideways
// ones up to kMaxColumnRunBytes; either keeps the combining marks (utf8IsCombiningMark, U+3099, U+309A) and variation
// selectors (U+FE00-FE0F, U+E0100-E01EF) that follow a character, and a run never ends inside such a cluster. Returns
// the piece's length in bytes (0 at the end of `text`), and whether it is upright.
constexpr size_t kMaxColumnRunBytes = 64;
size_t nextColumnPiece(const char* text, bool& upright);

// The first code point of a UTF-8 string; 0 for an empty one.
uint32_t firstCodepointOf(const char* utf8);
