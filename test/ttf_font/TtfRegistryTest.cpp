// How the font registry lists a TrueType family. Directory scanning needs a card; these are the decisions it uses.
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "SdCardFontRegistry.h"
#include "TtfBuilder.h"
#include "TtfFamily.h"
#include "TtfFontTestUtil.h"
#include "TtfTestUtil.h"

namespace {

SdCardFontFamilyInfo ttfFamily() {
  SdCardFontFamilyInfo family;
  family.name = "Literata";
  family.files.push_back({"/.crosspoint/fonts/Literata/Literata-Regular.ttf", 0, 0, 450048});
  family.files.push_back({"/.crosspoint/fonts/Literata/Literata-Bold.ttf", 0, 1, 452000});
  return family;
}

}  // namespace

TEST(TtfRegistry, ATtfFamilyHasNoPerSizeFiles) {
  const SdCardFontFamilyInfo family = ttfFamily();
  EXPECT_TRUE(family.ttf());
  EXPECT_TRUE(family.availableSizes().empty());
  EXPECT_EQ(nullptr, family.pickClosestSize(14));  // previews and the .cpfont loader skip it
  const SdCardFontFileInfo* bold = family.findFile(0, 1);
  ASSERT_NE(nullptr, bold);
  EXPECT_EQ(452000u, bold->fileBytes);
  EXPECT_EQ(nullptr, family.findFile(0, 2));
}

TEST(TtfRegistry, ACpfontFamilyIsUnchanged) {
  SdCardFontFamilyInfo family;
  family.name = "Bookerly-SD";
  family.files.push_back({"/.crosspoint/fonts/Bookerly-SD/Bookerly-SD_12.cpfont", 12, 0, 1000});
  family.files.push_back({"/.crosspoint/fonts/Bookerly-SD/Bookerly-SD_16.cpfont", 16, 0, 1200});
  EXPECT_FALSE(family.ttf());
  EXPECT_EQ((std::vector<uint8_t>{12, 16}), family.availableSizes());
  ASSERT_NE(nullptr, family.pickClosestSize(15));
  EXPECT_EQ(16, family.pickClosestSize(15)->pointSize);
}

TEST(TtfRegistry, TwoBooksFamiliesOfOneNameAreToldApartByTheirFiles) {
  // A book family is named as the book declares it, so the manager knows a loaded family by its files: the same
  // name from another book's cache is another family, and is loaded rather than kept (the Stage A edge).
  const auto bookFamily = [](const char* cache) {
    SdCardFontFamilyInfo family;
    family.name = "book:Book Body";
    family.files.push_back({std::string(cache) + "/fonts/0.ttf", 0, 0, 300000});
    family.files.push_back({std::string(cache) + "/fonts/1.ttf", 0, 1, 270000});
    return family;
  };
  const SdCardFontFamilyInfo first = bookFamily("/.crosspoint/epub_1");
  EXPECT_EQ(first.filesKey(), bookFamily("/.crosspoint/epub_1").filesKey());
  EXPECT_NE(first.filesKey(), bookFamily("/.crosspoint/epub_2").filesKey());
  SdCardFontFamilyInfo renamed = bookFamily("/.crosspoint/epub_1");
  renamed.name = "book:Book Display";
  EXPECT_NE(first.filesKey(), renamed.filesKey());
  SdCardFontFamilyInfo fewer = bookFamily("/.crosspoint/epub_1");
  fewer.files.pop_back();
  EXPECT_NE(first.filesKey(), fewer.filesKey());
}

// --- Discovery of .otf files: the decisions scanDirectory makes with probeFile and assignStyles ---

namespace {

ttf::FileProbe probeOf(const std::vector<uint8_t>& bytes) {
  ttf_test::MemoryFile file{bytes};
  return ttf::probeFile(file.source());
}

}  // namespace

TEST(TtfRegistry, AFolderOfTtfAndOtfFilesIsOneFamily) {
  // Sorted by name: OpenDyslexic-Bold.otf, then a TrueType regular.
  const ttf::FileProbe files[] = {probeOf(ttf_test::readFont("OpenDyslexic/OpenDyslexic-Bold.otf")),
                                  probeOf(ttf_test::readFont("SourceSerif4Caption/SourceSerif4Caption-Regular.ttf"))};
  ASSERT_EQ(1, files[0].style);
  ASSERT_EQ(0, files[1].style);
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(files, 2, chosen));
  EXPECT_EQ(1, chosen[0]);
  EXPECT_EQ(0, chosen[1]);
  EXPECT_EQ(-1, chosen[2]);
  EXPECT_EQ(-1, chosen[3]);
}

TEST(TtfRegistry, AnOtfFileIsListedAndACff2FileIsNot) {
  EXPECT_TRUE(ttf::isOutlineFontFileName("OpenDyslexic-Regular.otf"));
  const ttf::FileProbe cff[] = {probeOf(ttf_test::readFont("OpenDyslexic/OpenDyslexic-Regular.otf"))};
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(cff, 1, chosen));
  EXPECT_EQ(0, chosen[0]);
  // Its CFF table renamed: the file has an outline table only the variable form has.
  std::vector<uint8_t> bytes = ttf_test::readFont("OpenDyslexic/OpenDyslexic-Regular.otf");
  bytes[ttf_test::findRecord(bytes, "CFF ") + 3] = '2';
  const ttf::FileProbe cff2[] = {probeOf(bytes)};
  EXPECT_EQ(-1, cff2[0].style);
  EXPECT_FALSE(ttf::assignStyles(cff2, 1, chosen));
}

// --- A family folder on the card, scanned as discovery scans it ---
//
// OpenDyslexic's Italic declares itself regular in head.macStyle and OS/2 fsSelection alike. It tied with the
// Regular for the regular slot, and sorting first by name it won: every page of the book was set in italic. The
// name now gives the bit the tables leave out. Ported from crosspoint-reader PR #4325 (Georg Kaindl).

namespace {

namespace fs = std::filesystem;

struct FamilyFolder {
  fs::path dir;
  explicit FamilyFolder(const std::string& tag) {
    dir = fs::temp_directory_path() / ("ttf_registry_" + std::to_string(static_cast<long>(getpid())) + "_" + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  ~FamilyFolder() { fs::remove_all(dir); }
  void add(const std::string& name, const std::vector<uint8_t>& bytes) const {
    std::ofstream out(dir / name, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
};

// The file that serves `style`, by name; "" when none does.
std::string servedBy(const SdCardFontFamilyInfo& family, const uint8_t style) {
  const SdCardFontFileInfo* file = family.findFile(0, style);
  return file ? fs::path(file->path).filename().string() : "";
}

}  // namespace

TEST(TtfRegistry, AnItalicThatDeclaresItselfRegularDoesNotServeTheBodyText) {
  const FamilyFolder folder("opendyslexic");
  for (const char* style : {"Regular", "Bold", "Italic", "BoldItalic"}) {
    const std::string name = std::string("OpenDyslexic-") + style + ".otf";
    folder.add(name, ttf_test::readFont("OpenDyslexic/" + name));
  }
  SdCardFontRegistry registry;
  SdCardFontFamilyInfo family;
  ASSERT_TRUE(registry.scanFamily(folder.dir.string().c_str(), family));
  EXPECT_EQ("OpenDyslexic-Regular.otf", servedBy(family, 0));
  EXPECT_EQ("OpenDyslexic-Bold.otf", servedBy(family, 1));
  EXPECT_EQ("OpenDyslexic-Italic.otf", servedBy(family, 2));
  EXPECT_EQ("OpenDyslexic-BoldItalic.otf", servedBy(family, 3));
}

TEST(TtfRegistry, AFamilyWithoutStyleBitsOrWeightsIsSortedOutByItsNames) {
  // Every file says regular at weight 400: only the names tell them apart.
  ttf_test::Font font;
  font.os2 = true;
  font.os2Selection = 0x40;  // REGULAR
  font.glyphs.resize(1);
  font.glyphs[0].contours = {ttf_test::rect(0, 0, 64, 64)};
  const std::vector<uint8_t> bytes = ttf_test::buildFont(font);
  const FamilyFolder folder("unmarked");
  for (const char* name : {"Face-Bold.ttf", "Face-BoldItalic.ttf", "Face-Italic.ttf", "Face-Regular.ttf"}) {
    folder.add(name, bytes);
  }
  SdCardFontRegistry registry;
  SdCardFontFamilyInfo family;
  ASSERT_TRUE(registry.scanFamily(folder.dir.string().c_str(), family));
  EXPECT_EQ("Face-Regular.ttf", servedBy(family, 0));
  EXPECT_EQ("Face-Bold.ttf", servedBy(family, 1));
  EXPECT_EQ("Face-Italic.ttf", servedBy(family, 2));
  EXPECT_EQ("Face-BoldItalic.ttf", servedBy(family, 3));
}
