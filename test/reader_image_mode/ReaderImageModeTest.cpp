// The reader's Images setting as its layout, gray-pass and refresh decisions read it (ReaderImageMode.h).
//
// Black & White is Display with the images left as the B/W pass draws them: the same layout, so the same section
// cache, and no gray pass for the images, so the page is a text page to the one-pass AA paths, the pre-render and
// "Refresh after image pages". The values are stored by number in settings.json and in each book's override in
// recent.json, so the old ones must not move.
#include <gtest/gtest.h>

#include "CrossPointSettings.h"
#include "ReaderImageMode.h"

// The real settings type, for its defaults (as in test/setting_info_options).
CrossPointSettings CrossPointSettings::instance;

namespace {

using S = CrossPointSettings;
constexpr uint8_t kDisplay = S::IMAGES_DISPLAY;
constexpr uint8_t kPlaceholder = S::IMAGES_PLACEHOLDER;
constexpr uint8_t kSuppress = S::IMAGES_SUPPRESS;
constexpr uint8_t kBlackWhite = S::IMAGES_DISPLAY_BW;

// A saved 0, 1 or 2 means what it meant before Black & White existed, and Black & White is the new 3. The settings
// load bounds a global value by the row's option count and a book's override by IMAGE_RENDERING_COUNT - 1, so 3 loads
// and 4 does not.
TEST(ReaderImageMode, StoredValuesAreStable) {
  static_assert(S::IMAGES_DISPLAY == 0 && S::IMAGES_PLACEHOLDER == 1 && S::IMAGES_SUPPRESS == 2);
  static_assert(S::IMAGES_DISPLAY_BW == 3);
  static_assert(S::IMAGE_RENDERING_COUNT == 4);
  EXPECT_EQ(S::getInstance().imageRendering, kDisplay) << "the default stays Display";
}

// The parser knows display, placeholder and suppress; Black & White reaches it, and keys the section cache, as Display.
// Switching between the two therefore keeps every laid-out chapter, and the parser still emits the book's images (its
// CSS background pictures are taken only under display).
TEST(ReaderImageMode, BlackAndWhiteIsLaidOutAsDisplay) {
  EXPECT_EQ(ReaderImageMode::forLayout(kBlackWhite), kDisplay);
  EXPECT_EQ(ReaderImageMode::forLayout(kDisplay), kDisplay);
  EXPECT_EQ(ReaderImageMode::forLayout(kPlaceholder), kPlaceholder);
  EXPECT_EQ(ReaderImageMode::forLayout(kSuppress), kSuppress);
}

// A page with images in Black & White asks for no image gray pass, and so no 4-level cache either.
TEST(ReaderImageMode, BlackAndWhiteImagesTakeNoGrayPass) {
  EXPECT_FALSE(ReaderImageMode::imagesInGray(kBlackWhite));
  EXPECT_FALSE(ReaderImageMode::imagesTakeGrayPass(/*pageHasImages=*/true, kBlackWhite));
  EXPECT_TRUE(ReaderImageMode::imagesTakeGrayPass(/*pageHasImages=*/true, kDisplay));
}

// Without images there is nothing to decide: a text page is a text page whatever the setting says.
TEST(ReaderImageMode, ATextPageTakesNoImageGrayPass) {
  for (uint8_t mode = 0; mode < S::IMAGE_RENDERING_COUNT; ++mode) {
    EXPECT_FALSE(ReaderImageMode::imagesTakeGrayPass(/*pageHasImages=*/false, mode)) << int{mode};
  }
}

// The three old values keep the gray pass they always had: a page that has images under them takes it. (Placeholder
// and Suppress lay out no image blocks, so in practice only Display has any.)
TEST(ReaderImageMode, OldValuesKeepTheirGrayPass) {
  for (const uint8_t mode : {kDisplay, kPlaceholder, kSuppress}) {
    EXPECT_TRUE(ReaderImageMode::imagesInGray(mode)) << int{mode};
  }
}

// "Refresh after image pages" clears the gray an image leaves. Display owes it; Black & White leaves no gray and turns
// like a text page; Suppress never owed it.
TEST(ReaderImageMode, OnlyGrayImagesOweTheRefreshAfter) {
  EXPECT_TRUE(ReaderImageMode::imagePageOwesRefresh(kDisplay));
  EXPECT_FALSE(ReaderImageMode::imagePageOwesRefresh(kBlackWhite));
  EXPECT_FALSE(ReaderImageMode::imagePageOwesRefresh(kSuppress));
}

// The page summary's images= field, which the device test counts.
TEST(ReaderImageMode, LogLabel) {
  EXPECT_STREQ(ReaderImageMode::logLabel(false, kBlackWhite), "none");
  EXPECT_STREQ(ReaderImageMode::logLabel(false, kDisplay), "none");
  EXPECT_STREQ(ReaderImageMode::logLabel(true, kDisplay), "gray");
  EXPECT_STREQ(ReaderImageMode::logLabel(true, kBlackWhite), "bw");
}

}  // namespace
