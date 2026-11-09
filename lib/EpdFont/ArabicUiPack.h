#pragma once

#include <cstddef>
#include <cstdint>

/// The Arabic UI pack's name and characters (lib/EpdFont/scripts/build_ui_script_pack.py writes it;
/// src/UiScriptPack.cpp opens it). A .cpfont family on the card, /.crosspoint/fonts/ArabicUI/, that the UI faces'
/// providers draw from and the reader never does: SdCardFontRegistry leaves it out of the reading fonts.
namespace arabic_ui_pack {

constexpr const char* FAMILY = "ArabicUI";

struct Range {
  uint16_t first;
  uint16_t last;
};

/// What the pack holds, exactly: every presentation form the mapper emits (shape_logical: Forms-B U+FE80-FEFC and the
/// Forms-A forms of minibidi.c's xshapeforms), the harakat and superscript alef, and the Arabic and Persian digits and
/// punctuation. The host test ui_script_pack checks the pack files hold these and nothing else.
constexpr Range RANGES[] = {
    {0x060C, 0x060C}, {0x061B, 0x061B}, {0x061F, 0x061F}, {0x0640, 0x0640}, {0x064B, 0x0652},
    {0x0660, 0x066C}, {0x0670, 0x0670}, {0x06D4, 0x06D4}, {0x06F0, 0x06F9}, {0xFB50, 0xFBB1},
    {0xFBD3, 0xFBDC}, {0xFBDE, 0xFBE7}, {0xFBFC, 0xFBFF}, {0xFE80, 0xFEFC},
};

/// Whether the pack holds `codepoint`.
constexpr bool covers(const uint32_t codepoint) {
  if (codepoint < 0x060C || codepoint > 0xFEFC) return false;
  for (const Range& range : RANGES) {
    if (codepoint >= range.first && codepoint <= range.last) return true;
  }
  return false;
}

}  // namespace arabic_ui_pack
