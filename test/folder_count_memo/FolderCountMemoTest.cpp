// The session table of folder book counts behind Browse Files' folder cards: a fixed size, keyed
// by path, emptied by a change of stamp (the card changed, or the counting rules did).

#include <gtest/gtest.h>

#include <string>

#include "FolderCountMemo.h"

namespace {
constexpr uint32_t kStamp = 7;
}

TEST(FolderCountMemo, AFolderNeverCountedIsNotKnown) {
  FolderCountMemo memo;
  EXPECT_EQ(memo.find("/books", kStamp), -1);
}

TEST(FolderCountMemo, ACountIsFoundAgainUnderTheSameStamp) {
  FolderCountMemo memo;
  memo.store("/books/Pratchett", 41, kStamp);
  EXPECT_EQ(memo.find("/books/Pratchett", kStamp), 41);
  EXPECT_EQ(memo.find("/books/Pratchet", kStamp), -1);
  EXPECT_EQ(memo.find("/books/Pratchett/Discworld", kStamp), -1);
}

TEST(FolderCountMemo, AnEmptyFolderIsKnownToBeEmpty) {
  FolderCountMemo memo;
  memo.store("/books/empty", 0, kStamp);
  EXPECT_EQ(memo.find("/books/empty", kStamp), 0);
}

TEST(FolderCountMemo, CountingAgainReplacesTheOldNumber) {
  FolderCountMemo memo;
  memo.store("/books", 10, kStamp);
  memo.store("/books", 12, kStamp);
  EXPECT_EQ(memo.find("/books", kStamp), 12);
  EXPECT_EQ(memo.size(), 1u);
}

// A write on the card, or hidden files shown: nothing counted before may be shown after.
TEST(FolderCountMemo, ADifferentStampForgetsEverything) {
  FolderCountMemo memo;
  memo.store("/books", 10, kStamp);
  memo.store("/comics", 3, kStamp);
  EXPECT_EQ(memo.find("/books", kStamp + 1), -1);
  EXPECT_EQ(memo.size(), 0u);
  // ... and going back to the old stamp does not bring them back.
  EXPECT_EQ(memo.find("/comics", kStamp), -1);
}

TEST(FolderCountMemo, AnUnfinishedCountIsNotRecorded) {
  FolderCountMemo memo;
  memo.store("/books", -1, kStamp);
  EXPECT_EQ(memo.find("/books", kStamp), -1);
  EXPECT_EQ(memo.size(), 0u);
}

// Fixed size whatever the library: when it is full the oldest entry makes room. A walk records
// the deepest folders first, so what survives is the folder asked about and those nearest it.
TEST(FolderCountMemo, AFullTableDropsTheOldestEntry) {
  FolderCountMemo memo;
  for (size_t i = 0; i <= FolderCountMemo::CAPACITY; ++i) {
    memo.store("/books/" + std::to_string(i), static_cast<int>(i), kStamp);
  }
  EXPECT_EQ(memo.size(), FolderCountMemo::CAPACITY);
  EXPECT_EQ(memo.find("/books/0", kStamp), -1);
  EXPECT_EQ(memo.find("/books/1", kStamp), 1);
  EXPECT_EQ(memo.find("/books/" + std::to_string(FolderCountMemo::CAPACITY), kStamp),
            static_cast<int>(FolderCountMemo::CAPACITY));
}

// The count is stored in 16 bits: a larger one is clamped, never wrapped into a negative "unknown".
TEST(FolderCountMemo, ACountBeyondSixteenBitsIsClampedNotWrapped) {
  FolderCountMemo memo;
  memo.store("/library", 40000, kStamp);
  EXPECT_EQ(memo.find("/library", kStamp), 32767);
}

TEST(FolderCountMemo, TheTableStaysSmall) {
  // 64 folders' worth, whatever the size of the library.
  EXPECT_LE(sizeof(FolderCountMemo), 600u);
}
