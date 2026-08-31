// The contract that makes EpdFontGroup::ringBytes safe to trust.
//
// A group is no longer inflated into a buffer of its uncompressed size; it is streamed through
// a ring of ringBytes and each glyph is compacted out of the passing bytes. That is only sound
// if two things hold, and both are checked here against real shipped font data rather than a
// synthetic stream:
//
//   1. ringBytes is SUFFICIENT — every group decodes byte-for-byte through a ring that size.
//   2. ringBytes is HONEST — it is genuinely smaller than the group for the big groups, which
//      is the entire reason the field exists. A converter that quietly emitted
//      ringBytes == uncompressedSize would pass (1) while delivering nothing.
//
// And the failure mode is checked too: a ring that is too small must be REFUSED, not decoded
// into garbage. uzlib returns TINF_DICT_ERROR for a back-reference that outruns the ring, so a
// mis-generated font cannot silently render wrong pixels.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "EpdFontData.h"
#include "FontDecompressor.h"
#include "uzlib.h"

// A real generated font: 15 groups, uncompressed sizes up to ~25 KB, rings measured from the
// finished DEFLATE streams by fontconvert.py.
#include "notosans_14_regular.h"

// The font with the largest glyph in the built-in set (54x38, 513 packed bytes), which used to
// overflow HOT_GLYPH_BUF_SIZE by one byte.
#include "bookerly_18_bolditalic.h"

namespace {

struct Decoded {
  bool ok = false;
  std::vector<uint8_t> bytes;
};

// Decode one group through a ring of exactly `ringBytes`, the way FontDecompressor::GroupStream
// does: output is pulled in small chunks while the ring alone carries back-reference history.
Decoded DecodeThroughRing(const EpdFontData& font, const EpdFontGroup& group, uint32_t ringBytes) {
  std::vector<uint8_t> ring(ringBytes, 0);
  std::vector<uint8_t> out(group.uncompressedSize);

  uzlib_uncomp d;
  memset(&d, 0, sizeof(d));
  uzlib_uncompress_init(&d, ring.data(), ringBytes);
  d.source = &font.bitmap[group.compressedOffset];
  d.source_limit = d.source + group.compressedSize;

  // 64 bytes at a time: the same MAX_ROW_STRIDE-sized pull the real decoder uses to skip
  // forward, so the ring is exercised across many separate calls rather than one big read.
  uint32_t produced = 0;
  while (produced < group.uncompressedSize) {
    const uint32_t want = std::min<uint32_t>(64, group.uncompressedSize - produced);
    d.dest = out.data() + produced;
    d.dest_start = out.data();
    d.dest_limit = d.dest + want;
    const int res = uzlib_uncompress(&d);
    if (res != TINF_OK && res != TINF_DONE) return {};
    const uint32_t got = static_cast<uint32_t>(d.dest - (out.data() + produced));
    if (got == 0) return {};
    produced += got;
  }
  return {true, std::move(out)};
}

// Only the fields the decode path reads; everything else stays zeroed. Assigned rather than
// brace-initialised because EpdFontData has ~25 members and listing four of them trips
// -Wmissing-field-initializers.
const EpdFontData& Font() {
  static const EpdFontData font = [] {
    EpdFontData f{};
    f.bitmap = notosans_14_regularBitmaps;
    f.glyph = notosans_14_regularGlyphs;
    f.intervals = notosans_14_regularIntervals;
    f.intervalCount = sizeof(notosans_14_regularIntervals) / sizeof(notosans_14_regularIntervals[0]);
    return f;
  }();
  return font;
}

const EpdFontGroup* Groups() { return notosans_14_regularGroups; }
constexpr uint16_t kGroupCount = sizeof(notosans_14_regularGroups) / sizeof(notosans_14_regularGroups[0]);

}  // namespace

// (1) Every group decodes correctly through exactly the ring the converter promised.
TEST(FontGroupStream, RingBytesIsSufficient) {
  for (uint16_t i = 0; i < kGroupCount; i++) {
    const EpdFontGroup& g = Groups()[i];
    ASSERT_GT(g.ringBytes, 0u) << "group " << i << " has no measured ring";
    ASSERT_LE(g.ringBytes, g.uncompressedSize) << "group " << i << " claims a ring larger than itself";

    const Decoded ring = DecodeThroughRing(Font(), g, g.ringBytes);
    ASSERT_TRUE(ring.ok) << "group " << i << " failed to decode through its own " << g.ringBytes << "-byte ring";

    // Ground truth: the same stream decoded with a full 32 KB window.
    const Decoded full = DecodeThroughRing(Font(), g, 32768);
    ASSERT_TRUE(full.ok) << "group " << i << " failed to decode at all";
    EXPECT_EQ(ring.bytes, full.bytes) << "group " << i << " decoded differently through its ring";
  }
}

// (2) The field actually buys something. Without this the whole change is a no-op that still
// passes every correctness check.
TEST(FontGroupStream, RingIsSmallerThanTheGroup) {
  uint32_t peakRing = 0;
  uint32_t peakGroup = 0;
  for (uint16_t i = 0; i < kGroupCount; i++) {
    peakRing = std::max(peakRing, static_cast<uint32_t>(Groups()[i].ringBytes));
    peakGroup = std::max(peakGroup, Groups()[i].uncompressedSize);
  }
  // The transient the reader pays is the peak ring, not the peak group.
  EXPECT_LT(peakRing, peakGroup) << "rings are no cheaper than inflating whole groups";
  // GROUP_RING_MAX_BYTES in fontconvert.py. The converter enforces it; this is the on-device
  // half of that contract, and it is what bounds the transient allocation.
  EXPECT_LE(peakRing, 4096u) << "a group exceeds the ring ceiling the decoder budgets for";
}

// Every glyph must fit the fallback cache slot. A glyph that does not is not a crash and not a
// test failure anywhere else — getBitmap simply refuses it and the character renders blank on
// any page whose prewarm missed it, which is how one 513-byte glyph went unnoticed against a
// 512-byte buffer. fontconvert.py enforces the same bound when generating; this is the shipped-
// data side of that contract, so a hand-edited or stale header cannot reintroduce it.
TEST(FontGroupStream, EveryGlyphFitsTheFallbackSlot) {
  struct FontUnderTest {
    const char* name;
    const EpdGlyph* glyphs;
    size_t count;
  };
  const FontUnderTest fonts[] = {
      {"bookerly_18_bolditalic", bookerly_18_bolditalicGlyphs,
       sizeof(bookerly_18_bolditalicGlyphs) / sizeof(bookerly_18_bolditalicGlyphs[0])},
      {"notosans_14_regular", notosans_14_regularGlyphs,
       sizeof(notosans_14_regularGlyphs) / sizeof(notosans_14_regularGlyphs[0])},
  };

  for (const auto& f : fonts) {
    uint16_t largest = 0;
    for (size_t i = 0; i < f.count; i++) {
      largest = std::max(largest, f.glyphs[i].dataLength);
      ASSERT_LE(f.glyphs[i].dataLength, FontDecompressor::HOT_GLYPH_BUF_SIZE)
          << f.name << " glyph " << i << " (" << +f.glyphs[i].width << "x" << +f.glyphs[i].height
          << ") cannot be served by the fallback cache and will render blank";
    }
    EXPECT_GT(largest, 0u) << f.name << " has no glyph data";
  }
}

// A ring too small must fail loudly. This is what stops a bad ringBytes from turning into
// wrong pixels instead of a visible error.
TEST(FontGroupStream, TooSmallRingIsRefusedNotCorrupted) {
  // The largest group is the one whose references certainly outrun a tiny ring.
  uint16_t widest = 0;
  for (uint16_t i = 1; i < kGroupCount; i++) {
    if (Groups()[i].ringBytes > Groups()[widest].ringBytes) widest = i;
  }
  const EpdFontGroup& g = Groups()[widest];
  ASSERT_GT(g.ringBytes, 512u) << "fixture has no group with a reach worth truncating";

  const Decoded good = DecodeThroughRing(Font(), g, g.ringBytes);
  ASSERT_TRUE(good.ok);

  const Decoded starved = DecodeThroughRing(Font(), g, 256);
  // Either uzlib refuses it (the expected TINF_DICT_ERROR path) or -- if this particular
  // stream happens to survive -- the bytes must still be right. Silent corruption is the
  // only outcome that is not allowed.
  if (starved.ok) {
    EXPECT_EQ(starved.bytes, good.bytes) << "a starved ring produced different bytes without erroring";
  }
}
