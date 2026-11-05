#pragma once

// Ours (writing modes step 2): where a style block of the glyph store gives the shaper its tables.

#include <OtFace.h>

#include <cstdint>

namespace ttf {

/// The views a style block gives the shaper. Flash mode (fileBytes > 0): the file's own tables, and the CPac at
/// align4(fileBytes) up to blockBytes. SD mode (fileBytes == 0): the block's directory, its CPac record included.
/// CPpl is always empty (plans are built, never read), and a file's own CPac is never taken.
bool shapingTables(const uint8_t* block, uint32_t blockBytes, uint32_t fileBytes, ot::FaceTables* out);

}  // namespace ttf
