// The key a book's reading-font offer is declined by (src/ReadingFontScript.h, BookPackOffer): one per script, both
// styles alike, matched as a whole entry of the saved list.
#include <gtest/gtest.h>

#include <string>

#include "ReadingFontScript.h"

namespace {

TEST(ReadingFontScript, StylesOfAScriptShareTheirKey) {
  EXPECT_EQ(ReadingFontScript::of("NotoSerifDevanagari"), "Devanagari");
  EXPECT_EQ(ReadingFontScript::of("NotoSansDevanagari"), "Devanagari");
  EXPECT_EQ(ReadingFontScript::of("NotoSansArabic"), "Arabic");
  EXPECT_EQ(ReadingFontScript::of("NotoNaskhArabic"), "Arabic");
  EXPECT_EQ(ReadingFontScript::of("NotoSerifJP"), "JP");
  EXPECT_EQ(ReadingFontScript::of("NotoSansTC"), "TC");
  EXPECT_EQ(ReadingFontScript::of("Bookerly"), "Bookerly");  // no Noto stem: the name itself
}

TEST(ReadingFontScript, ListedMatchesWholeEntriesOnly) {
  const std::string list = "Devanagari,JP,Arabic";
  EXPECT_TRUE(ReadingFontScript::listed(list, "Devanagari"));
  EXPECT_TRUE(ReadingFontScript::listed(list, "JP"));
  EXPECT_TRUE(ReadingFontScript::listed(list, "Arabic"));
  EXPECT_FALSE(ReadingFontScript::listed(list, "Deva"));
  EXPECT_FALSE(ReadingFontScript::listed(list, "P"));
  EXPECT_FALSE(ReadingFontScript::listed(list, "Arabic,"));
  EXPECT_FALSE(ReadingFontScript::listed(list, "SC"));
  EXPECT_FALSE(ReadingFontScript::listed(list, ""));
  EXPECT_FALSE(ReadingFontScript::listed("", "JP"));
  EXPECT_FALSE(ReadingFontScript::listed("TJP", "JP"));
}

TEST(ReadingFontScript, AddAppendsOnce) {
  std::string list;
  ReadingFontScript::add(list, "Tamil");
  EXPECT_EQ(list, "Tamil");
  ReadingFontScript::add(list, "KR");
  ReadingFontScript::add(list, "Tamil");
  ReadingFontScript::add(list, "");
  EXPECT_EQ(list, "Tamil,KR");
  EXPECT_TRUE(ReadingFontScript::listed(list, "KR"));
}

}  // namespace
