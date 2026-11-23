// screenshot_name::build: the path a card screenshot is saved under, from what the screen on top reports.
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "ScreenshotName.h"

namespace {

std::string build(const ScreenshotInfo& info, const unsigned long stamp = 1234, const size_t size = 256) {
  char out[256];
  screenshot_name::build(info, stamp, out, size);
  return out;
}

ScreenshotInfo book(const char* title, const int chapter, const int page, const int percent) {
  ScreenshotInfo info;
  info.title = title;
  info.chapter = chapter;
  info.page = page;
  info.percent = percent;
  return info;
}

}  // namespace

TEST(ScreenshotName, AScreenWithoutABookKeepsThePlainName) {
  EXPECT_EQ(build(ScreenshotInfo{}), "/screenshots/screenshot-1234.bmp");
}

TEST(ScreenshotName, AnEpubPageNamesTheBookChapterPageAndProgress) {
  EXPECT_EQ(build(book("Moby Dick", 3, 5, 42)), "/screenshots/Moby Dick/Moby Dick_ch3_p5_42pct_1234.bmp");
}

TEST(ScreenshotName, AFormatWithoutChaptersLeavesTheChapterOut) {
  EXPECT_EQ(build(book("notes", 0, 12, 7)), "/screenshots/notes/notes_p12_7pct_1234.bmp");
}

TEST(ScreenshotName, WhatTheReaderDoesNotKnowIsLeftOut) {
  // An EPUB whose chapter is still being laid out: no page and no progress yet.
  EXPECT_EQ(build(book("Emma", 2, 0, -1)), "/screenshots/Emma/Emma_ch2_1234.bmp");
}

TEST(ScreenshotName, TheProgressIsClampedTo100) {
  EXPECT_EQ(build(book("Emma", 0, 1, 250)), "/screenshots/Emma/Emma_p1_100pct_1234.bmp");
  EXPECT_EQ(build(book("Emma", 0, 1, 0)), "/screenshots/Emma/Emma_p1_0pct_1234.bmp");
}

TEST(ScreenshotName, CharactersFatRefusesAreReplaced) {
  EXPECT_EQ(build(book("Who? Me: A/B *story*", 0, 1, 1)),
            "/screenshots/Who_ Me_ A_B _story_/Who_ Me_ A_B _story__p1_1pct_1234.bmp");
}

TEST(ScreenshotName, NonAsciiTitlesKeepTheirCharacters) {
  EXPECT_EQ(build(book("北京故事", 1, 2, 3)), "/screenshots/北京故事/北京故事_ch1_p2_3pct_1234.bmp");
}

TEST(ScreenshotName, ATitleWithNothingUsableBecomesBook) {
  EXPECT_EQ(build(book("...", 0, 1, 1)), "/screenshots/book/book_p1_1pct_1234.bmp");
}

TEST(ScreenshotName, ALongTitleIsCutOnACharacterBoundary) {
  // 30 three-byte characters: 90 bytes, cut to the 20 characters that fit 60 bytes.
  std::string title;
  for (int i = 0; i < 30; ++i) title += "字";
  std::string cut;
  for (int i = 0; i < 20; ++i) cut += "字";
  ASSERT_EQ(cut.size(), screenshot_name::kMaxTitleBytes);
  EXPECT_EQ(build(book(title.c_str(), 1, 1, 1)), "/screenshots/" + cut + "/" + cut + "_ch1_p1_1pct_1234.bmp");
}

TEST(ScreenshotName, TheLongestNameStaysUnder200Bytes) {
  const std::string title(500, 'x');
  const std::string name = build(book(title.c_str(), 99999, 99999, 100), 4294967295UL);
  EXPECT_LT(name.size(), 200u) << name;
}

TEST(ScreenshotName, NoRoomForTheBookFallsBackToThePlainName) {
  // 40 bytes hold "/screenshots/screenshot-1234.bmp" but not the book's name.
  EXPECT_EQ(build(book("Moby Dick", 3, 5, 42), 1234, 40), "/screenshots/screenshot-1234.bmp");
}

TEST(ScreenshotName, AZeroSizedBufferIsLeftAlone) {
  char out[4] = {'a', 'b', 'c', '\0'};
  screenshot_name::build(book("Emma", 1, 1, 1), 1, out, 0);
  EXPECT_STREQ(out, "abc");
}
