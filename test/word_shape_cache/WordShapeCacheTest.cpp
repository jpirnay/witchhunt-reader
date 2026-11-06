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

// An entry's identity given by its hash, for a text of kWord's length.
struct Identity {
  uint64_t hash;
  uint32_t key;
};

// As replayed(), for an identity given by its hash (the store's open entry is kWord's).
std::vector<ReadPiece> replayedAs(WordShapeCache& cache, const Identity& id, bool* hit = nullptr) {
  ShapedStore fresh;
  EXPECT_TRUE(fresh.insert(0));
  EXPECT_TRUE(fresh.open(0, kWord, id.key));
  const bool replayedOk = cache.replay(id.hash, lengthOf(kWord), id.key, fresh);
  if (hit) *hit = replayedOk;
  if (!replayedOk) {
    EXPECT_TRUE(fresh.openPieces().empty()) << "a miss writes nothing";
    fresh.abandon();
    return {};
  }
  fresh.commit();
  return readBack(fresh, 0, kWord, id.key);
}

// The first `count` of identityOf(0), identityOf(1), ... whose entries go to identityOf(0)'s set.
template <typename IdentityOf>
std::vector<Identity> identitiesOfOneSet(const WordShapeCache& cache, const size_t count,
                                         const IdentityOf& identityOf) {
  std::vector<Identity> out = {identityOf(0)};
  const size_t set = cache.setOf(out[0].hash, lengthOf(kWord), out[0].key);
  for (uint32_t n = 1; n < 1000000 && out.size() < count; ++n) {
    const Identity id = identityOf(n);
    if (cache.setOf(id.hash, lengthOf(kWord), id.key) == set) out.push_back(id);
  }
  return out;
}

// The bytes of `id`'s set in an armed cache over `arena`, the cache's whole tail being kWordCacheBytes.
uint8_t* setBytes(const BuildArena& arena, const WordShapeCache& cache, const Identity& id) {
  return arena.tail(kS) + cache.setOf(id.hash, lengthOf(kWord), id.key) * WordShapeCache::kSetBytes;
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

  // Each lookup gets its own pieces or none (later entries of the same set took its way), never another's.
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

  // Three identities of one set: two are held side by side, and the third takes the place of the one used least
  // recently, whose lookup then misses rather than replay another's pieces. Same text with other keys, then other
  // texts (by their hash) with the same key.
  const auto threeInOneSet = [&](const auto& identityOf) {
    BuildArena region(kCap);
    WordShapeCache shared;
    ASSERT_TRUE(shared.arm(region, kS));
    const std::vector<Identity> ids = identitiesOfOneSet(shared, 3, identityOf);
    ASSERT_EQ(3u, ids.size());
    shared.insert(ids[0].hash, lengthOf(kWord), ids[0].key, made, 0);  // regular
    shared.insert(ids[1].hash, lengthOf(kWord), ids[1].key, made, 1);  // bold
    shared.insert(ids[2].hash, lengthOf(kWord), ids[2].key, made, 2);  // other's pieces
    EXPECT_TRUE(replayedAs(shared, ids[0]).empty());
    EXPECT_EQ(boldRead, replayedAs(shared, ids[1]));
    EXPECT_EQ(otherRead, replayedAs(shared, ids[2]));
  };
  threeInOneSet([&](uint32_t n) { return Identity{hashOf(kWord), hindiKey + n}; });
  threeInOneSet([&](uint32_t n) { return Identity{hashOf(kWord) + (static_cast<uint64_t>(n) << 8), hindiKey}; });

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

// The form bytes an entry of one Reordered piece has room for in `bytes` of entry.
constexpr size_t formRoom(const size_t bytes) {
  return bytes - WordShapeCache::kEntryHeaderBytes - WordShapeCache::kPieceHeaderBytes;
}

TEST(WordShapeCache, ATooLargeTokenIsOversize) {
  BuildArena arena(kCap);
  WordShapeCache cache;
  ASSERT_TRUE(cache.arm(arena, kS));
  // Thirty records: 120 B of forms, past both ways of a set.
  PieceSpec big{PieceKind::Glyphs, 0, 19, {}, ""};
  for (uint16_t g = 0; g < 30; ++g) big.records.push_back(record(g, 64));
  ShapedStore made;
  writeToken(made, 0, kWord, kKey, {big});
  EXPECT_TRUE(replayed(cache, kWord, kKey).empty());
  cache.insert(hashOf(kWord), lengthOf(kWord), kKey, made, 0);
  EXPECT_EQ(0u, cache.stats().inserts);
  EXPECT_EQ(1u, cache.stats().oversize);
  EXPECT_EQ(0u, cache.stats().bypassed);  // the tail was usable
  EXPECT_TRUE(replayed(cache, kWord, kKey).empty());
  EXPECT_EQ(2u, cache.stats().misses);

  // So is a token too long for an entry's one-byte offsets, whatever its forms.
  const std::string longText(300, 'a');
  writeToken(made, 1, longText, kKey, {{PieceKind::Text, 0, 290, {}, ""}, {PieceKind::Reordered, 290, 300, {}, "zz"}});
  cache.insert(hashOf(longText), lengthOf(longText), kKey, made, 1);
  EXPECT_EQ(0u, cache.stats().inserts);
  EXPECT_EQ(2u, cache.stats().oversize);

  // One that fills a way to the byte, and one that fills both: stored. A byte more than both: not.
  const std::string oneWay(formRoom(WordShapeCache::kWayBytes), 'r');
  const std::string twoWays(formRoom(WordShapeCache::kDoubleEntryBytes), 's');
  writeToken(made, 2, kWord, kKey + 7, {{PieceKind::Reordered, 0, 19, {}, oneWay}});
  writeToken(made, 3, kWord, kKey + 8, {{PieceKind::Reordered, 0, 19, {}, twoWays}});
  writeToken(made, 4, kWord, kKey + 9, {{PieceKind::Reordered, 0, 19, {}, twoWays + "t"}});
  const auto storedWhole = [&](const uint32_t key, const size_t token, const std::string& form) {
    cache.insert(hashOf(kWord), lengthOf(kWord), key, made, token);
    bool hit = false;
    const std::vector<ReadPiece> got = replayed(cache, kWord, key, &hit);
    EXPECT_TRUE(hit) << key;
    ASSERT_EQ(1u, got.size());
    EXPECT_EQ(form, got[0].bytes);
  };
  storedWhole(kKey + 7, 2, oneWay);
  storedWhole(kKey + 8, 3, twoWays);
  cache.insert(hashOf(kWord), lengthOf(kWord), kKey + 9, made, 4);
  EXPECT_EQ(2u, cache.stats().inserts);
  EXPECT_EQ(3u, cache.stats().oversize);
}

TEST(WordShapeCache, TwoWaysKeepTwoWordsOfOneSet) {
  BuildArena arena(kCap);
  WordShapeCache cache;
  ASSERT_TRUE(cache.arm(arena, kS));
  const std::vector<Identity> ids =
      identitiesOfOneSet(cache, 3, [](uint32_t n) { return Identity{hashOf(kWord), kKey + n}; });
  ASSERT_EQ(3u, ids.size());
  const std::vector<PieceSpec> a = {{PieceKind::Glyphs, 0, 19, {record(1, 100)}, ""}};
  const std::vector<PieceSpec> b = {{PieceKind::Glyphs, 0, 19, {record(2, 100), record(3, 0, 1, -1)}, ""}};
  const std::vector<PieceSpec> c = {{PieceKind::Reordered, 0, 19, {}, "ccc"}};
  ShapedStore made;
  writeToken(made, 0, kWord, ids[0].key, a);
  writeToken(made, 1, kWord, ids[1].key, b);
  writeToken(made, 2, kWord, ids[2].key, c);
  const auto readA = readBack(made, 0, kWord, ids[0].key);
  const auto readB = readBack(made, 1, kWord, ids[1].key);
  const auto readC = readBack(made, 2, kWord, ids[2].key);

  // Both words of the set replay.
  cache.insert(ids[0].hash, lengthOf(kWord), ids[0].key, made, 0);
  cache.insert(ids[1].hash, lengthOf(kWord), ids[1].key, made, 1);
  EXPECT_EQ(readB, replayedAs(cache, ids[1]));
  EXPECT_EQ(readA, replayedAs(cache, ids[0]));  // now the set's most recent

  // A third takes the place of the one used least recently: b, though a went in first.
  cache.insert(ids[2].hash, lengthOf(kWord), ids[2].key, made, 2);
  EXPECT_EQ(readA, replayedAs(cache, ids[0]));
  EXPECT_EQ(readC, replayedAs(cache, ids[2]));
  EXPECT_TRUE(replayedAs(cache, ids[1]).empty());
  EXPECT_EQ(3u, cache.stats().inserts);
  EXPECT_EQ(4u, cache.stats().hits);

  // An identity stored again keeps one place: the set still holds the other word.
  cache.insert(ids[2].hash, lengthOf(kWord), ids[2].key, made, 2);
  EXPECT_EQ(readA, replayedAs(cache, ids[0]));
  EXPECT_EQ(readC, replayedAs(cache, ids[2]));

  // And the other way round: c used last, so b takes a's place.
  cache.insert(ids[1].hash, lengthOf(kWord), ids[1].key, made, 1);
  EXPECT_EQ(readB, replayedAs(cache, ids[1]));
  EXPECT_EQ(readC, replayedAs(cache, ids[2]));
  EXPECT_TRUE(replayedAs(cache, ids[0]).empty());
}

TEST(WordShapeCache, ALongEntryTakesBothWays) {
  BuildArena arena(kCap);
  WordShapeCache cache;
  ASSERT_TRUE(cache.arm(arena, kS));
  const std::vector<Identity> ids =
      identitiesOfOneSet(cache, 4, [](uint32_t n) { return Identity{hashOf(kWord), kKey + n}; });
  ASSERT_EQ(4u, ids.size());
  const Identity& longId = ids[0];

  // A one-piece entry of 100 B, whose form puts, where way 1's header would be, the header of an entry of its own:
  // one Text piece, for an identity of the same set. Read as an entry, way 1 would replay that.
  Identity fake{0, kKey ^ 0xFAu};
  for (uint32_t n = 1; n < 1000000; ++n) {
    fake.hash = hashOf(kWord) ^ (static_cast<uint64_t>(n) << 20);
    if (cache.setOf(fake.hash, lengthOf(kWord), fake.key) == cache.setOf(longId.hash, lengthOf(kWord), longId.key)) {
      break;
    }
  }
  ASSERT_EQ(cache.setOf(fake.hash, lengthOf(kWord), fake.key), cache.setOf(longId.hash, lengthOf(kWord), longId.key));
  std::string form(100 - WordShapeCache::kEntryHeaderBytes - WordShapeCache::kPieceHeaderBytes, 'f');
  // Way 1's byte i holds the entry's byte kWayBytes + i - kTailMarkerBytes; the form starts at kEntryHeaderBytes +
  // kPieceHeaderBytes.
  const auto formIndexOfWay1 = [](size_t i) {
    return WordShapeCache::kWayBytes + i - WordShapeCache::kTailMarkerBytes - WordShapeCache::kEntryHeaderBytes -
           WordShapeCache::kPieceHeaderBytes;
  };
  form[formIndexOfWay1(WordShapeCache::kPiecesAt)] = 1;
  for (int i = 0; i < 4; ++i) {
    form[formIndexOfWay1(WordShapeCache::kKeyAt + i)] = static_cast<char>(fake.key >> (8 * i));
  }
  for (int i = 0; i < 8; ++i) {
    form[formIndexOfWay1(WordShapeCache::kHashAt + i)] = static_cast<char>(fake.hash >> (8 * i));
  }
  const size_t piece = formIndexOfWay1(WordShapeCache::kEntryHeaderBytes);
  form[piece] = static_cast<char>(PieceKind::Text);
  form[piece + 1] = 0;
  form[piece + 2] = static_cast<char>(lengthOf(kWord));
  form[piece + 3] = 0;
  form[piece + 4] = 0;

  ShapedStore made;
  writeToken(made, 0, kWord, longId.key, {{PieceKind::Reordered, 0, 19, {}, form}});
  writeToken(made, 1, kWord, ids[1].key, kWordPieces);
  writeToken(made, 2, kWord, ids[2].key, {{PieceKind::Reordered, 0, 19, {}, "two"}});
  writeToken(made, 3, kWord, ids[3].key, {{PieceKind::Reordered, 0, 19, {}, "three"}});
  const auto readLong = readBack(made, 0, kWord, longId.key);

  // The set's two words, then the long entry: it takes both ways, and both go.
  cache.insert(ids[1].hash, lengthOf(kWord), ids[1].key, made, 1);
  cache.insert(ids[2].hash, lengthOf(kWord), ids[2].key, made, 2);
  cache.insert(longId.hash, lengthOf(kWord), longId.key, made, 0);
  EXPECT_EQ(3u, cache.stats().inserts);
  bool hit = false;
  EXPECT_EQ(readLong, replayedAs(cache, longId, &hit));
  EXPECT_TRUE(hit);
  EXPECT_TRUE(replayedAs(cache, ids[1]).empty());
  EXPECT_TRUE(replayedAs(cache, ids[2]).empty());
  // Its second way is no entry of its own, whatever its bytes say.
  const uint8_t* set = setBytes(arena, cache, longId);
  EXPECT_EQ(fake.key, static_cast<uint32_t>(set[WordShapeCache::kWayBytes + WordShapeCache::kKeyAt]) |
                          (static_cast<uint32_t>(set[WordShapeCache::kWayBytes + WordShapeCache::kKeyAt + 1]) << 8) |
                          (static_cast<uint32_t>(set[WordShapeCache::kWayBytes + WordShapeCache::kKeyAt + 2]) << 16) |
                          (static_cast<uint32_t>(set[WordShapeCache::kWayBytes + WordShapeCache::kKeyAt + 3]) << 24))
      << "the fake header sits where way 1's would";
  EXPECT_TRUE(replayedAs(cache, fake).empty());

  // A one-way entry into the set evicts the long entry whole: no half of it is read, and the other way is free.
  cache.insert(ids[2].hash, lengthOf(kWord), ids[2].key, made, 2);
  EXPECT_TRUE(replayedAs(cache, longId).empty());
  EXPECT_TRUE(replayedAs(cache, fake).empty());
  EXPECT_FALSE(replayedAs(cache, ids[2]).empty());
  cache.insert(ids[3].hash, lengthOf(kWord), ids[3].key, made, 3);
  EXPECT_FALSE(replayedAs(cache, ids[2]).empty());
  EXPECT_FALSE(replayedAs(cache, ids[3]).empty());
}

TEST(WordShapeCache, ACorruptEntryIsAMiss) {
  BuildArena arena(kCap);
  WordShapeCache cache;
  ASSERT_TRUE(cache.arm(arena, kS));
  ShapedStore made;
  writeToken(made, 0, kWord, kKey, kWordPieces);
  const Identity word{hashOf(kWord), kKey};
  cache.insert(word.hash, lengthOf(kWord), word.key, made, 0);
  ASSERT_FALSE(replayedAs(cache, word).empty());

  // Its piece count zeroed (the first entry of an empty set is in way 0): a miss, and the way is emptied.
  uint8_t* set = setBytes(arena, cache, word);
  ASSERT_EQ(lengthOf(kWord), set[WordShapeCache::kLengthAt]);
  set[WordShapeCache::kPiecesAt] = 0;
  bool hit = true;
  EXPECT_TRUE(replayedAs(cache, word, &hit).empty());
  EXPECT_FALSE(hit);
  EXPECT_EQ(0u, set[WordShapeCache::kLengthAt]);
  cache.insert(word.hash, lengthOf(kWord), word.key, made, 0);
  EXPECT_FALSE(replayedAs(cache, word).empty());

  // A long entry whose second way has lost its marker: the halves disagree, a miss, and both ways are emptied.
  const Identity longWord{hashOf(kWord) + 1, kKey};
  writeToken(made, 1, kWord, longWord.key, {{PieceKind::Reordered, 0, 19, {}, std::string(80, 'l')}});
  cache.insert(longWord.hash, lengthOf(kWord), longWord.key, made, 1);
  ASSERT_FALSE(replayedAs(cache, longWord).empty());
  uint8_t* longSet = setBytes(arena, cache, longWord);
  ASSERT_NE(0, longSet[WordShapeCache::kFlagsAt] & WordShapeCache::kDoubleHead);
  longSet[WordShapeCache::kWayBytes + WordShapeCache::kFlagsAt] = 0;
  EXPECT_TRUE(replayedAs(cache, longWord, &hit).empty());
  EXPECT_FALSE(hit);
  EXPECT_EQ(0u, longSet[WordShapeCache::kLengthAt]);
  EXPECT_EQ(0, longSet[WordShapeCache::kFlagsAt] & WordShapeCache::kDoubleHead);
}

}  // namespace
