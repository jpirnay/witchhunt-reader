// Lyra Tiles' layout file: what the web editor writes is the layout Home draws, and a file from a
// later editor, or a broken one, never leaves Home without one.
#include <gtest/gtest.h>

#include <string>

#include "components/themes/lyra/HomeLayoutFile.h"

namespace {

using HomeTilesLayout::Widget;
namespace dest = HomeTilesLayout::destination;

HomeLayoutFile::Parsed parse(const std::string& json) { return HomeLayoutFile::parse(json); }

}  // namespace

TEST(HomeLayoutFile, ReadsEveryWidget) {
  const auto p = parse(R"({"version":1,"rows":[
    [{"widget":"book","size":"compact"}],
    [{"widget":"also"}],
    [{"widget":"clock"},{"widget":"calendar"}],
    [{"widget":"week"},{"widget":"weather"}],
    [{"widget":"picture","path":"/home/a.jpg"},{"widget":"picture"}],
    [{"widget":"shortcut","to":"library"},{"widget":"shortcut","to":"recent"}]]})");
  ASSERT_TRUE(p.ok);
  ASSERT_EQ(p.config.rowCount, 6);
  EXPECT_EQ(p.config.rows[0].cells[0].widget, Widget::Book);
  EXPECT_EQ(p.config.rows[0].cells[0].option, 1);
  EXPECT_EQ(p.config.rows[1].cells[0].widget, Widget::AlsoReading);
  EXPECT_EQ(p.config.rows[2].cells[1].widget, Widget::Calendar);
  EXPECT_EQ(p.config.rows[3].cells[0].widget, Widget::Summary);
  EXPECT_EQ(p.config.rows[3].cells[1].widget, Widget::Weather);
  EXPECT_EQ(p.config.rows[4].cells[0].option, 0);
  EXPECT_EQ(p.config.rows[4].cells[1].option, 1);
  EXPECT_EQ(p.pictures[0], "/home/a.jpg");
  EXPECT_EQ(p.pictures[1], "/home");  // a picture without a path takes the folder
  EXPECT_EQ(p.config.rows[5].cells[0].option, dest::Library);
  EXPECT_EQ(p.config.rows[5].cells[1].option, dest::Recent);
}

TEST(HomeLayoutFile, OnePictureNamedTwiceIsOnePicture) {
  const auto p = parse(
      R"({"version":1,"rows":[[{"widget":"picture","path":"/x"},{"widget":"picture","path":"/x"}],
                              [{"widget":"picture","path":"/y"}],[{"widget":"picture","path":"/z"}]]})");
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.config.rows[0].cells[0].option, p.config.rows[0].cells[1].option);
  EXPECT_EQ(p.pictures[1], "/y");
  // A third picture is past what a layout keeps: its row is left out.
  EXPECT_EQ(p.config.rowCount, 2);
}

TEST(HomeLayoutFile, WhatItDoesNotKnowItLeavesOut) {
  const auto p = parse(R"({"version":1,"rows":[
    [{"widget":"book"}],
    [{"widget":"stock-ticker"},{"widget":"clock"}],
    [{"widget":"shortcut","to":"casino"}],
    [{"widget":"clock"},{"widget":"weather"},{"widget":"calendar"}]]})");
  ASSERT_TRUE(p.ok);
  ASSERT_EQ(p.config.rowCount, 3);
  EXPECT_EQ(p.config.rows[1].count, 1);  // the unknown widget's partner takes the row
  EXPECT_EQ(p.config.rows[1].cells[0].widget, Widget::Clock);
  EXPECT_EQ(p.config.rows[2].count, 2);  // a third widget in a row is dropped
}

TEST(HomeLayoutFile, ABookCannotShareARow) {
  const auto p = parse(R"({"version":1,"rows":[[{"widget":"book"},{"widget":"clock"}]]})");
  ASSERT_TRUE(p.ok);
  ASSERT_EQ(p.config.rowCount, 1);
  EXPECT_EQ(p.config.rows[0].count, 1);
  EXPECT_EQ(p.config.rows[0].cells[0].widget, Widget::Book);
}

// Home selects, cycles and draws one book card: a second one drew a blank card, and Down from the tiles
// went back to the first. The first of each is kept; a later one goes as an unknown widget does.
TEST(HomeLayoutFile, OneBookCardAndOneAlsoReading) {
  const auto p = parse(R"({"version":1,"rows":[
    [{"widget":"book"}],
    [{"widget":"also"}],
    [{"widget":"shortcut","to":"library"},{"widget":"shortcut","to":"stats"}],
    [{"widget":"book","size":"compact"}],
    [{"widget":"also"}],
    [{"widget":"book"},{"widget":"clock"}]]})");
  ASSERT_TRUE(p.ok);
  ASSERT_EQ(p.config.rowCount, 4);
  EXPECT_EQ(p.config.rows[0].cells[0].widget, Widget::Book);
  EXPECT_EQ(p.config.rows[0].cells[0].option, 0);  // the first card, large
  EXPECT_EQ(p.config.rows[1].cells[0].widget, Widget::AlsoReading);
  EXPECT_EQ(p.config.rows[2].count, 2);
  // The book beside the clock is a second one: the clock takes the row.
  EXPECT_EQ(p.config.rows[3].count, 1);
  EXPECT_EQ(p.config.rows[3].cells[0].widget, Widget::Clock);
}

// A book the parser leaves out for sharing a row is not the layout's book: a later one is kept.
TEST(HomeLayoutFile, ABookLeftOutBesideAnotherWidgetIsNotTheFirst) {
  const auto p = parse(R"({"version":1,"rows":[[{"widget":"clock"},{"widget":"book"}],[{"widget":"book"}]]})");
  ASSERT_TRUE(p.ok);
  ASSERT_EQ(p.config.rowCount, 2);
  EXPECT_EQ(p.config.rows[0].cells[0].widget, Widget::Clock);
  EXPECT_EQ(p.config.rows[1].cells[0].widget, Widget::Book);
}

TEST(HomeLayoutFile, AtMostSixRows) {
  std::string rows;
  for (int i = 0; i < 9; ++i) rows += std::string(i ? "," : "") + R"([{"widget":"clock"}])";
  const auto p = parse(R"({"version":1,"rows":[)" + rows + "]}");
  ASSERT_TRUE(p.ok);
  EXPECT_EQ(p.config.rowCount, HomeTilesLayout::kMaxRows);
}

TEST(HomeLayoutFile, ABrokenOrForeignFileIsNotALayout) {
  EXPECT_FALSE(parse("").ok);
  EXPECT_FALSE(parse("{not json").ok);
  EXPECT_FALSE(parse(R"({"version":2,"rows":[[{"widget":"book"}]]})").ok);
  EXPECT_FALSE(parse(R"({"version":1,"rows":[]})").ok);
  EXPECT_FALSE(parse(R"({"version":1,"rows":[[{"widget":"nothing"}]]})").ok);
  EXPECT_FALSE(parse(std::string(HomeLayoutFile::kMaxBytes + 1, ' ')).ok);
}
