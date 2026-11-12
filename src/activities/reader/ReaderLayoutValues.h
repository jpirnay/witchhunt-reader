#pragma once

#include <cstdint>

// What the reader bars' Layout tab sets: the values a book is laid out and drawn with, in the numbering the settings
// store them in (CrossPointSettings). The reader fills in one set for the book (its own settings, else the global ones)
// and one for the global settings; the bars hand back the set they changed (ReaderBarsResult). Kept to plain numbers,
// with no header of the firmware's, because every activity sees it through ActivityResult.h. The controls that change
// them are ReaderLayoutControls.h.
struct ReaderLayoutValues {
  // 0: the book's own fonts (Publisher Fonts, All Text); else 1 + a font-family option (fontFamilyOptionCount's
  // numbering: the built-in families, then the card's).
  uint8_t font = 0;
  uint8_t fontSize = 0;          // points
  uint8_t lineSpacing = 0;       // a LineSpacingStep
  uint8_t marginHorizontal = 0;  // pixels
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;  // CrossPointSettings::PARAGRAPH_ALIGNMENT
  bool hyphenation = false;
  uint8_t textDarkness = 0;  // CrossPointSettings::TEXT_DARKNESS
  bool embeddedStyle = false;

  bool operator==(const ReaderLayoutValues& other) const = default;
};
