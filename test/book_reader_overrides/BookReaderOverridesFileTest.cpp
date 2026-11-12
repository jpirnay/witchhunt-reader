// BookReaderOverridesFile: a book's own settings in overrides.bin in its cache folder. Runs over a real folder through
// the stdio storage shim.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "BookReaderOverrides.h"
#include "CrossPointSettings.h"
#include "Epub/LineSpacing.h"

namespace fs = std::filesystem;
using Field = BookReaderOverrides::Field;

namespace {

class BookReaderOverridesFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = (fs::path(BOOK_OVERRIDES_ROOT) / info->name() / "epub_12345").generic_string();
    fs::remove_all(fs::path(dir_).parent_path());
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(fs::path(dir_).parent_path()); }

  std::string path(const std::string& suffix = "") const {
    return (fs::path(dir_) / (std::string(BookReaderOverridesFile::kName) + suffix)).generic_string();
  }
  void writeBytes(const std::string& bytes) const { std::ofstream(path(), std::ios::binary) << bytes; }

  // A file as a firmware with `count` fields would write it, every value `value`, then the family name.
  static std::string fileWith(const uint8_t count, const int8_t value, const std::string& name = "",
                              const uint8_t fontSizeOrder = CrossPointSettings::FONT_SIZE_ORDER_VERSION) {
    std::string bytes = {'B',
                         'O',
                         1,
                         static_cast<char>(fontSizeOrder),
                         static_cast<char>(LINE_SPACING_ORDER_VERSION),
                         static_cast<char>(count)};
    bytes.append(count, static_cast<char>(value));
    bytes.push_back(static_cast<char>(name.size()));
    bytes += name;
    return bytes;
  }

  static BookReaderOverrides someSettings() {
    BookReaderOverrides overrides;
    const auto numbering = BookReaderOverrides::current();
    overrides.setFromStored(Field::FontSize, CrossPointSettings::DEFAULT_FONT_POINT_SIZE, numbering);
    overrides.setFromStored(Field::LineSpacing, static_cast<int>(LineSpacingStep::Pct120), numbering);
    overrides.setFromStored(Field::Hyphenation, 0, numbering);
    overrides.setFromStored(Field::PublisherFonts, CrossPointSettings::PUBLISHER_FONTS_ALL, numbering);
    overrides.setSdFontFamilyFromStored("Literata");
    return overrides;
  }

  std::string dir_;
};

}  // namespace

TEST_F(BookReaderOverridesFileTest, SavedSettingsReadBack) {
  const BookReaderOverrides saved = someSettings();
  ASSERT_TRUE(BookReaderOverridesFile::save(dir_, saved));
  BookReaderOverrides read;
  ASSERT_TRUE(BookReaderOverridesFile::load(dir_, read));
  EXPECT_EQ(read, saved);
  // Nothing of the replace is left beside it.
  EXPECT_FALSE(fs::exists(path(".tmp")));
  EXPECT_FALSE(fs::exists(path(".old")));
}

TEST_F(BookReaderOverridesFileTest, ASecondSaveReplacesTheFirst) {
  ASSERT_TRUE(BookReaderOverridesFile::save(dir_, someSettings()));
  BookReaderOverrides other;
  other.setFromStored(Field::BionicReading, 1, BookReaderOverrides::current());
  ASSERT_TRUE(BookReaderOverridesFile::save(dir_, other));
  BookReaderOverrides read;
  ASSERT_TRUE(BookReaderOverridesFile::load(dir_, read));
  EXPECT_EQ(read, other);
  EXPECT_FALSE(fs::exists(path(".old")));
}

TEST_F(BookReaderOverridesFileTest, ABookWithNoFileHasNoSettings) {
  BookReaderOverrides read = someSettings();
  EXPECT_FALSE(BookReaderOverridesFile::load(dir_, read));
  EXPECT_EQ(read, someSettings()) << "a failed load leaves what it was given";
}

TEST_F(BookReaderOverridesFileTest, SettingsBackToTheGlobalOnesRemoveTheFile) {
  ASSERT_TRUE(BookReaderOverridesFile::save(dir_, someSettings()));
  ASSERT_TRUE(BookReaderOverridesFile::save(dir_, BookReaderOverrides{}));
  EXPECT_FALSE(fs::exists(path()));
  // And with no file there, nothing to remove is no failure.
  EXPECT_TRUE(BookReaderOverridesFile::save(dir_, BookReaderOverrides{}));
}

TEST_F(BookReaderOverridesFileTest, ACutFileIsNotRead) {
  const std::string whole = fileWith(BookReaderOverrides::kFieldCount, 1, "Literata");
  for (const size_t length : {size_t{0}, size_t{3}, size_t{6}, whole.size() - 9, whole.size() - 1}) {
    writeBytes(whole.substr(0, length));
    BookReaderOverrides read;
    EXPECT_FALSE(BookReaderOverridesFile::load(dir_, read)) << "length " << length;
  }
}

TEST_F(BookReaderOverridesFileTest, AFileOfAnotherKindIsNotRead) {
  std::string bytes = fileWith(BookReaderOverrides::kFieldCount, 1);
  bytes[0] = 'X';
  writeBytes(bytes);
  BookReaderOverrides read;
  EXPECT_FALSE(BookReaderOverridesFile::load(dir_, read));

  bytes = fileWith(BookReaderOverrides::kFieldCount, 1);
  bytes[2] = 2;  // a format version this firmware does not know
  writeBytes(bytes);
  EXPECT_FALSE(BookReaderOverridesFile::load(dir_, read));
}

TEST_F(BookReaderOverridesFileTest, AFileFromAnOlderFirmwareLeavesTheLaterFieldsFollowingTheSettings) {
  writeBytes(fileWith(3, 1));
  BookReaderOverrides read;
  ASSERT_TRUE(BookReaderOverridesFile::load(dir_, read));
  EXPECT_EQ(read.get(Field::EmbeddedStyle), 1);
  EXPECT_EQ(read.get(Field::ImageRendering), 1);
  EXPECT_EQ(read.get(Field::FontFamily), 1);
  for (uint8_t i = 3; i < BookReaderOverrides::kFieldCount; ++i) {
    EXPECT_EQ(read.get(static_cast<Field>(i)), -1) << "field " << int{i};
  }
}

TEST_F(BookReaderOverridesFileTest, AFileFromANewerFirmwareReadsTheFieldsThisOneKnows) {
  writeBytes(fileWith(BookReaderOverrides::kFieldCount + 3, 0, "Literata"));
  BookReaderOverrides read;
  ASSERT_TRUE(BookReaderOverridesFile::load(dir_, read));
  EXPECT_EQ(read.get(Field::Hyphenation), 0);
  EXPECT_EQ(read.get(Field::PublisherFonts), 0);
  EXPECT_EQ(read.sdFontFamilyOverride, "Literata");
}

TEST_F(BookReaderOverridesFileTest, SizesAreReadInTheNumberingTheFileWasWrittenIn) {
  // Font size 1 in a file stamped before point sizes is a FONT_SIZE rung, converted as recent.json converts it.
  writeBytes(fileWith(BookReaderOverrides::kFieldCount, 1, "", 0));
  BookReaderOverrides read;
  ASSERT_TRUE(BookReaderOverridesFile::load(dir_, read));
  EXPECT_EQ(read.get(Field::FontSize), static_cast<int8_t>(CrossPointSettings::fontPointSizeFromStored(1, 0)));
}

TEST_F(BookReaderOverridesFileTest, TheSetAsideCopyIsReadWhenAReplaceWasCutShort) {
  ASSERT_TRUE(BookReaderOverridesFile::save(dir_, someSettings()));
  // FileReplace moved the old file aside and stopped before the new one moved in.
  fs::rename(path(), path(".old"));
  BookReaderOverrides read;
  ASSERT_TRUE(BookReaderOverridesFile::load(dir_, read));
  EXPECT_EQ(read, someSettings());
}
