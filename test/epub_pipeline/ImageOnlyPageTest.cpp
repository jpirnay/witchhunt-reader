// A page made only of images is centred in the viewport. The reader's offset leaves a vertical page alone (its
// elements sit in the turned frame, where an image's height runs along the page's width); the parser centres it
// instead, across its block extent (ChapterHtmlSlimParser::emitPage).
#include <gtest/gtest.h>

#include <memory>

#include "../../src/activities/reader/ImageOnlyPage.h"
#include "Epub/Page.h"

namespace {

Page imagePage(const bool vertical) {
  Page page;
  page.elements.push_back(std::unique_ptr<PageElement>(
      new PageImage(std::unique_ptr<ImageBlock>(new ImageBlock("/img.jpg", 200, 400)), 0, 0)));
  if (vertical) page.verticalBlockExtent = 460;
  return page;
}

TEST(ImageOnlyPage, AHorizontalImagePageIsCentred) {
  const Page page = imagePage(false);
  EXPECT_EQ((700 - 400) / 2, ReaderPageLayout::imageOnlyPageYOffset(page, 700));
}

TEST(ImageOnlyPage, AnImageAsTallAsTheViewportStaysPut) {
  const Page page = imagePage(false);
  EXPECT_EQ(0, ReaderPageLayout::imageOnlyPageYOffset(page, 400));
}

TEST(ImageOnlyPage, AVerticalImagePageIsNotMoved) {
  const Page page = imagePage(true);
  EXPECT_EQ(0, ReaderPageLayout::imageOnlyPageYOffset(page, 700));
}

TEST(ImageOnlyPage, TheCentringOffsetIsOneComputation) {
  Page page = imagePage(false);
  EXPECT_EQ(150, page.imageOnlyCentringOffset(700));
  EXPECT_EQ(0, page.imageOnlyCentringOffset(400));
  page.elements.push_back(std::unique_ptr<PageElement>(new PageHR(0, 0, 10)));
  EXPECT_EQ(0, page.imageOnlyCentringOffset(700));
}

TEST(ImageOnlyPage, ThePredicateNeedsOnlyImages) {
  Page page = imagePage(false);
  EXPECT_TRUE(page.isImageOnly());
  page.elements.push_back(std::unique_ptr<PageElement>(new PageHR(0, 0, 10)));
  EXPECT_FALSE(page.isImageOnly());
  EXPECT_FALSE(Page().isImageOnly());
}

}  // namespace
