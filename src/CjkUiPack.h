#pragma once

#include <EpdFontData.h>
#include <I18n.h>

#include <cstdint>

/// The CJK UI packs on the card (cjk_ui_pack::PACKS; lib/EpdFont/scripts/build_ui_script_pack.py): the CJK characters
/// of book titles the cjk_ui_* faces lack, in each region's forms, at the CJK UI sizes. The CJK fallback of the UI
/// faces (FallbackFaceProvider::setPacks, wired by CjkUiFonts) searches them for book text before the last book's font,
/// whatever book that was. Without one installed, titles draw as before: the last book's font, else boxes.
///
/// A pack's file is opened the first time a string needs it (an SdCardFont: header and its handful of intervals, the
/// object itself 2,472 B on the C3, both sizes in it), and each string's pack characters are read in one pass when it
/// is measured or drawn (SdCardFont::prewarm), kept until that size's next string. Everything is freed at the next
/// screen (release()).
namespace CjkUiPack {

/// The UI language: its region's pack goes first for Han characters (cjk_ui_pack::order).
void setLanguage(Language language);

/// Whether an installed pack holds `codepoint` at `points`, without opening one.
bool covers(uint8_t points, uint32_t codepoint);

/// The face of the first pack, in the current string's order, that holds `codepoint` at `points`; opened on first
/// ask. nullptr when no installed pack holds it.
const EpdFontData* face(uint8_t points, uint32_t codepoint);

/// `utf8Text`'s pack characters (book text the UI face lacks) are measured or drawn next at `points`: orders the packs
/// for it and reads each pack's share in one pass, unless its table holds them all already.
void prepare(uint8_t points, const char* utf8Text);

/// Frees every open file and forgets what is installed: at a screen change (syncUiBookFonts), under the RenderLock.
void release();

}  // namespace CjkUiPack
