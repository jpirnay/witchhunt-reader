// Cover for how subcategory headings and submenus interact in the settings list.
//
// Two list transformations run over the same rows and each assumes something about the other:
// prepareSubmenus() lifts every row carrying a submenu out into that submenu, leaving one row
// behind that opens it, and insertSubcategorySeparators() walks a list inserting a heading row
// wherever the subcategory changes. Run in the wrong order they disagree, and the disagreement is
// invisible from either function on its own.
//
// That is what the gesture rows hit. Twenty of them share one submenu ("Gesture actions") and
// carry four different headings between them (swipes, taps, long taps, multi-touch). Headings
// were being inserted into the Controls tab first; then all twenty rows moved into the submenu,
// leaving three of the four headings behind with nothing underneath, a fourth sitting above the
// row that opens the submenu as though it described it, and the submenu itself -- the list the
// rows are actually in -- with no headings at all.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "activities/settings/SettingInfo.h"

namespace {

// The two id groups used below. Any distinct StrIds would do; these read as what they stand for.
constexpr StrId kSubmenu = StrId::STR_MENU_GESTURE_ACTIONS;
constexpr StrId kGroupA = StrId::STR_GEST_SWIPE_GROUP;
constexpr StrId kGroupB = StrId::STR_GEST_TAP_GROUP;
constexpr StrId kGroupC = StrId::STR_GEST_LONG_TAP_GROUP;
constexpr StrId kOther = StrId::STR_MENU_DISP_REFRESH;

SettingInfo row(const StrId label, const StrId submenu, const StrId subcategory) {
  auto s = SettingInfo::Action(label, SettingAction::None);
  if (submenu != StrId::STR_NONE_OPT) s.withSubmenu(submenu);
  if (subcategory != StrId::STR_NONE_OPT) s.withSubcategory(subcategory);
  return s;
}

// What SettingsActivity::onEnter does, in the order it does it: rows in plain, submenus lifted
// out, then headings inserted into whatever each list ended up holding.
void buildLikeTheSettingsScreen(std::vector<SettingInfo>& tab, std::vector<SettingInfo::SubmenuData>& submenus) {
  SettingInfo::prepareSubmenus(tab, submenus);
  SettingInfo::insertSubcategorySeparators(tab);
}

std::vector<SettingInfo>& submenuItems(std::vector<SettingInfo::SubmenuData>& submenus, const StrId id) {
  for (auto& d : submenus) {
    if (d.id == id) return d.items;
  }
  ADD_FAILURE() << "submenu was never created";
  static std::vector<SettingInfo> empty;
  return empty;
}

// A heading with no row under it before the next heading (or the end of the list). This is the
// shape the bug left behind, and it is worth naming: a reader sees a section title introducing
// nothing.
int strandedHeadings(const std::vector<SettingInfo>& items) {
  int stranded = 0;
  for (size_t i = 0; i < items.size(); ++i) {
    if (!items[i].isSeparator) continue;
    const bool hasRowUnder = i + 1 < items.size() && !items[i + 1].isSeparator;
    if (!hasRowUnder) ++stranded;
  }
  return stranded;
}

int countSeparators(const std::vector<SettingInfo>& items) {
  int n = 0;
  for (const auto& i : items) {
    if (i.isSeparator) ++n;
  }
  return n;
}

}  // namespace

// The gesture shape: one submenu, several headings among its rows.
TEST(SettingsGrouping, SubmenuRowsCarryTheirHeadingsIntoTheSubmenu) {
  std::vector<SettingInfo> tab;
  tab.push_back(row(StrId::STR_GEST_SWIPE_LEFT, kSubmenu, kGroupA));
  tab.push_back(row(StrId::STR_GEST_SWIPE_RIGHT, kSubmenu, kGroupA));
  tab.push_back(row(StrId::STR_GEST_TAP_LEFT, kSubmenu, kGroupB));
  tab.push_back(row(StrId::STR_GEST_TAP_RIGHT, kSubmenu, kGroupB));
  tab.push_back(row(StrId::STR_GEST_LONG_TAP_LEFT, kSubmenu, kGroupC));
  std::vector<SettingInfo::SubmenuData> submenus;
  buildLikeTheSettingsScreen(tab, submenus);

  // The submenu holds every row, each group introduced once, in order.
  const auto& items = submenuItems(submenus, kSubmenu);
  ASSERT_EQ(countSeparators(items), 3) << "the submenu should introduce each of its three groups";
  EXPECT_EQ(strandedHeadings(items), 0);
  ASSERT_EQ(items.size(), 8u);  // 5 rows + 3 headings
  EXPECT_TRUE(items[0].isSeparator);
  EXPECT_EQ(items[0].nameId, kGroupA);
  EXPECT_TRUE(items[3].isSeparator);
  EXPECT_EQ(items[3].nameId, kGroupB);
  EXPECT_TRUE(items[6].isSeparator);
  EXPECT_EQ(items[6].nameId, kGroupC);
}

// ...and the tab it left keeps only the row that opens it, with none of those headings.
TEST(SettingsGrouping, TheTabKeepsNoHeadingForRowsThatMovedAway) {
  std::vector<SettingInfo> tab;
  tab.push_back(row(StrId::STR_GEST_SWIPE_LEFT, kSubmenu, kGroupA));
  tab.push_back(row(StrId::STR_GEST_TAP_LEFT, kSubmenu, kGroupB));
  tab.push_back(row(StrId::STR_GEST_LONG_TAP_LEFT, kSubmenu, kGroupC));
  std::vector<SettingInfo::SubmenuData> submenus;
  buildLikeTheSettingsScreen(tab, submenus);

  EXPECT_EQ(strandedHeadings(tab), 0) << "headings were left behind by the rows they described";
  ASSERT_EQ(tab.size(), 1u) << "only the row that opens the submenu belongs in the tab";
  EXPECT_FALSE(tab[0].isSeparator);
  EXPECT_EQ(tab[0].subcategory, StrId::STR_NONE_OPT)
      << "the row that opens the submenu took a heading that describes only some of what is behind it";
}

// The other shape, which must keep working: a submenu whose rows all sit under one heading, where
// the heading really is about the row that opens it (Refresh, Front light).
TEST(SettingsGrouping, ASubmenuWhoseRowsAgreeStillHeadsItsOwnRow) {
  std::vector<SettingInfo> tab;
  tab.push_back(row(StrId::STR_GEST_SWIPE_LEFT, kOther, kOther));
  tab.push_back(row(StrId::STR_GEST_SWIPE_RIGHT, kOther, kOther));
  std::vector<SettingInfo::SubmenuData> submenus;
  buildLikeTheSettingsScreen(tab, submenus);

  ASSERT_EQ(tab.size(), 2u) << "one heading and the row that opens the submenu";
  EXPECT_TRUE(tab[0].isSeparator);
  EXPECT_EQ(tab[0].nameId, kOther);
  EXPECT_EQ(tab[1].subcategory, kOther);
  EXPECT_EQ(strandedHeadings(tab), 0);
}

// Rows that stay in the tab are grouped there exactly as before.
TEST(SettingsGrouping, PlainRowsAreStillGroupedInTheTab) {
  std::vector<SettingInfo> tab;
  tab.push_back(row(StrId::STR_GEST_SWIPE_LEFT, StrId::STR_NONE_OPT, kGroupA));
  tab.push_back(row(StrId::STR_GEST_SWIPE_RIGHT, StrId::STR_NONE_OPT, kGroupA));
  tab.push_back(row(StrId::STR_GEST_TAP_LEFT, StrId::STR_NONE_OPT, kGroupB));
  std::vector<SettingInfo::SubmenuData> submenus;
  buildLikeTheSettingsScreen(tab, submenus);

  ASSERT_EQ(tab.size(), 5u);  // 3 rows + 2 headings
  EXPECT_EQ(countSeparators(tab), 2);
  EXPECT_EQ(strandedHeadings(tab), 0);
  EXPECT_TRUE(tab[0].isSeparator);
  EXPECT_EQ(tab[0].nameId, kGroupA);
  EXPECT_TRUE(tab[3].isSeparator);
  EXPECT_EQ(tab[3].nameId, kGroupB);
}

// A heading already in the list (the Controls tab puts one in by hand) is respected rather than
// duplicated, and it introduces the rows that follow it.
TEST(SettingsGrouping, AnExistingHeadingIsNotDuplicated) {
  std::vector<SettingInfo> tab;
  tab.push_back(SettingInfo::Separator(kGroupA));
  tab.push_back(row(StrId::STR_GEST_SWIPE_LEFT, StrId::STR_NONE_OPT, kGroupA));
  tab.push_back(row(StrId::STR_GEST_TAP_LEFT, StrId::STR_NONE_OPT, kGroupB));
  std::vector<SettingInfo::SubmenuData> submenus;
  buildLikeTheSettingsScreen(tab, submenus);

  EXPECT_EQ(countSeparators(tab), 2) << "the hand-placed heading was repeated";
  EXPECT_EQ(strandedHeadings(tab), 0);
}
