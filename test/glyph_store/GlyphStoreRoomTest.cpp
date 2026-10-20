// Making room by keeping what is drawn: the use counts, kept only once the ring nears full.
#include <gtest/gtest.h>

#include <cstdint>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;

namespace {

constexpr uint32_t kFirst = 0x4E00;  // the code point of the first glyph each test stores

class GlyphStoreRoom : public StoreTest {
 protected:
  void SetUp() override { ASSERT_TRUE(installAndOpen(store, layout)); }

  // Commits one glyph alone. 60 x 60 is a 920-byte record: four fill a 4 KB segment past its header.
  Commit commitOne(const uint32_t cp) {
    EXPECT_TRUE(stage(batch, makeKey(cp, 0, 0), 60, 60));
    return store.commit(batch);
  }

  // Commits glyphs until `free` segments are free; returns how many were stored.
  uint32_t fillTo(const uint32_t free) {
    uint32_t n = 0;
    while (store.freeSegments() > free && n < 200) {
      EXPECT_EQ(Commit::Stored, commitOne(kFirst + n)) << n;
      ++n;
    }
    return n;
  }

  const EpdGlyph* glyphOf(const uint32_t cp) {
    const EpdGlyph* glyph = nullptr;
    EXPECT_EQ(Lookup::Found, store.find(makeKey(cp, 0, 0), &glyph)) << cp;
    return glyph;
  }

  const Layout layout = smallLayout();
  alignas(4) uint8_t buffer[8192] = {};
  GlyphBatch batch{buffer, sizeof buffer};
};

}  // namespace

TEST_F(GlyphStoreRoom, NoCountsAreKeptWhileTheRingHasRoom) {
  ASSERT_EQ(Commit::Stored, commitOne(kFirst));
  EXPECT_FALSE(store.countsKept());
  store.noteDrawn(glyphOf(kFirst));
  EXPECT_EQ(0u, store.useCount(makeKey(kFirst, 0, 0)));
  EXPECT_EQ(0u, store.drawsCounted());
}

TEST_F(GlyphStoreRoom, CountsAreKeptOnceTheRingNearsFull) {
  fillTo(kFreeSegmentsTarget + 3);  // 11 of 16 segments live: under 75%, and 5 free
  EXPECT_FALSE(store.countsKept());
  fillTo(kFreeSegmentsTarget + 1);
  EXPECT_TRUE(store.countsKept());
}

TEST_F(GlyphStoreRoom, DrawsAreCountedUpToThree) {
  fillTo(kFreeSegmentsTarget + 1);
  ASSERT_TRUE(store.countsKept());
  const EpdGlyph* glyph = glyphOf(kFirst);
  const uint32_t key = makeKey(kFirst, 0, 0);
  const uint8_t want[] = {0, 1, 2, 3, 3};
  uint32_t drawn = 0;
  for (const uint8_t count : want) {
    EXPECT_EQ(count, store.useCount(key)) << drawn;
    store.noteDrawn(glyph);
    ++drawn;
  }
  EXPECT_EQ(3u, store.useCount(key));
  EXPECT_EQ(5u, store.drawsCounted());
  // Another glyph is untouched.
  EXPECT_EQ(0u, store.useCount(makeKey(kFirst + 1, 0, 0)));
}

TEST_F(GlyphStoreRoom, ADrawOfSomethingElseIsIgnored) {
  fillTo(kFreeSegmentsTarget + 1);
  ASSERT_TRUE(store.countsKept());
  const uint32_t before = store.drawsCounted();
  EpdGlyph onStack{};
  store.noteDrawn(&onStack);
  store.noteDrawn(nullptr);
  store.noteDrawn(reinterpret_cast<const EpdGlyph*>(store.logBase() + store.geometry().logSize + 4));
  store.noteDrawn(reinterpret_cast<const EpdGlyph*>(store.logBase() + store.geometry().logSize));
  // Inside the log, but not at a record: a segment header, and the middle of a bitmap.
  store.noteDrawn(reinterpret_cast<const EpdGlyph*>(store.logBase() + 4));
  store.noteDrawn(reinterpret_cast<const EpdGlyph*>(glyphOf(kFirst)->dataOffset + store.logBase() + 8));
  EXPECT_EQ(before, store.drawsCounted());
  for (uint32_t i = 0; i < 8; ++i) EXPECT_EQ(0u, store.useCount(makeKey(kFirst + i, 0, 0))) << i;
}

TEST_F(GlyphStoreRoom, CountsStartAtZeroAfterAReopen) {
  fillTo(kFreeSegmentsTarget + 1);
  const uint32_t key = makeKey(kFirst, 0, 0);
  store.noteDrawn(glyphOf(kFirst));
  ASSERT_EQ(1u, store.useCount(key));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_FALSE(store.countsKept());
  EXPECT_EQ(0u, store.useCount(key));
  EXPECT_EQ(0u, store.drawsCounted());
}

TEST_F(GlyphStoreRoom, CountsAreFreedByAWipe) {
  fillTo(kFreeSegmentsTarget + 1);
  ASSERT_TRUE(store.countsKept());
  ASSERT_TRUE(store.wipe());
  EXPECT_FALSE(store.countsKept());
}
