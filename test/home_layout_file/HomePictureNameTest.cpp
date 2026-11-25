// The names of the pictures Home makes (src/components/themes/lyra/HomePictureName.h), and which earlier
// files a new one replaces.

#include <gtest/gtest.h>

#include <string>

#include "components/themes/lyra/HomePictureName.h"

TEST(HomePictureName, NamesThePathTheSizeAndTheWidget) {
  const std::string name = homePictureName("/home/web/web-1.jpg", 0x1234, 218, 170);
  EXPECT_EQ(name.substr(0, 4), "pic_");
  EXPECT_EQ(name.substr(12), "_00001234_218x170.bmp");
  EXPECT_EQ(name.size(), 33u);
}

TEST(HomePictureName, OnePathAndWidgetDifferOnlyInTheSize) {
  const std::string a = homePictureName("/home/web/web-1.jpg", 8123, 448, 284);
  const std::string b = homePictureName("/home/web/web-1.jpg", 9001, 448, 284);
  EXPECT_NE(a, b);
  EXPECT_EQ(a.substr(0, 13), b.substr(0, 13));
}

// A picture a plugin fetches again: the new copy replaces the file the old one left.
TEST(HomePictureName, ANewCopyReplacesTheOldOnesFile) {
  const std::string made = homePictureName("/home/web/web-1.jpg", 9001, 448, 284);
  EXPECT_TRUE(replacesHomePicture(made, homePictureName("/home/web/web-1.jpg", 8123, 448, 284)));
  EXPECT_TRUE(replacesHomePicture(made, homePictureName("/home/web/web-1.jpg", 0x7fffffff, 448, 284)));
}

TEST(HomePictureName, KeepsEverythingElse) {
  const std::string made = homePictureName("/home/web/web-1.jpg", 9001, 448, 284);
  EXPECT_FALSE(replacesHomePicture(made, made));
  // Another picture, as a folder's other pictures are.
  EXPECT_FALSE(replacesHomePicture(made, homePictureName("/home/harbour.jpg", 8123, 448, 284)));
  // The same picture in a widget of another size.
  EXPECT_FALSE(replacesHomePicture(made, homePictureName("/home/web/web-1.jpg", 8123, 218, 170)));
  EXPECT_FALSE(replacesHomePicture(made, homePictureName("/home/web/web-1.jpg", 8123, 448, 285)));
  // A picture being written, and files that are not Home's.
  EXPECT_FALSE(replacesHomePicture(made, homePictureName("/home/web/web-1.jpg", 8123, 448, 284) + ".part"));
  EXPECT_FALSE(replacesHomePicture(made, "home.json"));
  EXPECT_FALSE(replacesHomePicture(made, ""));
  EXPECT_FALSE(replacesHomePicture("", made));
  EXPECT_FALSE(replacesHomePicture("not_a_picture_name_of_the_right_len", made));
}
