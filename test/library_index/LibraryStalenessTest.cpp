// When the book index is reused and when it is built again (docs/design/library-index.md, "When it
// rebuilds"): kept across boots and wakes, rebuilt only for a reason.
#include <LibraryStaleness.h>
#include <gtest/gtest.h>

namespace {

LibraryStaleness::Facts current() {
  LibraryStaleness::Facts facts;
  facts.indexValid = true;
  facts.sameHiddenRule = true;
  facts.cardChanged = false;
  return facts;
}

}  // namespace

// A boot or a wake is no reason: the index lives on the card it describes.
TEST(LibraryStaleness, AValidIndexOfAnUnchangedCardIsReused) {
  EXPECT_FALSE(LibraryStaleness::rebuildNeeded(current()));
}

TEST(LibraryStaleness, AMissingOrInvalidIndexIsBuilt) {
  auto facts = current();
  facts.indexValid = false;
  EXPECT_TRUE(LibraryStaleness::rebuildNeeded(facts));
}

TEST(LibraryStaleness, TheOtherHiddenFilesSettingListsOtherBooks) {
  auto facts = current();
  facts.sameHiddenRule = false;
  EXPECT_TRUE(LibraryStaleness::rebuildNeeded(facts));
}

// Anything the firmware changed on the card since the last finished build -- a web or Calibre upload,
// an OPDS download, a move or removal on the device, a USB Drive session -- in this boot or an
// earlier one.
TEST(LibraryStaleness, AChangeSinceTheLastBuildRebuilds) {
  auto facts = current();
  facts.cardChanged = true;
  EXPECT_TRUE(LibraryStaleness::rebuildNeeded(facts));
}

// A book or folder dated after the newest indexed book is not in the index: it was put on the card
// where the firmware did not see it, on a computer.
TEST(LibraryStaleness, AnEntryNewerThanTheNewestIndexedBookIsAnUnseenChange) {
  EXPECT_TRUE(LibraryStaleness::unseenChange(/*entryDate=*/5000, /*newestIndexed=*/4200));
}

TEST(LibraryStaleness, AnEntryNoNewerThanTheIndexIsNot) {
  EXPECT_FALSE(LibraryStaleness::unseenChange(4200, 4200));
  EXPECT_FALSE(LibraryStaleness::unseenChange(100, 4200));
}

// An index with no books has no newest date to compare with; the next build finds what is there.
TEST(LibraryStaleness, AnIndexWithoutBooksSeesNoUnseenChange) { EXPECT_FALSE(LibraryStaleness::unseenChange(5000, 0)); }
