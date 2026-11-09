#pragma once

#include <EpdFontData.h>

#include <cstdint>

/// The Arabic UI pack: the Arabic presentation forms, harakat, digits and punctuation at the built-in UI faces' sizes
/// and weights (6, 8, 10, 12 and 14 pt; lib/EpdFont/scripts/build_ui_script_pack.py), a 1-bit .cpfont family the Font
/// Manager installs like any SD family: /.crosspoint/fonts/ArabicUI/ArabicUI_<pt>.cpfont. It is no reading font: the
/// registry leaves it out (arabic_ui_pack::FAMILY). The UI faces' providers (UiFaceProvider) serve a code
/// point the face lacks from it, before the last book's font, whatever book was read last; without it, Arabic UI text
/// falls to that font as before, or draws nothing.
///
/// A size's file is opened the first time a string drawn at that size needs it (an SdCardFont: its header and
/// interval tables, about 2.7 KB of heap), and each string's pack characters are read from the card in one pass when
/// it is measured or drawn (SdCardFont::prewarm); a string with no Arabic opens and reads nothing. Everything is freed
/// at the next screen (release()).
namespace UiScriptPack {

/// Whether the pack holds `codepoint` (arabic_ui_pack::covers).
bool covers(uint32_t codepoint);

/// Whether the pack's file for a UI face of `points` is on the card. Asked of the card once per screen.
bool installed(uint8_t points);

/// The pack's face for a UI face of `points`, bold or not, its file opened on first ask; nullptr when the file is not
/// on the card, does not load, or has no such style.
const EpdFontData* face(uint8_t points, bool bold);

/// `utf8Text` is measured or drawn next in a UI face of `points`: its pack characters are read in one pass unless
/// the face's table holds them all already.
void prepare(uint8_t points, bool bold, const char* utf8Text);

/// Frees every open file and forgets what is installed: at a screen change (syncUiBookFonts), under the RenderLock.
void release();

}  // namespace UiScriptPack
