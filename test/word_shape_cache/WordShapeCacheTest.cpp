// Ours (writing modes step 2, PR 3): WordShapeCache, the build's shaped words in the lent arena's free tail.
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "BuildArena.h"
#include "HashUtils.h"
#include "ShapedStore.h"
#include "WordShapeCache.h"

namespace {

using IndicShaper::PieceKind;

constexpr size_t kCap = 52272;  // the X3's framebuffer, the lent region
constexpr size_t kS = WordShapeCache::kWordCacheBytes;

// One piece as a test writes it: its kind and bytes of the text, and its records (Glyphs) or form (Reordered).
struct PieceSpec {
  PieceKind kind;
  uint16_t begin, end;
  std::vector<GlyphRecord> records;
  std::string form;
};

// A piece as a store reads it back: everything but the width, which the layout measures after a replay.
struct ReadPiece {
  PieceKind kind;
  uint16_t begin, end, count;
  std::string bytes;  // the records' or the form's bytes
  bool operator==(const ReadPiece& o) const {
    return kind == o.kind && begin == o.begin && end == o.end && count == o.count && bytes == o.bytes;
  }
};

GlyphRecord record(uint16_t glyph, int16_t advance, int8_t dx = 0, int8_t dy = 0) {
  GlyphRecord r;
  r.glyph = glyph;
  r.advance = advance;
  r.dx = dx;
  r.dy = dy;
  return r;
}

// Token `token`'s entry for (text, key), its pieces written through the sink as IndicShaper::shapeToken writes them.
void writeToken(ShapedStore& store, size_t token, std::string_view text, uint32_t key,
                const std::vector<PieceSpec>& pieces) {
  while (store.size() <= token) ASSERT_TRUE(store.insert(store.size()));
  ASSERT_TRUE(store.open(token, text, key));
  for (const PieceSpec& p : pieces) {
    ASSERT_TRUE(store.beginPiece(p.kind, p.begin, p.end));
    for (const GlyphRecord& r : p.records) ASSERT_TRUE(store.addGlyph(r));
    if (!p.form.empty()) {
      ASSERT_TRUE(store.addBytes(p.form.data(), p.form.size()));
    }
  }
  store.commit();
}

std::vector<ReadPiece> readBack(const ShapedStore& store, size_t token, std::string_view text, uint32_t key) {
  std::vector<ReadPiece> out;
  const auto pieces = store.pieces(token, text, key);
  if (!pieces) return out;
  for (const ShapedStore::Piece& p : *pieces) {
    const GlyphRun run = store.glyphs(p);
    const std::string_view form = store.form(p);
    std::string bytes =
        run.bytes > 0 ? std::string(reinterpret_cast<const char*>(run.records), run.bytes) : std::string(form);
    out.push_back({p.kind, p.begin, p.end, p.count, bytes});
  }
  return out;
}

uint64_t hashOf(std::string_view text) { return HashUtils::fnvHash64(text.data(), text.size()); }
uint16_t lengthOf(std::string_view text) { return static_cast<uint16_t>(text.size()); }

// A word of three pieces: a glyph run (one record with an offset), a Text piece, a Reordered form.
const std::string kWord = "\xE0\xA4\xAE\xE0\xA4\xA8\xE0\xA5\x81\xE0\xA4\xB7,\xE0\xA4\x95\xE0\xA4\xBF";
const std::vector<PieceSpec> kWordPieces = {
    {PieceKind::Glyphs, 0, 12, {record(101, 160), record(202, 80, -3, 5), record(303, 0)}, ""},
    {PieceKind::Text, 12, 13, {}, ""},
    {PieceKind::Reordered, 13, 19, {}, "\xE0\xA4\xBF\xE0\xA4\x95"},
};
constexpr uint32_t kKey = 0x5EED1234u;

// Replays (text, key) into token 0 of a fresh store: its pieces when it hit, none when it missed.
std::vector<ReadPiece> replayed(WordShapeCache& cache, std::string_view text, uint32_t key, bool* hit = nullptr) {
  ShapedStore fresh;
  EXPECT_TRUE(fresh.insert(0));
  EXPECT_TRUE(fresh.open(0, text, key));
  const bool replayedOk = cache.replay(hashOf(text), lengthOf(text), key, fresh);
  if (hit) *hit = replayedOk;
  if (!replayedOk) {
    EXPECT_TRUE(fresh.openPieces().empty()) << "a miss writes nothing";
    fresh.abandon();
    return {};
  }
  fresh.commit();
  return readBack(fresh, 0, text, key);
}

TEST(WordShapeCache, AHitReplaysTheSamePieces) {
  BuildArena arena(kCap);
  ASSERT_NE(arena.alloc(10844, 1), nullptr);  // resident at the parse's start: the X3, the long Hindi chapter
  WordShapeCache cache;
  ASSERT_TRUE(cache.arm(arena, kS));
  EXPECT_EQ(kS, cache.bytes());

  ShapedStore made;
  writeToken(made, 3, kWord, kKey, kWordPieces);  // any token of the store: the entry is the text's, not the place's
  const std::vector<ReadPiece> shaped = readBack(made, 3, kWord, kKey);
  ASSERT_EQ(3u, shaped.size());
  cache.insert(hashOf(kWord), lengthOf(kWord), kKey, made, 3);
  EXPECT_EQ(1u, cache.stats().inserts);

  bool hit = false;
  EXPECT_EQ(shaped, replayed(cache, kWord, kKey, &hit));
  EXPECT_TRUE(hit);
  EXPECT_EQ(1u, cache.stats().hits);
  EXPECT_EQ(0u, cache.stats().misses);
  EXPECT_EQ(0u, cache.stats().bypassed);
  // The arena did not move: the tail is room it still counts as free.
  EXPECT_EQ(10844u, arena.used());
}

TEST(WordShapeCache, AMissWritesNothing) {
  BuildArena arena(kCap);
  WordShapeCache cache;
  ASSERT_TRUE(cache.arm(arena, kS));
  bool hit = true;
  EXPECT_TRUE(replayed(cache, kWord, kKey, &hit).empty());
  EXPECT_FALSE(hit);
  EXPECT_EQ(1u, cache.stats().misses);
}

TEST(WordShapeCache, AnAllocIntoTheTailEmptiesIt) {
  BuildArena arena(kCap);
  ASSERT_NE(arena.alloc(16384, 1), nullptr);  // the cursor at 16 KB
  WordShapeCache cache;
  ASSERT_TRUE(cache.arm(arena, kS));
  ShapedStore made;
  writeToken(made, 0, kWord, kKey, kWordPieces);
  const std::vector<ReadPiece> shaped = readBack(made, 0, kWord, kKey);
  cache.insert(hashOf(kWord), lengthOf(kWord), kKey, made, 0);
  ASSERT_EQ(shaped, replayed(cache, kWord, kKey));

  // The tail as the cache left it, the entry in it.
  const std::vector<uint8_t> tailBefore(arena.tail(kS), arena.tail(kS) + kS);

  // A 33 KB header walk: its end passes capacity - S. It writes all of its bytes, then puts the tail's back exactly as
  // they were: the slots look whole, and only the watermark says they were overwritten.
  BuildArena::Block walk = arena.reserveBlock();
  auto* ring = static_cast<uint8_t*>(arena.alloc(33 * 1024, 1));
  ASSERT_NE(ring, nullptr);
  ASSERT_GT(arena.used(), kCap - kS);
  std::memset(ring, 0xA5, 33 * 1024);
  std::memcpy(arena.tail(kS), tailBefore.data(), kS);
  ASSERT_TRUE(arena.release(walk));

  bool hit = true;
  EXPECT_TRUE(replayed(cache, kWord, kKey, &hit).empty());  // nothing read from the overwritten bytes
  EXPECT_FALSE(hit);
  EXPECT_EQ(1u, cache.stats().clears);
  EXPECT_EQ(1u, cache.stats().misses);  // re-armed at once: the cursor is back at 16 KB
  EXPECT_EQ(0u, cache.stats().bypassed);

  // And it works again.
  cache.insert(hashOf(kWord), lengthOf(kWord), kKey, made, 0);
  EXPECT_EQ(2u, cache.stats().inserts);
  EXPECT_EQ(shaped, replayed(cache, kWord, kKey, &hit));
  EXPECT_TRUE(hit);
  EXPECT_EQ(2u, cache.stats().hits);
  EXPECT_EQ(1u, cache.stats().clears);
}

TEST(WordShapeCache, ItRefillsOnlyOnceTheCursorIsBackDown) {
  BuildArena arena(kCap);
  ASSERT_NE(arena.alloc(16384, 1), nullptr);
  WordShapeCache cache;
  ASSERT_TRUE(cache.arm(arena, kS));
  ShapedStore made;
  writeToken(made, 0, kWord, kKey, kWordPieces);
  cache.insert(hashOf(kWord), lengthOf(kWord), kKey, made, 0);

  // A block that reaches the tail by one byte and stays live: every call is bypassed, and nothing is stored, until it
  // goes.
  const size_t tailStart = kCap - kS;
  const size_t reArmLine = tailStart - WordShapeCache::kRoomBelow;
  BuildArena::Block held = arena.reserveBlock();
  ASSERT_NE(arena.alloc(tailStart - arena.used() + 1, 1), nullptr);
  bool hit = true;
  EXPECT_TRUE(replayed(cache, kWord, kKey, &hit).empty());
  EXPECT_FALSE(hit);
  EXPECT_EQ(1u, cache.stats().clears);
  EXPECT_EQ(1u, cache.stats().bypassed);
  cache.insert(hashOf(kWord), lengthOf(kWord), kKey, made, 0);
  EXPECT_EQ(1u, cache.stats().inserts);  // not stored
  EXPECT_TRUE(replayed(cache, kWord, kKey).empty());
  EXPECT_EQ(2u, cache.stats().bypassed);
  EXPECT_EQ(1u, cache.stats().clears);  // emptied once

  // Released, but the cursor still a byte above capacity - S - 16 KB: not yet.
  ASSERT_TRUE(arena.release(held));
  BuildArena::Block page = arena.reserveBlock();
  ASSERT_NE(arena.alloc(reArmLine - arena.used() + 1, 1), nullptr);
  EXPECT_EQ(reArmLine + 1, arena.used());
  EXPECT_TRUE(replayed(cache, kWord, kKey).empty());
  EXPECT_EQ(3u, cache.stats().bypassed);
  ASSERT_TRUE(arena.release(page));

  // Down: re-armed, empty, and filled again.
  EXPECT_TRUE(replayed(cache, kWord, kKey, &hit).empty());
  EXPECT_FALSE(hit);
  EXPECT_EQ(1u, cache.stats().misses);
  cache.insert(hashOf(kWord), lengthOf(kWord), kKey, made, 0);
  EXPECT_FALSE(replayed(cache, kWord, kKey, &hit).empty());
  EXPECT_TRUE(hit);
}

TEST(WordShapeCache, ItDoesNotArmWithoutRoom) {
  {
    BuildArena arena(kCap);
    ASSERT_NE(arena.alloc(kCap - (kS + 16384) + 1, 1), nullptr);  // a byte short of S + 16 KB
    WordShapeCache cache;
    EXPECT_FALSE(cache.arm(arena, kS));
    EXPECT_EQ(0u, cache.bytes());
    ShapedStore made;
    writeToken(made, 0, kWord, kKey, kWordPieces);
    for (int i = 0; i < 3; ++i) {
      EXPECT_TRUE(replayed(cache, kWord, kKey).empty());
      cache.insert(hashOf(kWord), lengthOf(kWord), kKey, made, 0);
    }
    EXPECT_EQ(3u, cache.stats().bypassed);  // every lookup
    EXPECT_EQ(0u, cache.stats().inserts);   // and nothing stored
    EXPECT_EQ(0u, cache.stats().misses);
    EXPECT_EQ(0u, cache.stats().clears);
  }
  {
    BuildArena arena(kCap);
    ASSERT_NE(arena.alloc(kCap - (kS + 16384), 1), nullptr);  // exactly S + 16 KB: enough
    WordShapeCache cache;
    EXPECT_TRUE(cache.arm(arena, kS));
  }
  {
    BuildArena owned(10240);  // the owned arena of a released build: no cache
    WordShapeCache cache;
    EXPECT_FALSE(cache.arm(owned, kS));
  }
}

TEST(WordShapeCache, ADisarmedCacheNeverTouchesTheTail) {
  BuildArena arena(kCap);
  WordShapeCache cache;
  ASSERT_TRUE(cache.arm(arena, kS));
  ShapedStore made;
  writeToken(made, 0, kWord, kKey, kWordPieces);
  cache.insert(hashOf(kWord), lengthOf(kWord), kKey, made, 0);
  cache.disarm();
  EXPECT_EQ(kS, cache.bytes());  // what it held, for the build's log line

  // The region belongs to its next user now, which writes all of it.
  arena.reset();
  auto* all = static_cast<uint8_t*>(arena.alloc(kCap, 1));
  ASSERT_NE(all, nullptr);
  std::memset(all, 0x5A, kCap);
  const std::vector<uint8_t> before(all, all + kCap);
  ASSERT_TRUE(replayed(cache, kWord, kKey).empty());
  cache.insert(hashOf(kWord), lengthOf(kWord), kKey, made, 0);
  EXPECT_EQ(before, std::vector<uint8_t>(all, all + kCap));
  EXPECT_EQ(1u, cache.stats().bypassed);
  EXPECT_EQ(1u, cache.stats().inserts);
}

TEST(WordShapeCache, KeysKeepFormsApart) {
  BuildArena arena(kCap);
  WordShapeCache cache;
  ASSERT_TRUE(cache.arm(arena, kS));
  const std::vector<PieceSpec> regular = {{PieceKind::Glyphs, 0, 19, {record(1, 100), record(2, 100)}, ""}};
  const std::vector<PieceSpec> bold = {{PieceKind::Glyphs, 0, 19, {record(7, 120), record(8, 120), record(9, 0)}, ""}};
  const std::string other = "\xE0\xA4\x95\xE0\xA4\xAE\xE0\xA4\xB2";
  const std::vector<PieceSpec> otherPieces = {{PieceKind::Glyphs, 0, 9, {record(40, 90)}, ""}};
  constexpr uint32_t kBoldKey = kKey ^ 0x100u;

  // Language tags fold into the key: the same word and style in two languages are two entries, and so is a book
  // without a language.
  static constexpr uint32_t kHindi[] = {0x48494E20u, 0};
  static constexpr uint32_t kMarathi[] = {0x4D415220u, 0};
  const uint32_t hindiKey = WordShapeCache::keyFor(kKey, kHindi);
  const uint32_t marathiKey = WordShapeCache::keyFor(kKey, kMarathi);
  EXPECT_NE(hindiKey, marathiKey);
  EXPECT_NE(hindiKey, WordShapeCache::keyFor(kKey, nullptr));
  EXPECT_NE(hindiKey, WordShapeCache::keyFor(kBoldKey, kHindi));
  EXPECT_EQ(hindiKey, WordShapeCache::keyFor(kKey, kHindi));

  ShapedStore made;
  writeToken(made, 0, kWord, hindiKey, regular);
  writeToken(made, 1, kWord, WordShapeCache::keyFor(kBoldKey, kHindi), bold);
  writeToken(made, 2, other, hindiKey, otherPieces);
  writeToken(made, 3, kWord, marathiKey, bold);
  cache.insert(hashOf(kWord), lengthOf(kWord), hindiKey, made, 0);
  cache.insert(hashOf(kWord), lengthOf(kWord), WordShapeCache::keyFor(kBoldKey, kHindi), made, 1);
  cache.insert(hashOf(other), lengthOf(other), hindiKey, made, 2);
  cache.insert(hashOf(kWord), lengthOf(kWord), marathiKey, made, 3);

  // Each lookup gets its own pieces or none (a later entry of the same slot took its place), never another's.
  const auto own = [&](std::string_view text, uint32_t key, const std::vector<ReadPiece>& expected) {
    const std::vector<ReadPiece> got = replayed(cache, text, key);
    EXPECT_TRUE(got.empty() || got == expected);
    return !got.empty();
  };
  ShapedStore oracle;
  writeToken(oracle, 0, kWord, 1, regular);
  writeToken(oracle, 1, kWord, 1, bold);
  writeToken(oracle, 2, other, 1, otherPieces);
  const auto regularRead = readBack(oracle, 0, kWord, 1);
  const auto boldRead = readBack(oracle, 1, kWord, 1);
  const auto otherRead = readBack(oracle, 2, other, 1);
  int hits = 0;
  hits += own(kWord, hindiKey, regularRead);
  hits += own(kWord, WordShapeCache::keyFor(kBoldKey, kHindi), boldRead);
  hits += own(other, hindiKey, otherRead);
  hits += own(kWord, marathiKey, boldRead);
  EXPECT_GE(hits, 1);
  EXPECT_TRUE(own(kWord, marathiKey, boldRead));  // the last inserted is held

  // Two identities that share a slot: the later entry takes it, and the earlier one's lookup misses rather than replay
  // the later one's pieces. Same text with another key, then another text (by its hash) with the same key: searched
  // for among candidates, as the slot is the cache's to choose.
  const auto sharesASlot = [&](const auto& identityOf) {
    int shared = 0;
    for (uint32_t n = 1; n <= 4096; ++n) {
      BuildArena own(kCap);
      WordShapeCache fresh;
      EXPECT_TRUE(fresh.arm(own, kS));
      uint64_t hashA = 0, hashB = 0;
      uint32_t keyA = 0, keyB = 0;
      identityOf(0, &hashA, &keyA);
      identityOf(n, &hashB, &keyB);
      fresh.insert(hashA, lengthOf(kWord), keyA, made, 0);  // regular
      fresh.insert(hashB, lengthOf(kWord), keyB, made, 1);  // bold
      ShapedStore into;
      EXPECT_TRUE(into.insert(0));
      EXPECT_TRUE(into.open(0, kWord, keyA));
      if (fresh.replay(hashA, lengthOf(kWord), keyA, into)) {
        into.commit();
        EXPECT_EQ(regularRead, readBack(into, 0, kWord, keyA)) << "candidate " << n;
      } else {
        EXPECT_TRUE(into.openPieces().empty());
        into.abandon();
        ++shared;
      }
    }
    return shared;
  };
  EXPECT_GT(sharesASlot([&](uint32_t n, uint64_t* hash, uint32_t* key) {
              *hash = hashOf(kWord);
              *key = hindiKey + n;
            }),
            0);
  EXPECT_GT(sharesASlot([&](uint32_t n, uint64_t* hash, uint32_t* key) {
              *hash = hashOf(kWord) + (static_cast<uint64_t>(n) << 8);
              *key = hindiKey;
            }),
            0);

  // The identity is the hash, the length and the key: one of them off is a miss.
  ShapedStore fresh;
  ASSERT_TRUE(fresh.insert(0));
  ASSERT_TRUE(fresh.open(0, kWord, marathiKey));
  EXPECT_FALSE(cache.replay(hashOf(kWord), static_cast<uint16_t>(lengthOf(kWord) + 1), marathiKey, fresh));
  EXPECT_FALSE(cache.replay(hashOf(kWord) ^ 1u, lengthOf(kWord), marathiKey, fresh));
  EXPECT_FALSE(cache.replay(hashOf(kWord), lengthOf(kWord), marathiKey + 1, fresh));
  EXPECT_TRUE(fresh.openPieces().empty());
  fresh.abandon();
}

TEST(WordShapeCache, ATooLargeTokenIsBypassed) {
  BuildArena arena(kCap);
  WordShapeCache cache;
  ASSERT_TRUE(cache.arm(arena, kS));
  // Twenty records: 80 B of forms, past one slot.
  PieceSpec big{PieceKind::Glyphs, 0, 19, {}, ""};
  for (uint16_t g = 0; g < 20; ++g) big.records.push_back(record(g, 64));
  ShapedStore made;
  writeToken(made, 0, kWord, kKey, {big});
  EXPECT_TRUE(replayed(cache, kWord, kKey).empty());
  cache.insert(hashOf(kWord), lengthOf(kWord), kKey, made, 0);
  EXPECT_EQ(0u, cache.stats().inserts);
  EXPECT_EQ(1u, cache.stats().bypassed);
  EXPECT_TRUE(replayed(cache, kWord, kKey).empty());
  EXPECT_EQ(2u, cache.stats().misses);

  // So is a token too long for a slot's one-byte offsets, whatever its forms.
  const std::string longText(300, 'a');
  writeToken(made, 1, longText, kKey, {{PieceKind::Text, 0, 290, {}, ""}, {PieceKind::Reordered, 290, 300, {}, "zz"}});
  cache.insert(hashOf(longText), lengthOf(longText), kKey, made, 1);
  EXPECT_EQ(0u, cache.stats().inserts);
  EXPECT_EQ(2u, cache.stats().bypassed);

  // One that fits a slot to the byte is stored.
  const size_t room =
      WordShapeCache::kSlotBytes - WordShapeCache::kEntryHeaderBytes - WordShapeCache::kPieceHeaderBytes;
  PieceSpec exact{PieceKind::Reordered, 0, 19, {}, std::string(room, 'r')};
  writeToken(made, 2, kWord, kKey + 7, {exact});
  cache.insert(hashOf(kWord), lengthOf(kWord), kKey + 7, made, 2);
  EXPECT_EQ(1u, cache.stats().inserts);
  bool hit = false;
  const std::vector<ReadPiece> got = replayed(cache, kWord, kKey + 7, &hit);
  EXPECT_TRUE(hit);
  ASSERT_EQ(1u, got.size());
  EXPECT_EQ(std::string(room, 'r'), got[0].bytes);
}

}  // namespace
