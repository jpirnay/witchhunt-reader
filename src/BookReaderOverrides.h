#pragma once

#include <cstdint>
#include <string>

// A book's own reader settings. Each value is -1 (the SD family name empty) while the book follows the global
// setting, otherwise the value the book is read with.
//
// The field names keep the "Override" suffix they had as RecentBook members, so the reader's book*Override members and
// these read alike.
struct BookReaderOverrides {
  // Each settable value, in the order a file stores them. APPEND ONLY: a file written by an older firmware holds the
  // leading fields and a newer one more, and both are read by position (BookReaderOverrides::setFromStored).
  enum class Field : uint8_t {
    EmbeddedStyle,
    ImageRendering,
    FontFamily,
    FontSize,
    BionicReading,
    ParagraphAlignment,
    TextAntiAliasing,
    Hyphenation,
    FontSizeNormalization,
    GuideDots,
    InlineFootnotePreviews,
    LineSpacing,
    VerticalText,
    PublisherFonts,
    Count
  };
  static constexpr uint8_t kFieldCount = static_cast<uint8_t>(Field::Count);

  // The numbering a stored value is in. Font sizes and line-spacing steps have been renumbered before; every file
  // that stores them stamps the numbering it used, and setFromStored() converts from it.
  struct Numbering {
    uint8_t fontSizeOrder;
    uint8_t lineSpacingOrder;
  };
  // The numbering this firmware writes.
  static Numbering current();

  // 0 = off, 1 = on.
  int8_t embeddedStyleOverride = -1;
  // CrossPointSettings::IMAGE_RENDERING.
  int8_t imageRenderingOverride = -1;
  // CrossPointSettings::FONT_FAMILY. Never set together with sdFontFamilyOverride.
  int8_t fontFamilyOverride = -1;
  // An SD-card font family by name.
  std::string sdFontFamilyOverride;
  // A point size (see CrossPointSettings::fontPointSize).
  int8_t fontSizeOverride = -1;
  // 0 = off, 1 = on.
  int8_t bionicReadingOverride = -1;
  // CrossPointSettings::PARAGRAPH_ALIGNMENT.
  int8_t paragraphAlignmentOverride = -1;
  // 0 = off, 1 = on.
  int8_t textAntiAliasingOverride = -1;
  // 0 = off, 1 = on.
  int8_t hyphenationOverride = -1;
  // 0 = off, 1 = on.
  int8_t fontSizeNormalizationOverride = -1;
  // 0 = off, 1 = on.
  int8_t guideDotsOverride = -1;
  // 0 = off, 1 = on.
  int8_t inlineFootnotePreviewsOverride = -1;
  // A LineSpacingStep (lib/Epub/Epub/LineSpacing.h), as CrossPointSettings::lineSpacing stores it.
  int8_t lineSpacingOverride = -1;
  // CrossPointSettings::VERTICAL_TEXT.
  int8_t verticalTextOverride = -1;
  // CrossPointSettings::PUBLISHER_FONTS.
  int8_t publisherFontsOverride = -1;

  // The value of `field`, -1 while the book follows the setting.
  int8_t get(Field field) const;
  // Takes `stored`, as a file of `numbering` holds it, when it is a value `field` can have, and -1 (follow the
  // setting) when it is not: absent (pass -1), out of range, or a size or step no numbering knows. A file is never
  // trusted to hold only what this firmware would have written.
  void setFromStored(Field field, int stored, Numbering numbering);
  // Takes an SD family name as a file holds it. A family by name and a built-in family are never both set: the name
  // wins, as the reader resolves it.
  void setSdFontFamilyFromStored(std::string name);

  // True when the book has a setting of its own.
  bool any() const;
  bool operator==(const BookReaderOverrides& other) const = default;
};

// A book's own settings on the card: overrides.bin in its cache folder (/.crosspoint/epub_<hash>/), where they stay
// for as long as the book does. Layout in docs/file-formats.md. The folder moves with the book (BookRelocation), and
// a cache clear keeps the file as reading state (BookCache::isReadingState).
namespace BookReaderOverridesFile {

inline constexpr char kName[] = "overrides.bin";

// Reads the book's settings into `out`. False, leaving `out` as it was, when there is no file or it is not one this
// firmware can read; a value in it that a field cannot have follows the setting (BookReaderOverrides::setFromStored).
bool load(const std::string& cacheDir, BookReaderOverrides& out);

// Writes `overrides`, replacing the file whole so a cut write never leaves a torn one, or removes the file when the
// book has no setting of its own. False when that failed, logged.
bool save(const std::string& cacheDir, const BookReaderOverrides& overrides);

}  // namespace BookReaderOverridesFile
