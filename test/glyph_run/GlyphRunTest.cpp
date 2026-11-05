// Compact glyph records of a shaped run: the byte layout and the two pen rules.
#include <GlyphRun.h>
#include <gtest/gtest.h>

#include <vector>

namespace {

std::vector<uint8_t> encodeAll(const std::vector<GlyphRecord>& recs) {
  std::vector<uint8_t> out;
  for (const auto& r : recs) {
    uint8_t buf[6];
    size_t n = 0;
    EXPECT_TRUE(glyph_run::encode(r, buf, sizeof(buf), &n));
    out.insert(out.end(), buf, buf + n);
  }
  return out;
}

struct Seen {
  GlyphRecord rec;
  int pen;
};

std::vector<Seen> walk(const std::vector<uint8_t>& bytes, uint16_t count, int x, bool accumulate) {
  GlyphRun run;
  run.records = bytes.data();
  run.count = count;
  run.bytes = static_cast<uint16_t>(bytes.size());
  std::vector<Seen> seen;
  forEachGlyph(run, x, accumulate, [&](const GlyphRecord& r, int pen) { seen.push_back({r, pen}); });
  return seen;
}

}  // namespace

TEST(GlyphRun, ARecordWithoutAnOffsetIsFourBytes) {
  GlyphRecord r;
  r.glyph = 0x0105;
  r.advance = 48;
  uint8_t out[6] = {};
  size_t n = 0;
  ASSERT_TRUE(glyph_run::encode(r, out, sizeof(out), &n));
  EXPECT_EQ(n, 4u);
  const uint8_t want[4] = {0x05, 0x01, 0x30, 0x00};
  EXPECT_EQ(0, memcmp(out, want, 4));
}

TEST(GlyphRun, AnOffsetAddsTwoBytes) {
  GlyphRecord r;
  r.glyph = 7;
  r.advance = 16;
  r.dx = -3;
  r.dy = 2;
  uint8_t out[6] = {};
  size_t n = 0;
  ASSERT_TRUE(glyph_run::encode(r, out, sizeof(out), &n));
  EXPECT_EQ(n, 6u);
  const uint8_t want[6] = {0x07, 0x00, 0x10, 0x80, 0xFD, 0x02};
  EXPECT_EQ(0, memcmp(out, want, 6));

  GlyphRecord back;
  size_t read = 0;
  ASSERT_TRUE(glyph_run::decode(out, sizeof(out), &back, &read));
  EXPECT_EQ(read, 6u);
  EXPECT_EQ(back.glyph, 7);
  EXPECT_EQ(back.advance, 16);
  EXPECT_EQ(back.dx, -3);
  EXPECT_EQ(back.dy, 2);
}

TEST(GlyphRun, ANegativeAdvanceRoundTrips) {
  GlyphRecord r;
  r.glyph = 1;
  r.advance = -16;
  uint8_t out[4] = {};
  size_t n = 0;
  ASSERT_TRUE(glyph_run::encode(r, out, sizeof(out), &n));
  EXPECT_EQ(out[2] | (out[3] << 8), 0x7FF0);
  GlyphRecord back;
  size_t read = 0;
  ASSERT_TRUE(glyph_run::decode(out, sizeof(out), &back, &read));
  EXPECT_EQ(back.advance, -16);
}

TEST(GlyphRun, AnAdvancePastTheFieldIsRefused) {
  // advance is int16_t, so 16384 and -16385 are representable there but not in the 15-bit field.
  for (int v : {16384, -16385}) {
    GlyphRecord r;
    r.advance = static_cast<int16_t>(v);
    uint8_t out[6];
    memset(out, 0xAA, sizeof(out));
    size_t n = 99;
    EXPECT_FALSE(glyph_run::encode(r, out, sizeof(out), &n));
    for (uint8_t b : out) EXPECT_EQ(b, 0xAA);
    EXPECT_EQ(n, 99u);
  }
  GlyphRecord ok;
  ok.advance = 0;
  uint8_t small[3];
  size_t n = 0;
  EXPECT_FALSE(glyph_run::encode(ok, small, sizeof(small), &n));
}

TEST(GlyphRun, TheWidestGlyphAt120PxFits) {
  GlyphRecord r;
  r.glyph = 9;
  r.advance = 5760;  // 3 em at 120 px, 12.4
  uint8_t out[4];
  size_t n = 0;
  ASSERT_TRUE(glyph_run::encode(r, out, sizeof(out), &n));
  GlyphRecord back;
  size_t read = 0;
  ASSERT_TRUE(glyph_run::decode(out, sizeof(out), &back, &read));
  EXPECT_EQ(back.advance, 5760);
}

TEST(GlyphRun, ARecordCutShortEndsTheWalk) {
  GlyphRecord a;
  a.glyph = 1;
  a.advance = 16;
  GlyphRecord b;
  b.glyph = 2;
  b.advance = 16;
  b.dx = 1;
  const auto bytes = encodeAll({a, b});
  ASSERT_EQ(bytes.size(), 10u);
  GlyphRun run;
  run.records = bytes.data();
  run.count = 2;
  run.bytes = 5;
  int visits = 0;
  forEachGlyph(run, 0, false, [&](const GlyphRecord&, int) { visits++; });
  EXPECT_EQ(visits, 1);
}

TEST(GlyphRun, ThePenSnapsPerRecordUnscaled) {
  GlyphRecord a;
  a.advance = 88;
  const auto bytes = encodeAll({a, a});
  const auto seen = walk(bytes, 2, 0, false);
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[0].pen, 0);
  EXPECT_EQ(seen[1].pen, 6);
  GlyphRun run;
  run.records = bytes.data();
  run.count = 2;
  run.bytes = static_cast<uint16_t>(bytes.size());
  EXPECT_EQ(glyphRunAdvance(run, false), 12);
}

TEST(GlyphRun, ThePenAccumulatesAtAScale) {
  GlyphRecord a;
  a.advance = 88;
  const auto bytes = encodeAll({a, a});
  const auto seen = walk(bytes, 2, 0, true);
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[0].pen, 0);
  EXPECT_EQ(seen[1].pen, 6);
  GlyphRun run;
  run.records = bytes.data();
  run.count = 2;
  run.bytes = static_cast<uint16_t>(bytes.size());
  EXPECT_EQ(glyphRunAdvance(run, true), 11);
}

TEST(GlyphRun, AMarkSitsAtThePenPlusItsOffset) {
  GlyphRecord base;
  base.glyph = 1;
  base.advance = 128;
  GlyphRecord mark;
  mark.glyph = 2;
  mark.dx = -5;
  mark.dy = -3;
  const auto bytes = encodeAll({base, mark});
  const auto seen = walk(bytes, 2, 0, false);
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[1].pen, 8);
  EXPECT_EQ(seen[1].pen + seen[1].rec.dx, 3);
  EXPECT_EQ(seen[1].rec.dy, -3);
}
