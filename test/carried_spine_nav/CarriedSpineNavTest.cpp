// CarriedSpineNav: where a position on a spine item with no page of its own lands, and which item comes next.
#include <gtest/gtest.h>

#include "activities/reader/CarriedSpineNav.h"

using CarriedSpineNav::land;
using CarriedSpineNav::nextReadingSpine;

TEST(CarriedSpineNav, APositionOnTheHeadingFileIsTheNextFilesFirstPage) {
  // A saved page, a bookmark, a TOC entry, a link, a search hit, a sync: every target but a turn back.
  EXPECT_EQ(25, land(24, /*lastPageTarget=*/false).spine);
  EXPECT_FALSE(land(24, false).lastPage);
}

TEST(CarriedSpineNav, ATurnBackGoesOnToTheFileBefore) {
  EXPECT_EQ(23, land(24, /*lastPageTarget=*/true).spine);
  EXPECT_TRUE(land(24, true).lastPage);
}

TEST(CarriedSpineNav, BeforeTheFirstFileThereIsNothingToTurnBackTo) {
  EXPECT_EQ(1, land(0, /*lastPageTarget=*/true).spine);
  EXPECT_FALSE(land(0, true).lastPage);
}

TEST(CarriedSpineNav, TheNextChapterStepsOverAHeadingFile) {
  EXPECT_EQ(24, nextReadingSpine(23, /*nextCarried=*/false));
  EXPECT_EQ(25, nextReadingSpine(23, /*nextCarried=*/true));
}
