// Ours (writing modes step 2): the compact glyph records of a shaped run, their byte layout and the two pen rules.
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

TEST(GlyphRun, AnOffsetCutOffAfterItsFlagIsNotRead) {
  // A record whose flag says an offset follows, with 4 or 5 of its 6 bytes left (a run cut short, a section read back
  // from the card): decode refuses it and reads nothing past them, so the walk stops before it. The buffer holds all
  // six, so a decode that read on would return the offset.
  GlyphRecord r;
  r.glyph = 3;
  r.advance = 32;
  r.dx = 2;
  r.dy = -1;
  const auto bytes = encodeAll({r});
  ASSERT_EQ(bytes.size(), 6u);
  ASSERT_NE(bytes[3] & 0x80, 0);
  for (const size_t left : {size_t{4}, size_t{5}}) {
    GlyphRecord back;
    size_t read = 99;
    EXPECT_FALSE(glyph_run::decode(bytes.data(), left, &back, &read)) << left;
    EXPECT_EQ(read, 99u) << left;
    GlyphRun run;
    run.records = bytes.data();
    run.count = 1;
    run.bytes = static_cast<uint16_t>(left);
    int visits = 0;
    forEachGlyph(run, 0, false, [&](const GlyphRecord&, int) { visits++; });
    EXPECT_EQ(visits, 0) << left;
    EXPECT_EQ(glyphRunAdvance(run, false), 0) << left;
  }
  GlyphRecord back;
  size_t read = 0;
  ASSERT_TRUE(glyph_run::decode(bytes.data(), 6, &back, &read));  // whole, it is read
  EXPECT_EQ(read, 6u);
  EXPECT_EQ(back.dx, 2);
}

TEST(GlyphRun, TheFieldsEdgesRoundTrip) {
  // 16383 and -16384, 12.4: 1023.9375 and -1024 px, the most the 15-bit field holds either way.
  for (const int v : {glyph_run::kMaxAdvance, glyph_run::kMinAdvance}) {
    GlyphRecord r;
    r.glyph = 0xFFFF;
    r.advance = static_cast<int16_t>(v);
    uint8_t out[4] = {};
    size_t n = 0;
    ASSERT_TRUE(glyph_run::encode(r, out, sizeof(out), &n)) << v;
    EXPECT_EQ(n, 4u) << v;
    EXPECT_EQ(out[3] & 0x80, 0) << v;  // no offset follows, whatever the advance's sign
    GlyphRecord back;
    size_t read = 0;
    ASSERT_TRUE(glyph_run::decode(out, sizeof(out), &back, &read)) << v;
    EXPECT_EQ(back.advance, v);
    EXPECT_EQ(back.glyph, 0xFFFF);
  }
  EXPECT_EQ(glyph_run::kMaxAdvance, 16383);
  EXPECT_EQ(glyph_run::kMinAdvance, -16384);
}

TEST(GlyphRun, ANegativeAdvanceRoundsAsToPixelDoes) {
  // fp4::toPixel rounds half up: -8 (-0.5 px) moves the pen by 0, -9 by -1, and so does the walk.
  EXPECT_EQ(fp4::toPixel(-8), 0);
  EXPECT_EQ(fp4::toPixel(-9), -1);
  for (const int16_t advance : {int16_t{-8}, int16_t{-9}}) {
    GlyphRecord a;
    a.advance = advance;
    const auto bytes = encodeAll({a, a});
    const auto seen = walk(bytes, 2, 10, false);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].pen, 10);
    EXPECT_EQ(seen[1].pen, 10 + fp4::toPixel(advance)) << advance;
    GlyphRun run;
    run.records = bytes.data();
    run.count = 2;
    run.bytes = static_cast<uint16_t>(bytes.size());
    EXPECT_EQ(glyphRunAdvance(run, false), 2 * fp4::toPixel(advance)) << advance;
    EXPECT_EQ(glyphRunAdvance(run, true), fp4::toPixel(2 * advance)) << advance;  // -16: -1, -18: -1
  }
}
