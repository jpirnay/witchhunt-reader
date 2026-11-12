#include "BookReaderOverrides.h"

#include <Epub/LineSpacing.h>
#include <FileReplace.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointSettings.h"

namespace {

// -1 unless `stored` lies in [-1, maxValue].
int8_t inRange(const int stored, const int maxValue) {
  return stored >= -1 && stored <= maxValue ? static_cast<int8_t>(stored) : -1;
}

constexpr int kOnOff = 1;

// overrides.bin: the magic, the format version, the numbering its sizes and steps are in, the count of values, the
// values (int8, in Field order), the SD family name's length and its bytes. A later firmware appends fields and writes
// a larger count; this one reads the ones it knows and leaves the rest. kFormatVersion changes only for a layout this
// reading cannot follow.
constexpr char kMagic[2] = {'B', 'O'};
constexpr uint8_t kFormatVersion = 1;
constexpr size_t kHeaderBytes = 6;  // magic, version, two numbering stamps, count
// Longer than any family name the settings can hold (CrossPointSettings::sdFontFamilyName).
constexpr size_t kMaxNameBytes = 63;
constexpr size_t kMaxFileBytes = kHeaderBytes + 255 + 1 + kMaxNameBytes;

std::string pathIn(const std::string& cacheDir) { return cacheDir + "/" + BookReaderOverridesFile::kName; }

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

namespace BookReaderOverridesFile {

bool load(const std::string& cacheDir, BookReaderOverrides& out) {
  std::string path = pathIn(cacheDir);
  if (!Storage.exists(path.c_str())) {
    // A replace cut between setting the old file aside and moving the new one in leaves only the old one
    // (FileReplace::replace); it is the book's settings until the next save.
    path += FileReplace::kAsideSuffix;
    if (!Storage.exists(path.c_str())) return false;
  }
  std::string bytes;
  if (!Storage.readFileToString("BKO", path, kMaxFileBytes, bytes)) return false;
  const auto byteAt = [&bytes](const size_t i) { return static_cast<uint8_t>(bytes[i]); };
  if (bytes.size() < kHeaderBytes || bytes[0] != kMagic[0] || bytes[1] != kMagic[1] || byteAt(2) != kFormatVersion) {
    LOG_ERR("BKO", "%s is not a settings file this firmware reads", path.c_str());
    return false;
  }
  const BookReaderOverrides::Numbering numbering{byteAt(3), byteAt(4)};
  const size_t count = byteAt(5);
  const size_t nameAt = kHeaderBytes + count;
  if (bytes.size() < nameAt + 1 || bytes.size() < nameAt + 1 + byteAt(nameAt)) {
    LOG_ERR("BKO", "%s is cut short", path.c_str());
    return false;
  }

  BookReaderOverrides read;
  read.setSdFontFamilyFromStored(bytes.substr(nameAt + 1, byteAt(nameAt)));
  const size_t known = std::min<size_t>(count, BookReaderOverrides::kFieldCount);
  for (size_t i = 0; i < known; ++i) {
    read.setFromStored(static_cast<BookReaderOverrides::Field>(i), static_cast<int8_t>(byteAt(kHeaderBytes + i)),
                       numbering);
  }
  out = std::move(read);
  return true;
}

bool save(const std::string& cacheDir, const BookReaderOverrides& overrides) {
  const std::string path = pathIn(cacheDir);
  if (!overrides.any()) {
    if (!Storage.exists(path.c_str()) || Storage.remove(path.c_str())) return true;
    LOG_ERR("BKO", "Could not remove %s", path.c_str());
    return false;
  }

  const BookReaderOverrides::Numbering numbering = BookReaderOverrides::current();
  const size_t nameBytes = std::min(overrides.sdFontFamilyOverride.size(), kMaxNameBytes);
  std::string bytes;
  bytes.reserve(kHeaderBytes + BookReaderOverrides::kFieldCount + 1 + nameBytes);
  bytes.append(kMagic, sizeof kMagic);
  bytes.push_back(static_cast<char>(kFormatVersion));
  bytes.push_back(static_cast<char>(numbering.fontSizeOrder));
  bytes.push_back(static_cast<char>(numbering.lineSpacingOrder));
  bytes.push_back(static_cast<char>(BookReaderOverrides::kFieldCount));
  for (uint8_t i = 0; i < BookReaderOverrides::kFieldCount; ++i) {
    bytes.push_back(static_cast<char>(overrides.get(static_cast<BookReaderOverrides::Field>(i))));
  }
  bytes.push_back(static_cast<char>(nameBytes));
  bytes.append(overrides.sdFontFamilyOverride, 0, nameBytes);

  const std::string tmp = path + ".tmp";
  bool written = false;
  {
    HalFile file;
    if (Storage.openFileForWrite("BKO", tmp, file)) {
      written = file.write(bytes.data(), bytes.size()) == bytes.size();
      written = file.close() && written;
    }
  }
  if (!written || !FileReplace::replace(tmp.c_str(), path.c_str())) {
    LOG_ERR("BKO", "Could not save %s", path.c_str());
    if (Storage.exists(tmp.c_str())) Storage.remove(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace BookReaderOverridesFile
