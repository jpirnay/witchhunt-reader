#pragma once

#include "NorFlash.h"

/// The raw flash area the firmware keeps its caches in, all of them through this one NorFlash: the .cpfont cache and
/// the language slot (FlashFontPartition, FlashLangPartition), the TTF glyph store and the hyphenation store (through a
/// FlashAreaRegion each), and SystemStatus. Offsets run from the area's start, as they always ran from the `spiffs`
/// partition's. docs/design/flash-area.md has the consumers, the backends and how a board gets one.
namespace FlashArea {

/// The board's area, chosen at the first call and kept for the run; nullptr when the board has none.
NorFlash* get();

}  // namespace FlashArea
