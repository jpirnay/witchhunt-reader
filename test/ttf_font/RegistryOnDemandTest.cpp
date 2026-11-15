// The SD font list is read from the card when first asked for, not at boot, and a book reads only the families it
// draws, one folder each (SdCardFontRegistry::findOnCard): the list cost the X3 ~12 KB of heap and 0.75 s of its boot
// for 23 families (2026-11-15). A family read alone must be the record the list holds, or a book would draw what no
// list offers.
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "SdCardFontRegistry.h"
#include "TtfBuilder.h"
#include "TtfTestUtil.h"

namespace {

namespace fs = std::filesystem;

// Three fonts roots of their own, the primary first, as the card's three are read.
struct Card {
  fs::path dir;
  std::string rootPaths[3];
  const char* roots[3] = {};
  Card() {
    dir = fs::temp_directory_path() / ("registry_on_demand_" + std::to_string(static_cast<long>(getpid())) + "_" +
                                       testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(dir);
    for (int r = 0; r < 3; ++r) {
      rootPaths[r] = (dir / ("root" + std::to_string(r))).string();
      fs::create_directories(rootPaths[r]);
      roots[r] = rootPaths[r].c_str();
    }
  }
  ~Card() { fs::remove_all(dir); }
  void add(const int root, const std::string& family, const std::string& file,
           const std::vector<uint8_t>& bytes = {1, 2, 3}) const {
    const fs::path folder = fs::path(rootPaths[root]) / family;
    fs::create_directories(folder);
    if (file.empty()) return;
    std::ofstream out(folder / file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
};

std::vector<uint8_t> outlineFont() {
  ttf_test::Font font;
  font.os2 = true;
  font.os2Selection = 0x40;  // REGULAR
  font.glyphs.resize(1);
  font.glyphs[0].contours = {ttf_test::rect(0, 0, 64, 64)};
  return ttf_test::buildFont(font);
}

// A card with what the list reads and what it leaves out.
void fill(const Card& card) {
  card.add(0, "Bitter", "Bitter_12.cpfont");
  card.add(0, "Bitter", "Bitter_14.cpfont", {1, 2, 3, 4, 5});
  card.add(0, "Literata", "Literata-Regular.ttf", outlineFont());
  card.add(0, "Literata", "Literata-Bold.ttf", outlineFont());
  card.add(0, "Shared", "Shared_10.cpfont");  // the primary wins over root 1's
  card.add(1, "Shared", "Shared_12.cpfont");
  card.add(0, "OnlyLater", "");  // empty in the primary: root 1's is the family
  card.add(1, "OnlyLater", "OnlyLater_10.cpfont");
  card.add(2, "Visible", "Visible_16.cpfont");
  card.add(0, "Empty", "");
  card.add(0, "NoFonts", "readme.txt");
  card.add(0, "Foo__staging", "Foo_10.cpfont");
  card.add(0, "Foo__backup", "Foo_10.cpfont");
  card.add(0, ".Hidden", "Hidden_10.cpfont");
  card.add(0, "_Mac", "Mac_10.cpfont");
  card.add(0, "ChineseUI", "ChineseUI_14.cpfont");  // a CJK UI pack: no reading font
}

void expectSameRecord(const SdCardFontFamilyInfo& listed, const SdCardFontFamilyInfo& alone) {
  EXPECT_EQ(listed.name, alone.name);
  ASSERT_EQ(listed.files.size(), alone.files.size()) << listed.name;
  for (size_t f = 0; f < listed.files.size(); ++f) {
    EXPECT_EQ(listed.files[f].path, alone.files[f].path);
    EXPECT_EQ(listed.files[f].pointSize, alone.files[f].pointSize);
    EXPECT_EQ(listed.files[f].style, alone.files[f].style);
    EXPECT_EQ(listed.files[f].fileBytes, alone.files[f].fileBytes);
  }
  EXPECT_EQ(listed.filesKey(), alone.filesKey());
}

}  // namespace

TEST(RegistryOnDemand, AFamilyReadAloneIsTheRecordTheListHolds) {
  const Card card;
  fill(card);
  const SdCardFontRegistry registry(card.roots, 3);
  std::vector<std::string> names;
  for (const auto& family : registry.getFamilies()) names.push_back(family.name);
  EXPECT_EQ((std::vector<std::string>{"Bitter", "Literata", "OnlyLater", "Shared", "Visible"}), names);

  for (const auto& listed : registry.getFamilies()) {
    SdCardFontFamilyInfo alone;
    ASSERT_TRUE(registry.findOnCard(listed.name.c_str(), alone)) << listed.name;
    expectSameRecord(listed, alone);
  }
  EXPECT_EQ(fs::path(registry.findFamily("Shared")->files.front().path).parent_path().parent_path().string(),
            card.rootPaths[0]);
  EXPECT_EQ(fs::path(registry.findFamily("OnlyLater")->files.front().path).parent_path().parent_path().string(),
            card.rootPaths[1]);
}

TEST(RegistryOnDemand, ANameTheListLeavesOutIsNotReadAlone) {
  const Card card;
  fill(card);
  const SdCardFontRegistry registry(card.roots, 3);
  const char* const leftOut[] = {"Empty", "NoFonts",   "Foo__staging", "Foo__backup", ".Hidden",
                                 "_Mac",  "ChineseUI", "bitter",       "Missing",     ""};
  for (const char* name : leftOut) {
    SdCardFontFamilyInfo family;
    family.name = "stale";
    family.files.push_back({"/stale", 14, 0, 1});
    EXPECT_FALSE(registry.findOnCard(name, family)) << name;
    EXPECT_TRUE(family.name.empty()) << name;
    EXPECT_TRUE(family.files.empty()) << name;
  }
  EXPECT_FALSE(registry.listed()) << "findOnCard() is a read of one folder, not of the list";
  for (const char* name : leftOut) EXPECT_EQ(nullptr, registry.findFamily(name)) << name;
}

TEST(RegistryOnDemand, TheListIsReadWhenFirstAskedForAndAgainAfterARelease) {
  const Card card;
  fill(card);
  SdCardFontRegistry registry(card.roots, 3);
  EXPECT_FALSE(registry.listed());
  EXPECT_EQ(0u, registry.generation());

  EXPECT_EQ(5, registry.getFamilyCount());
  EXPECT_TRUE(registry.listed());
  EXPECT_EQ(0u, registry.generation()) << "a read on first use says nothing of a change on the card";

  registry.release();
  EXPECT_FALSE(registry.listed());
  card.add(0, "Added", "Added_12.cpfont");
  EXPECT_EQ(0, registry.getFamilyIndex("Added")) << "the list read again holds what the card holds now";
  EXPECT_EQ(0u, registry.generation());

  EXPECT_TRUE(registry.discover());
  EXPECT_EQ(1u, registry.generation()) << "discover(), as an install asks for it, does";
}

TEST(RegistryOnDemand, TheFolderNamesComeWithNoFontRead) {
  const Card card;
  fill(card);
  const SdCardFontRegistry registry(card.roots, 3);
  std::vector<std::string> names;
  registry.forEachFamilyFolder(
      [](void* ctx, const char* name) {
        static_cast<std::vector<std::string>*>(ctx)->push_back(name);
        return true;
      },
      &names);
  std::sort(names.begin(), names.end());
  // Folders with no font the reader draws come too; the hidden, staging, backup and CJK UI ones do not.
  EXPECT_EQ((std::vector<std::string>{"Bitter", "Empty", "Literata", "NoFonts", "OnlyLater", "OnlyLater", "Shared",
                                      "Shared", "Visible"}),
            names);
  EXPECT_FALSE(registry.listed());

  int visited = 0;
  registry.forEachFamilyFolder(
      [](void* ctx, const char*) {
        ++*static_cast<int*>(ctx);
        return false;
      },
      &visited);
  EXPECT_EQ(1, visited) << "a visit that returns false ends the walk";
}

TEST(RegistryOnDemand, TheNamesOutlastTheList) {
  const Card card;
  fill(card);
  SdCardFontRegistry registry(card.roots, 3);
  ASSERT_EQ(5, registry.nameCount());
  EXPECT_FALSE(registry.listed()) << "read for the names alone, the list is not kept";
  const char* const first = registry.name(0);
  EXPECT_EQ(5, registry.getFamilyCount());
  EXPECT_EQ(first, registry.name(0)) << "a read of the list on first use keeps the names handed out";
  for (uint8_t i = 0; i < registry.nameCount(); ++i) {
    EXPECT_EQ(registry.getFamilies()[i].name, registry.name(i));
    EXPECT_EQ(i, registry.nameIndex(registry.name(i)));
  }
  EXPECT_STREQ("", registry.name(5));
  EXPECT_EQ(-1, registry.nameIndex("Empty"));
  EXPECT_EQ(-1, registry.nameIndex(nullptr));

  registry.release();
  EXPECT_EQ(5, registry.nameCount());
  EXPECT_STREQ("Literata", registry.name(1));
  EXPECT_EQ(3, registry.nameIndex("Shared"));
  EXPECT_FALSE(registry.listed()) << "a menu that counts and labels the options reads nothing";

  card.add(0, "Added", "Added_12.cpfont");
  EXPECT_TRUE(registry.discover());
  EXPECT_EQ(6, registry.nameCount()) << "a read of the list names what it read";
  EXPECT_STREQ("Added", registry.name(0));
}
