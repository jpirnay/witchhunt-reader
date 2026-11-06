// Ours (writing modes step 2, PR 3): ShapedStore, the paragraph's shaped pieces and glyph records in nothrow chunks.
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "ShapedStore.h"

namespace {

using IndicShaper::PieceKind;

// A token of `n` glyph records (glyph base + i, advance 16 * (i + 1)), one Glyphs piece over the whole text.
bool writeGlyphToken(ShapedStore& store, size_t token, std::string_view text, uint32_t key, int n, int base) {
  if (!store.open(token, text, key)) return false;
  if (!store.beginPiece(PieceKind::Glyphs, 0, static_cast<uint16_t>(text.size()))) {
    store.abandon();
    return false;
  }
  for (int i = 0; i < n; i++) {
    GlyphRecord r;
    r.glyph = static_cast<uint16_t>(base + i);
    r.advance = static_cast<int16_t>(16 * (i + 1));
    if (!store.addGlyph(r)) {
      store.abandon();
      return false;
    }
  }
  store.commit();
  return true;
}

// Whether token `token` reads back as writeGlyphToken wrote it.
bool readsBack(const ShapedStore& store, size_t token, std::string_view text, uint32_t key, int n, int base) {
  auto pieces = store.pieces(token, text, key);
  if (!pieces || pieces->size() != 1) return false;
  const GlyphRun run = store.glyphs((*pieces)[0]);
  if (run.count != n) return false;
  int i = 0;
  bool ok = true;
  forEachGlyph(run, 0, false, [&](const GlyphRecord& r, int) {
    ok = ok && r.glyph == base + i && r.advance == 16 * (i + 1);
    i++;
  });
  return ok && i == n;
}

TEST(ShapedStore, APiecesRecordsReadBackAsWritten) {
  ShapedStore store;
  const std::string text = "abcdefghij";
  ASSERT_TRUE(store.insert(0));
  ASSERT_TRUE(store.open(0, text, 7));
  ASSERT_TRUE(store.beginPiece(PieceKind::Text, 0, 2));
  ASSERT_TRUE(store.beginPiece(PieceKind::Glyphs, 2, 6));
  GlyphRecord recs[5];
  for (int i = 0; i < 5; i++) {
    recs[i].glyph = static_cast<uint16_t>(100 + i);
    recs[i].advance = static_cast<int16_t>(40 + i);
  }
  recs[2].dx = -3;
  recs[2].dy = 5;
  for (const auto& r : recs) ASSERT_TRUE(store.addGlyph(r));
  ASSERT_TRUE(store.beginPiece(PieceKind::Reordered, 6, 10));
  ASSERT_TRUE(store.addBytes("xyz", 3));
  ASSERT_TRUE(store.addBytes("12", 2));
  store.commit();

  auto pieces = store.pieces(0, text, 7);
  ASSERT_TRUE(pieces.has_value());
  ASSERT_EQ(pieces->size(), 3u);
  const auto& text0 = (*pieces)[0];
  EXPECT_EQ(text0.kind, PieceKind::Text);
  EXPECT_EQ(text0.begin, 0);
  EXPECT_EQ(text0.end, 2);
  EXPECT_EQ(text0.bytes, 0);
  EXPECT_EQ(store.glyphs(text0).count, 0);
  EXPECT_TRUE(store.form(text0).empty());

  const auto& glyphs = (*pieces)[1];
  EXPECT_EQ(glyphs.kind, PieceKind::Glyphs);
  EXPECT_EQ(glyphs.count, 5);
  EXPECT_EQ(glyphs.bytes, 4 * 4 + 6);  // one record carries an offset
  std::vector<GlyphRecord> got;
  forEachGlyph(store.glyphs(glyphs), 0, false, [&](const GlyphRecord& r, int) { got.push_back(r); });
  ASSERT_EQ(got.size(), 5u);
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ(got[i].glyph, recs[i].glyph);
    EXPECT_EQ(got[i].advance, recs[i].advance);
    EXPECT_EQ(got[i].dx, recs[i].dx);
    EXPECT_EQ(got[i].dy, recs[i].dy);
  }

  const auto& reordered = (*pieces)[2];
  EXPECT_EQ(reordered.kind, PieceKind::Reordered);
  EXPECT_EQ(store.form(reordered), "xyz12");

  store.setWidth(0, 1, 33);
  EXPECT_EQ((*store.pieces(0, text, 7))[1].width, 33);
}

TEST(ShapedStore, AnEntryIsValidOnlyForItsTextAndKey) {
  ShapedStore store;
  ASSERT_TRUE(store.insert(0));
  ASSERT_TRUE(writeGlyphToken(store, 0, "abcd", 1, 3, 10));
  EXPECT_TRUE(store.pieces(0, "abcd", 1).has_value());
  EXPECT_FALSE(store.pieces(0, "abce", 1).has_value());  // same length, other text
  EXPECT_FALSE(store.pieces(0, "abcd", 2).has_value());  // other key
  EXPECT_FALSE(store.pieces(0, "abc", 1).has_value());   // other length
  EXPECT_FALSE(store.pieces(1, "abcd", 1).has_value());  // no such token
}

TEST(ShapedStore, AnEntryNeverWrittenHasNoPieces) {
  ShapedStore store;
  ASSERT_TRUE(store.insert(0));
  EXPECT_FALSE(store.pieces(0, "abcd", 1).has_value());
}

TEST(ShapedStore, APlainTokenCommitsAnEntryWithNoPieces) {
  ShapedStore store;
  ASSERT_TRUE(store.insert(0));
  ASSERT_TRUE(store.open(0, "plain", 3));
  store.commit();
  auto pieces = store.pieces(0, "plain", 3);
  ASSERT_TRUE(pieces.has_value());
  EXPECT_TRUE(pieces->empty());
}

TEST(ShapedStore, InsertAndEraseKeepTheOtherTokensEntries) {
  ShapedStore store;
  for (size_t t = 0; t < 3; t++) ASSERT_TRUE(store.insert(t));
  ASSERT_TRUE(writeGlyphToken(store, 0, "aaa", 1, 3, 10));
  ASSERT_TRUE(writeGlyphToken(store, 1, "bbb", 1, 4, 20));
  ASSERT_TRUE(writeGlyphToken(store, 2, "ccc", 1, 5, 30));

  ASSERT_TRUE(store.insert(1));  // a new token between a and b
  EXPECT_FALSE(store.pieces(1, "bbb", 1).has_value());
  EXPECT_TRUE(readsBack(store, 0, "aaa", 1, 3, 10));
  EXPECT_TRUE(readsBack(store, 2, "bbb", 1, 4, 20));
  EXPECT_TRUE(readsBack(store, 3, "ccc", 1, 5, 30));

  ASSERT_TRUE(writeGlyphToken(store, 1, "new", 1, 2, 40));
  EXPECT_TRUE(readsBack(store, 1, "new", 1, 2, 40));
  EXPECT_TRUE(readsBack(store, 2, "bbb", 1, 4, 20));

  store.erase(0);
  EXPECT_TRUE(readsBack(store, 0, "new", 1, 2, 40));
  EXPECT_TRUE(readsBack(store, 1, "bbb", 1, 4, 20));
  EXPECT_TRUE(readsBack(store, 2, "ccc", 1, 5, 30));
}

TEST(ShapedStore, AScratchEntryLeavesNothingBehind) {
  ShapedStore store;
  ASSERT_TRUE(store.reserve(4, 2 * ShapedStore::kChunkBytes));
  ASSERT_TRUE(store.insert(0));
  ASSERT_TRUE(store.insert(1));
  ASSERT_TRUE(writeGlyphToken(store, 0, "aaa", 1, 6, 10));
  ASSERT_TRUE(writeGlyphToken(store, 1, "bbb", 1, 7, 20));
  const size_t held = store.chunkBytes();

  ASSERT_TRUE(store.open(2, "scratch", 9));  // one past the last
  ASSERT_TRUE(store.beginPiece(PieceKind::Glyphs, 0, 7));
  GlyphRecord r;
  r.glyph = 77;
  for (int i = 0; i < 10; i++) ASSERT_TRUE(store.addGlyph(r));
  store.abandon();

  EXPECT_EQ(store.chunkBytes(), held);
  EXPECT_FALSE(store.pieces(2, "scratch", 9).has_value());
  EXPECT_TRUE(readsBack(store, 0, "aaa", 1, 6, 10));
  EXPECT_TRUE(readsBack(store, 1, "bbb", 1, 7, 20));

  // The scratch's room is free again: the next token is written where it began.
  ASSERT_TRUE(store.insert(2));
  ASSERT_TRUE(writeGlyphToken(store, 2, "ccc", 1, 3, 30));
  EXPECT_TRUE(readsBack(store, 2, "ccc", 1, 3, 30));
  EXPECT_EQ(store.chunkBytes(), held);
}

TEST(ShapedStore, AnAbandonedRewriteDropsTheStaleEntry) {
  ShapedStore store;
  ASSERT_TRUE(store.insert(0));
  ASSERT_TRUE(store.insert(1));
  ASSERT_TRUE(writeGlyphToken(store, 0, "aaa", 1, 3, 10));
  ASSERT_TRUE(writeGlyphToken(store, 1, "bbb", 1, 3, 20));
  ASSERT_TRUE(store.open(0, "aaa", 2));  // a new key: the old pieces are stale
  ASSERT_TRUE(store.beginPiece(PieceKind::Glyphs, 0, 3));
  store.abandon();
  EXPECT_FALSE(store.pieces(0, "aaa", 1).has_value());
  EXPECT_FALSE(store.pieces(0, "aaa", 2).has_value());
  EXPECT_TRUE(readsBack(store, 1, "bbb", 1, 3, 20));
}

TEST(ShapedStore, ARewrittenTokenReplacesItsPieces) {
  ShapedStore store;
  for (size_t t = 0; t < 3; t++) ASSERT_TRUE(store.insert(t));
  ASSERT_TRUE(writeGlyphToken(store, 0, "aaa", 1, 3, 10));
  ASSERT_TRUE(writeGlyphToken(store, 1, "bbb", 1, 4, 20));
  ASSERT_TRUE(writeGlyphToken(store, 2, "ccc", 1, 5, 30));
  ASSERT_TRUE(writeGlyphToken(store, 0, "aaa", 2, 6, 50));  // written again, now after the others
  EXPECT_FALSE(store.pieces(0, "aaa", 1).has_value());
  EXPECT_TRUE(readsBack(store, 0, "aaa", 2, 6, 50));
  EXPECT_TRUE(readsBack(store, 1, "bbb", 1, 4, 20));
  EXPECT_TRUE(readsBack(store, 2, "ccc", 1, 5, 30));
}

TEST(ShapedStore, DropFrontReclaimsTheConsumedTokens) {
  ShapedStore store;
  const int kRecords = 10;  // 40 B of records
  int serial = 0;
  std::vector<std::string> texts;  // by token, front first
  std::vector<int> bases;
  // Each round tops the paragraph up to 20 tokens, then consumes 16 of them: 4 stay for the next round.
  for (int round = 0; round < 50; round++) {
    while (texts.size() < 20) {
      texts.push_back("token" + std::to_string(serial));
      bases.push_back(serial * 20);
      serial++;
      ASSERT_TRUE(store.insert(texts.size() - 1));
      ASSERT_TRUE(writeGlyphToken(store, texts.size() - 1, texts.back(), 1, kRecords, bases.back())) << round;
    }
    store.dropFront(16);
    texts.erase(texts.begin(), texts.begin() + 16);
    bases.erase(bases.begin(), bases.begin() + 16);
    EXPECT_LE(store.chunkBytes(), 4 * ShapedStore::kChunkBytes);
    for (size_t t = 0; t < texts.size(); t++)
      ASSERT_TRUE(readsBack(store, t, texts[t], 1, kRecords, bases[t])) << round;
  }
}

TEST(ShapedStore, DropFrontKeepsTokensWrittenOutOfOrder) {
  ShapedStore store;
  for (size_t t = 0; t < 6; t++) ASSERT_TRUE(store.insert(t));
  const char* names[] = {"t0", "t1", "t2", "t3", "t4", "t5"};
  // Written 3, 5, 1, 4, 0, 2: the bytes lie in that order, the tokens in theirs.
  const size_t order[] = {3, 5, 1, 4, 0, 2};
  for (size_t t : order)
    ASSERT_TRUE(writeGlyphToken(store, t, names[t], 1, 3 + static_cast<int>(t), 10 * static_cast<int>(t)));
  store.dropFront(2);
  for (size_t t = 2; t < 6; t++)
    EXPECT_TRUE(readsBack(store, t - 2, names[t], 1, 3 + static_cast<int>(t), 10 * static_cast<int>(t))) << t;
}

TEST(ShapedStore, DropFrontCompactsAcrossChunks) {
  ShapedStore store;
  // 200 B tokens: five fill a chunk, so the pass spans several.
  const int kRecords = 50;
  std::vector<std::string> texts;
  for (size_t t = 0; t < 40; t++) {
    texts.push_back("tok" + std::to_string(t));
    ASSERT_TRUE(store.insert(t));
    ASSERT_TRUE(writeGlyphToken(store, t, texts[t], 1, kRecords, static_cast<int>(t) * 100));
  }
  const size_t before = store.chunkBytes();
  store.dropFront(30);
  for (size_t t = 0; t < 10; t++)
    EXPECT_TRUE(readsBack(store, t, texts[t + 30], 1, kRecords, static_cast<int>(t + 30) * 100)) << t;
  EXPECT_EQ(store.chunkBytes(), before);  // chunks are kept
  // The room is back: another 30 tokens fit without a new chunk.
  for (size_t t = 10; t < 40; t++) {
    ASSERT_TRUE(store.insert(t));
    ASSERT_TRUE(writeGlyphToken(store, t, "again", 1, kRecords, 7));
  }
  EXPECT_EQ(store.chunkBytes(), before);
}

TEST(ShapedStore, ResetKeepsTheChunksAndReleaseFreesThem) {
  const size_t base = heapLiveBytes();
  {
    ShapedStore store;
    ASSERT_TRUE(store.reserve(8, 3 * ShapedStore::kChunkBytes));
    EXPECT_EQ(store.chunkBytes(), 3 * ShapedStore::kChunkBytes);
    for (size_t t = 0; t < 4; t++) {
      ASSERT_TRUE(store.insert(t));
      ASSERT_TRUE(writeGlyphToken(store, t, "abc", 1, 20, 1));
    }
    const size_t live = heapLiveBytes();
    store.reset();
    EXPECT_FALSE(store.pieces(0, "abc", 1).has_value());
    EXPECT_EQ(store.chunkBytes(), 3 * ShapedStore::kChunkBytes);
    EXPECT_GE(heapLiveBytes(), base + 3 * ShapedStore::kChunkBytes);
    EXPECT_LE(heapLiveBytes(), live);

    // After a reset the chunks are written again, none added.
    ASSERT_TRUE(store.insert(0));
    ASSERT_TRUE(writeGlyphToken(store, 0, "abc", 1, 20, 1));
    EXPECT_EQ(store.chunkBytes(), 3 * ShapedStore::kChunkBytes);

    store.release();
    EXPECT_EQ(store.chunkBytes(), 0u);
    EXPECT_EQ(heapLiveBytes(), base);
  }
  EXPECT_EQ(heapLiveBytes(), base);
}

TEST(ShapedStore, APieceLargerThanAChunkGetsAChunkOfItsOwn) {
  ShapedStore store;
  ASSERT_TRUE(store.insert(0));
  ASSERT_TRUE(store.insert(1));
  ASSERT_TRUE(writeGlyphToken(store, 0, "small", 1, 3, 1));
  ASSERT_TRUE(store.open(1, "large", 1));
  ASSERT_TRUE(store.beginPiece(PieceKind::Glyphs, 0, 5));
  for (int i = 0; i < 300; i++) {
    GlyphRecord r;
    r.glyph = static_cast<uint16_t>(i);
    r.advance = 20;
    r.dx = 1;  // 6 B with its offset
    ASSERT_TRUE(store.addGlyph(r)) << i;
  }
  store.commit();
  auto pieces = store.pieces(1, "large", 1);
  ASSERT_TRUE(pieces.has_value());
  const GlyphRun run = store.glyphs((*pieces)[0]);
  EXPECT_EQ(run.count, 300);
  EXPECT_EQ(run.bytes, 1800);
  int i = 0;
  bool ok = true;
  forEachGlyph(run, 0, false, [&](const GlyphRecord& r, int) {
    ok = ok && r.glyph == i && r.dx == 1;
    i++;
  });
  EXPECT_TRUE(ok);
  EXPECT_EQ(i, 300);
  EXPECT_TRUE(readsBack(store, 0, "small", 1, 3, 1));
  EXPECT_GE(store.chunkBytes(), 1800u);

  // A small token after it does not share its chunk, and compaction keeps the big one.
  ASSERT_TRUE(store.insert(2));
  ASSERT_TRUE(writeGlyphToken(store, 2, "after", 1, 3, 5));
  store.dropFront(1);
  EXPECT_EQ((*store.pieces(0, "large", 1))[0].bytes, 1800);
  EXPECT_TRUE(readsBack(store, 1, "after", 1, 3, 5));
}

TEST(ShapedStore, ARefusedChunkFailsTheTokenNotTheStore) {
  ShapedStore store;
  ASSERT_TRUE(store.reserve(8, 0));
  ASSERT_TRUE(store.insert(0));
  ASSERT_TRUE(writeGlyphToken(store, 0, "keep", 1, 5, 9));
  ASSERT_TRUE(store.insert(1));
  ASSERT_TRUE(store.insert(2));

  {
    AllocationRefusal refuse(ShapedStore::kChunkBytes);  // the next chunk is refused
    ASSERT_TRUE(store.open(1, "big", 1));
    ASSERT_TRUE(store.beginPiece(PieceKind::Glyphs, 0, 3));
    GlyphRecord r;
    bool failed = false;
    for (int i = 0; i < 400 && !failed; i++) failed = !store.addGlyph(r);  // 1,600 B, past the first chunk
    EXPECT_TRUE(failed);
    store.abandon();
  }
  EXPECT_FALSE(store.pieces(1, "big", 1).has_value());
  EXPECT_TRUE(readsBack(store, 0, "keep", 1, 5, 9));
  ASSERT_TRUE(writeGlyphToken(store, 2, "later", 1, 5, 3));  // later small tokens fit
  EXPECT_TRUE(readsBack(store, 2, "later", 1, 5, 3));
}

TEST(ShapedStore, TheChunkCapFailsTheTokenNotTheStore) {
  ShapedStore store;
  ASSERT_TRUE(store.reserve(40, 0));
  // 250 records are 1,000 B: one to a chunk.
  size_t t = 0;
  for (; t < ShapedStore::kMaxChunks; t++) {
    ASSERT_TRUE(store.insert(t));
    ASSERT_TRUE(writeGlyphToken(store, t, "tok", 1, 250, static_cast<int>(t) * 300)) << t;
  }
  EXPECT_EQ(store.chunkBytes(), ShapedStore::kMaxChunks * ShapedStore::kChunkBytes);

  ASSERT_TRUE(store.insert(t));
  ASSERT_TRUE(store.open(t, "tok", 1));
  EXPECT_FALSE(store.beginPiece(PieceKind::Glyphs, 0, 3) && store.addGlyph(GlyphRecord{}) && [&] {
    for (int i = 0; i < 300; i++)
      if (!store.addGlyph(GlyphRecord{})) return false;
    return true;
  }());
  store.abandon();
  EXPECT_FALSE(store.pieces(t, "tok", 1).has_value());
  for (size_t u = 0; u < ShapedStore::kMaxChunks; u++)
    EXPECT_TRUE(readsBack(store, u, "tok", 1, 250, static_cast<int>(u) * 300)) << u;

  // A token with no form at all still commits, and a small one fits in the last chunk's room.
  ASSERT_TRUE(store.open(t, "plain", 1));
  store.commit();
  EXPECT_TRUE(store.pieces(t, "plain", 1).has_value());
  ASSERT_TRUE(store.insert(t + 1));
  ASSERT_TRUE(writeGlyphToken(store, t + 1, "tiny", 1, 3, 1));
  EXPECT_TRUE(readsBack(store, t + 1, "tiny", 1, 3, 1));
}

TEST(ShapedStore, ARecordOutsideItsFieldsFailsTheToken) {
  ShapedStore store;
  ASSERT_TRUE(store.insert(0));
  ASSERT_TRUE(store.open(0, "abc", 1));
  ASSERT_TRUE(store.beginPiece(PieceKind::Glyphs, 0, 3));
  GlyphRecord bad;
  bad.advance = 20000;  // past 15 bits
  EXPECT_FALSE(store.addGlyph(bad));
  store.abandon();
  EXPECT_FALSE(store.pieces(0, "abc", 1).has_value());
}

TEST(ShapedStore, OnlyOneEntryIsOpenAtATime) {
  ShapedStore store;
  ASSERT_TRUE(store.insert(0));
  ASSERT_TRUE(store.open(0, "abc", 1));
  EXPECT_FALSE(store.open(0, "abc", 1));
  EXPECT_FALSE(store.open(5, "abc", 1));
  store.abandon();
  EXPECT_FALSE(store.open(5, "abc", 1));  // more than one past the last
  EXPECT_TRUE(store.open(1, "abc", 1));
  store.abandon();
}

}  // namespace
