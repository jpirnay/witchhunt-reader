#include "BookReaderOverrides.h"

#include <Epub/LineSpacing.h>

#include "CrossPointSettings.h"

namespace {

// -1 unless `stored` lies in [-1, maxValue].
int8_t inRange(const int stored, const int maxValue) {
  return stored >= -1 && stored <= maxValue ? static_cast<int8_t>(stored) : -1;
}

constexpr int kOnOff = 1;

}  // namespace

BookReaderOverrides::Numbering BookReaderOverrides::current() {
  return Numbering{CrossPointSettings::FONT_SIZE_ORDER_VERSION, LINE_SPACING_ORDER_VERSION};
}

int8_t BookReaderOverrides::get(const Field field) const {
  switch (field) {
    case Field::EmbeddedStyle:
      return embeddedStyleOverride;
    case Field::ImageRendering:
      return imageRenderingOverride;
    case Field::FontFamily:
      return fontFamilyOverride;
    case Field::FontSize:
      return fontSizeOverride;
    case Field::BionicReading:
      return bionicReadingOverride;
    case Field::ParagraphAlignment:
      return paragraphAlignmentOverride;
    case Field::TextAntiAliasing:
      return textAntiAliasingOverride;
    case Field::Hyphenation:
      return hyphenationOverride;
    case Field::FontSizeNormalization:
      return fontSizeNormalizationOverride;
    case Field::GuideDots:
      return guideDotsOverride;
    case Field::InlineFootnotePreviews:
      return inlineFootnotePreviewsOverride;
    case Field::LineSpacing:
      return lineSpacingOverride;
    case Field::VerticalText:
      return verticalTextOverride;
    case Field::PublisherFonts:
      return publisherFontsOverride;
    case Field::Count:
      break;
  }
  return -1;
}

void BookReaderOverrides::setFromStored(const Field field, const int stored, const Numbering numbering) {
  switch (field) {
    case Field::EmbeddedStyle:
      embeddedStyleOverride = inRange(stored, kOnOff);
      return;
    case Field::ImageRendering:
      imageRenderingOverride = inRange(stored, CrossPointSettings::IMAGE_RENDERING_COUNT - 1);
      return;
    case Field::FontFamily:
      // A family by name already set wins (setSdFontFamilyFromStored), whichever a file holds first.
      fontFamilyOverride =
          sdFontFamilyOverride.empty() ? inRange(stored, CrossPointSettings::FONT_FAMILY_COUNT - 1) : int8_t{-1};
      return;
    case Field::FontSize: {
      const uint8_t points = CrossPointSettings::fontPointSizeFromStored(stored, numbering.fontSizeOrder);
      fontSizeOverride = points != 0 ? static_cast<int8_t>(points) : int8_t{-1};
      return;
    }
    case Field::BionicReading:
      bionicReadingOverride = inRange(stored, kOnOff);
      return;
    case Field::ParagraphAlignment:
      paragraphAlignmentOverride = inRange(stored, CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT - 1);
      return;
    case Field::TextAntiAliasing:
      textAntiAliasingOverride = inRange(stored, kOnOff);
      return;
    case Field::Hyphenation:
      hyphenationOverride = inRange(stored, kOnOff);
      return;
    case Field::FontSizeNormalization:
      fontSizeNormalizationOverride = inRange(stored, kOnOff);
      return;
    case Field::GuideDots:
      guideDotsOverride = inRange(stored, kOnOff);
      return;
    case Field::InlineFootnotePreviews:
      inlineFootnotePreviewsOverride = inRange(stored, kOnOff);
      return;
    case Field::LineSpacing: {
      const uint8_t step = lineSpacingStepFromStored(stored, numbering.lineSpacingOrder);
      lineSpacingOverride = step != kNoLineSpacingStep ? static_cast<int8_t>(step) : int8_t{-1};
      return;
    }
    case Field::VerticalText:
      verticalTextOverride = inRange(stored, CrossPointSettings::VERTICAL_TEXT_COUNT - 1);
      return;
    case Field::PublisherFonts:
      publisherFontsOverride = inRange(stored, CrossPointSettings::PUBLISHER_FONTS_COUNT - 1);
      return;
    case Field::Count:
      return;
  }
}

void BookReaderOverrides::setSdFontFamilyFromStored(std::string name) {
  sdFontFamilyOverride = std::move(name);
  if (!sdFontFamilyOverride.empty()) fontFamilyOverride = -1;
}

bool BookReaderOverrides::any() const {
  if (!sdFontFamilyOverride.empty()) return true;
  for (uint8_t i = 0; i < kFieldCount; ++i) {
    if (get(static_cast<Field>(i)) >= 0) return true;
  }
  return false;
}
