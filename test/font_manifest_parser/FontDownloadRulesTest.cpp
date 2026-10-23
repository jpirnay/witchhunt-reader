// FontDownloadRules: what a font download is checked for, on the device and from the web page alike: the kind of file
// a manifest names, what a downloaded file of that kind starts with, room on the card, and whether a family is an
// update.
#include <FontCatalog.h>
#include <FontDownloadRules.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using FontDownloadRules::FileKind;

constexpr uint64_t MB = 1024 * 1024;

bool head(const FileKind kind, const std::string& bytes) {
  return FontDownloadRules::headOk(kind, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

std::string bytes(const char* s, const size_t n = 8) { return std::string(s, n); }

FileKind kindOfFormat(const char* value) { return FontDownloadRules::kindOfFormat(value, strlen(value)); }

}  // namespace

TEST(FontDownloadRulesTest, AFilesKindIsItsExtensionInAnyCase) {
  EXPECT_EQ(FontDownloadRules::kindOf("Alpha/Alpha_10.cpfont"), FileKind::Cpfont);
  EXPECT_EQ(FontDownloadRules::kindOf("Alpha_10.CPFONT"), FileKind::Cpfont);
  EXPECT_EQ(FontDownloadRules::kindOf("NotoSansJP/NotoSansJP-Regular.ttf"), FileKind::TrueType);
  EXPECT_EQ(FontDownloadRules::kindOf("x.TTF"), FileKind::TrueType);
  EXPECT_EQ(FontDownloadRules::kindOf("NotoSansJP/NotoSansJP-Regular.otf"), FileKind::OpenTypeCff);
  EXPECT_EQ(FontDownloadRules::kindOf("x.OTF"), FileKind::OpenTypeCff);
  EXPECT_EQ(FontDownloadRules::kindOf("x.ttf.part"), FileKind::Unknown);  // a download in progress
  EXPECT_EQ(FontDownloadRules::kindOf("x.otf.part"), FileKind::Unknown);
  EXPECT_EQ(FontDownloadRules::kindOf("otf"), FileKind::Unknown);
  EXPECT_EQ(FontDownloadRules::kindOf("ttf"), FileKind::Unknown);
  EXPECT_EQ(FontDownloadRules::kindOf("x.ttc"), FileKind::Unknown);
  EXPECT_EQ(FontDownloadRules::kindOf("x.woff"), FileKind::Unknown);
  EXPECT_EQ(FontDownloadRules::kindOf(""), FileKind::Unknown);
  EXPECT_EQ(FontDownloadRules::kindOf(nullptr), FileKind::Unknown);
}

TEST(FontDownloadRulesTest, AManifestFormatNamesAKindExactly) {
  EXPECT_EQ(kindOfFormat("cpfont"), FileKind::Cpfont);
  EXPECT_EQ(kindOfFormat("ttf"), FileKind::TrueType);
  EXPECT_EQ(kindOfFormat("otf"), FileKind::OpenTypeCff);
  EXPECT_EQ(kindOfFormat("OTF"), FileKind::Unknown);
  EXPECT_EQ(kindOfFormat("woff2"), FileKind::Unknown);
  EXPECT_EQ(kindOfFormat(""), FileKind::Unknown);
  EXPECT_EQ(FontDownloadRules::kindOfFormat("otfx", 3), FileKind::OpenTypeCff);  // the length bounds it
  EXPECT_EQ(FontDownloadRules::kindOfFormat("otf", 2), FileKind::Unknown);
  EXPECT_EQ(FontDownloadRules::kindOfFormat(nullptr, 0), FileKind::Unknown);
}

TEST(FontDownloadRulesTest, ACpfontStartsWithItsMagic) {
  EXPECT_TRUE(head(FileKind::Cpfont, std::string("CPFONT\0\0rest", 12)));
  EXPECT_FALSE(head(FileKind::Cpfont, std::string("CPFONT\0\1", 8)));
  EXPECT_FALSE(head(FileKind::Cpfont, std::string("CPFONT", 6)));  // too short
}

TEST(FontDownloadRulesTest, ATrueTypeFileStartsWithTrueTypeOutlines) {
  EXPECT_TRUE(head(FileKind::TrueType, bytes("\x00\x01\x00\x00\x00\x0B\x00\x80")));
  EXPECT_TRUE(head(FileKind::TrueType, bytes("true\x00\x0B\x00\x80")));   // Apple's
  EXPECT_FALSE(head(FileKind::TrueType, bytes("OTTO\x00\x0B\x00\x80")));  // CFF outlines named .ttf
  EXPECT_FALSE(head(FileKind::TrueType, bytes("ttcf\x00\x02\x00\x00")));  // a collection
  EXPECT_FALSE(head(FileKind::TrueType, bytes("wOFF\x00\x01\x00\x00")));
  EXPECT_FALSE(head(FileKind::TrueType, bytes("wOF2\x00\x01\x00\x00")));
  EXPECT_FALSE(head(FileKind::TrueType, bytes("CPFONT\0\0")));           // a .cpfont named .ttf
  EXPECT_FALSE(head(FileKind::TrueType, bytes("\x00\x01\x00\x00", 4)));  // too short
  EXPECT_FALSE(head(FileKind::Unknown, bytes("\x00\x01\x00\x00\x00\x0B\x00\x80")));
}

TEST(FontDownloadRulesTest, AnOpenTypeCffFileStartsWithOtto) {
  EXPECT_TRUE(head(FileKind::OpenTypeCff, bytes("OTTO\x00\x0B\x00\x80")));
  for (const char* bad : {"\x00\x01\x00\x00\x00\x0B\x00\x80", "ttcf\x00\x02\x00\x00", "wOFF\x00\x01\x00\x00",
                          "wOF2\x00\x01\x00\x00", "CPFONT\0\0"})  // TrueType outlines, a collection, WOFF, a .cpfont
    EXPECT_FALSE(head(FileKind::OpenTypeCff, bytes(bad, 8)));
  EXPECT_FALSE(head(FileKind::OpenTypeCff, bytes("OTTO", 4)));  // too short
}

TEST(FontDownloadRulesTest, EveryShippedOtfFamilyPasses) {
  int checked = 0;
  for (const auto& family : std::filesystem::directory_iterator(SD_FONTS_DIR)) {
    if (!family.is_directory()) continue;
    for (const auto& file : std::filesystem::directory_iterator(family.path())) {
      const std::string name = file.path().filename().generic_string();
      if (FontDownloadRules::kindOf(name.c_str()) != FileKind::OpenTypeCff) continue;
      std::ifstream in(file.path(), std::ios::binary);
      std::string head8(FontDownloadRules::kHeadBytes, '\0');
      in.read(head8.data(), static_cast<std::streamsize>(head8.size()));
      EXPECT_TRUE(head(FileKind::OpenTypeCff, head8)) << name;
      ++checked;
    }
  }
  EXPECT_EQ(checked, 8);
}

TEST(FontDownloadRulesTest, TheCardMustHoldTheDownloadAndAMargin) {
  EXPECT_TRUE(FontDownloadRules::fits(10 * MB + FontDownloadRules::kSpaceMarginBytes, 10 * MB));
  EXPECT_FALSE(FontDownloadRules::fits(10 * MB + FontDownloadRules::kSpaceMarginBytes - 1, 10 * MB));
  EXPECT_FALSE(FontDownloadRules::fits(0, 0));
  EXPECT_TRUE(FontDownloadRules::fits(64ull * 1024 * MB, 9 * MB));  // a 64 GB card, past 32 bits
}

TEST(FontDownloadRulesTest, AFolderHoldingNoneOfTheManifestsFilesIsNotAnUpdate) {
  EXPECT_FALSE(FontDownloadRules::isUpdate(0, 0, 1));  // someone else's family of that name
  EXPECT_FALSE(FontDownloadRules::isUpdate(1, 0, 0));
  EXPECT_TRUE(FontDownloadRules::isUpdate(1, 0, 1));  // a hosted family that gained a file
  EXPECT_TRUE(FontDownloadRules::isUpdate(0, 1, 0));  // a file of the same name, other bytes
  EXPECT_FALSE(FontDownloadRules::isUpdate(0, 0, 0));
}

// "Download all" takes the families that are not on the card and say they belong to it; an update to an installed
// family is the caller's `|| hasUpdate`, and does not bring a family into the set.
TEST(FontDownloadRulesTest, DownloadAllTakesOnlyMissingFamiliesThatJoinIt) {
  const auto feedFamilies = [](FontCatalog::Builder& builder) {
    builder.beginFamily();
    builder.keepFamily("A", 1);
    builder.beginFamily();
    builder.outline(true);
    builder.inDownloadAll(false);
    builder.keepFamily("B", 1);
    builder.beginFamily();
    builder.keepFamily("C", 1);
  };
  FontCatalog::Builder counter;
  feedFamilies(counter);
  FontCatalog catalog;
  ASSERT_TRUE(catalog.allocate(counter.size()));
  FontCatalog::Builder filler(catalog);
  feedFamilies(filler);
  catalog.setInstalled(2, true);

  EXPECT_TRUE(FontDownloadRules::downloadAllTakes(catalog, 0));
  EXPECT_FALSE(FontDownloadRules::downloadAllTakes(catalog, 1));
  EXPECT_FALSE(FontDownloadRules::downloadAllTakes(catalog, 2));

  catalog.setHasUpdate(1, true);
  EXPECT_FALSE(FontDownloadRules::downloadAllTakes(catalog, 1));
  catalog.setHasUpdate(2, true);
  EXPECT_FALSE(FontDownloadRules::downloadAllTakes(catalog, 2));
}
