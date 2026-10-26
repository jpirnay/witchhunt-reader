// FallbackFaceProvider: the CJK UI faces serving what the Inter UI faces lack, through the same
// EpdFont lookup and bitmap hand-off the renderer uses.
//
// The CJK faces ship compressed (compress_ui_face.py), so the provider decodes their glyphs through
// a FontDecompressor; and the active UI language's faces are served flat, out of the language pack
// slot (CjkUiFonts). Both are exercised: a "flat" face here is a RAM copy of a compressed one that
// points at its inflated bitmap, which is exactly what CjkUiFonts builds over the mapped slot.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "EpdFont.h"
#include "FallbackFaceProvider.h"
#include "FontDecompressor.h"
#include "builtinFonts/cjk_ui_ja_10.h"
#include "builtinFonts/cjk_ui_ko_10.h"
#include "builtinFonts/cjk_ui_zh_10.h"
#include "builtinFonts/inter_ui_10_regular.h"

namespace {

constexpr uint32_t ZH_ONLY = 0x8BBE;      // 设: in the Chinese set, not the Japanese
constexpr uint32_t ZH_AND_JA = 0x5B9A;    // 定: in both, with each region's glyph form
constexpr uint32_t JA_ONLY = 0x66F8;      // 書
constexpr uint32_t HANGUL = 0xC124;       // 설
constexpr uint32_t IN_NO_FACE = 0x9F98;   // 龘
constexpr uint32_t GREEK_ALPHA = 0x03B1;  // in no face, and no CJK face's to answer for
constexpr uint32_t STAND_IN = 0x25A1;     // □, which every CJK UI face carries

using Bytes = std::vector<uint8_t>;

bool claimsHan(const uint32_t cp) { return cp >= 0x4E00 && cp <= 0x9FFF; }

// A UI face as CjkUiFonts::attach() leaves it: a RAM copy of the flash table, carrying the provider.
EpdFontData withProvider(const EpdFontData& face, GlyphProvider* provider) {
  EpdFontData copy = face;
  copy.provider = provider;
  return copy;
}

// What the renderer draws for a resolved glyph (GfxRenderer::getGlyphBitmap, uncompressed path),
// copied out at once: a decoded glyph lives only until the next decode.
Bytes drawn(const EpdFontData& font, const EpdGlyphRef& glyph) {
  const uint8_t* bitmap = &font.bitmap[epdGlyphBitmapOffset(&font, glyph)];
  if (font.provider && glyph.sdRecord) {
    const uint8_t* owned = nullptr;
    if (font.provider->ownsBitmap(glyph.sdRecord, owned)) bitmap = owned;
  }
  if (!bitmap) return {};
  return Bytes(bitmap, bitmap + glyphDataBytes(glyph.width, glyph.height, font.is2Bit));
}

// The bitmap compressed `face` holds for `cp` by its own tables, through a decompressor of its own.
Bytes faceBytes(const EpdFontData& face, const uint32_t cp) {
  static FontDecompressor reference;
  const EpdGlyphRef glyph = EpdFont(&face).findGlyph(cp);
  const uint8_t* bitmap = reference.getBitmap(&face, glyph, glyph.index);
  if (!bitmap) return {};
  return Bytes(bitmap, bitmap + glyphDataBytes(glyph.width, glyph.height, face.is2Bit));
}

// A compressed face inflated to the flat form the language pack slot holds: bitmap plus offsets.
struct FlatFace {
  Bytes bitmap;
  std::vector<uint16_t> offsets;
  EpdFontData data{};
};

bool appendTo(void* ctx, const uint8_t* bytes, const uint32_t len) {
  auto* out = static_cast<Bytes*>(ctx);
  out->insert(out->end(), bytes, bytes + len);
  return true;
}

void flatten(const EpdFontData& face, FlatFace& out) {
  Bytes ring(FontDecompressor::maxRingBytes(&face));
  Bytes glyph(FontDecompressor::HOT_GLYPH_BUF_SIZE);
  ASSERT_TRUE(FontDecompressor::inflateFace(&face, ring.data(), static_cast<uint32_t>(ring.size()), glyph.data(),
                                            &appendTo, &out.bitmap));
  const EpdFontGroup& last = face.groups[face.groupCount - 1];
  uint32_t offset = 0;
  for (uint32_t i = 0; i < last.firstGlyphIndex + last.glyphCount; ++i) {
    out.offsets.push_back(static_cast<uint16_t>(offset));
    const EpdGlyphRef g = epdResolveGlyph(&face, i);
    offset += glyphDataBytes(g.width, g.height, false);
  }
  out.data = face;
  out.data.bitmap = out.bitmap.data();
  out.data.bitmapOffsets = out.offsets.data();
  out.data.groups = nullptr;
  out.data.groupCount = 0;
}

class FallbackFaceProviderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    provider.setDecompressor(&decompressor);
    const EpdFontData* faces[] = {&cjk_ui_zh_10, &cjk_ui_ja_10, &cjk_ui_ko_10};
    ASSERT_TRUE(provider.setFaces(faces, 3));
    ui = withProvider(inter_ui_10_regular, &provider);
  }
  FontDecompressor decompressor;
  FallbackFaceProvider provider;
  EpdFontData ui{};
};

TEST_F(FallbackFaceProviderTest, CjkGlyphComesFromTheCjkFaceWithItsMetricsAndBitmap) {
  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph(ZH_ONLY);
  ASSERT_TRUE(glyph);
  const EpdGlyphRef own = EpdFont(&cjk_ui_zh_10).findGlyph(ZH_ONLY);
  EXPECT_EQ(glyph.width, own.width);
  EXPECT_EQ(glyph.height, own.height);
  EXPECT_EQ(glyph.advanceX, own.advanceX);
  EXPECT_EQ(glyph.left, own.left);
  EXPECT_EQ(glyph.top, own.top);
  const Bytes bitmap = drawn(ui, glyph);
  ASSERT_FALSE(bitmap.empty());
  EXPECT_EQ(bitmap, faceBytes(cjk_ui_zh_10, ZH_ONLY));
}

TEST_F(FallbackFaceProviderTest, LatinStaysWithTheUiFace) {
  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph('A');
  ASSERT_TRUE(glyph);
  EXPECT_EQ(glyph.sdRecord, nullptr);
  const EpdGlyphRef own = EpdFont(&inter_ui_10_regular).findGlyph('A');
  const uint8_t* expected = inter_ui_10_regular.bitmap + epdGlyphBitmapOffset(&inter_ui_10_regular, own);
  EXPECT_EQ(drawn(ui, glyph), Bytes(expected, expected + glyphDataBytes(own.width, own.height, false)));
}

TEST_F(FallbackFaceProviderTest, LaterFacesServeWhatEarlierOnesLack) {
  const EpdGlyphRef kanji = EpdFont(&ui).getGlyph(JA_ONLY);
  ASSERT_TRUE(kanji);
  EXPECT_EQ(drawn(ui, kanji), faceBytes(cjk_ui_ja_10, JA_ONLY));
  const EpdGlyphRef hangul = EpdFont(&ui).getGlyph(HANGUL);
  ASSERT_TRUE(hangul);
  EXPECT_EQ(drawn(ui, hangul), faceBytes(cjk_ui_ko_10, HANGUL));
}

TEST_F(FallbackFaceProviderTest, TheFirstFaceWinsACharacterTwoFacesShare) {
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_AND_JA)), faceBytes(cjk_ui_zh_10, ZH_AND_JA));

  const EpdFontData* japaneseFirst[] = {&cjk_ui_ja_10, &cjk_ui_zh_10, &cjk_ui_ko_10};
  ASSERT_TRUE(provider.setFaces(japaneseFirst, 3));
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_AND_JA)), faceBytes(cjk_ui_ja_10, ZH_AND_JA));
  // The two regions' forms really differ, or the line above proves nothing.
  EXPECT_NE(faceBytes(cjk_ui_ja_10, ZH_AND_JA), faceBytes(cjk_ui_zh_10, ZH_AND_JA));
}

TEST_F(FallbackFaceProviderTest, ACharacterNoFaceHasStaysMissingWithoutAStandIn) {
  // The UI face has no U+FFFD either, so the lookup ends empty: nothing is drawn, as before.
  EXPECT_FALSE(EpdFont(&ui).getGlyph(IN_NO_FACE));
}

TEST_F(FallbackFaceProviderTest, AClaimedCharacterNoFaceHasDrawsAsTheStandIn) {
  provider.setStandIn(STAND_IN, &claimsHan);
  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph(IN_NO_FACE);
  ASSERT_TRUE(glyph);
  EXPECT_EQ(drawn(ui, glyph), faceBytes(cjk_ui_zh_10, STAND_IN));
}

TEST_F(FallbackFaceProviderTest, AStandInLeavesCoveredAndUnclaimedCharactersAlone) {
  provider.setStandIn(STAND_IN, &claimsHan);
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_ONLY)), faceBytes(cjk_ui_zh_10, ZH_ONLY));
  EXPECT_FALSE(EpdFont(&ui).getGlyph(GREEK_ALPHA));
}

TEST_F(FallbackFaceProviderTest, ARecordStaysOwnedWhileLaterMissesFillTheRestOfTheRing) {
  const EpdGlyphRef first = EpdFont(&ui).getGlyph(ZH_ONLY);
  ASSERT_TRUE(first);
  const Bytes expected = faceBytes(cjk_ui_zh_10, ZH_ONLY);
  // Seven more misses fill the other slots of the eight-deep ring.
  for (const uint32_t cp : {JA_ONLY, HANGUL, ZH_AND_JA, JA_ONLY, HANGUL, ZH_AND_JA, JA_ONLY}) {
    ASSERT_TRUE(EpdFont(&ui).getGlyph(cp));
  }
  EXPECT_EQ(drawn(ui, first), expected);
}

TEST_F(FallbackFaceProviderTest, ARecordFromElsewhereIsNotOwned) {
  EpdGlyph foreign{};
  const uint8_t* bitmap = nullptr;
  EXPECT_FALSE(provider.ownsBitmap(&foreign, bitmap));
}

TEST_F(FallbackFaceProviderTest, AnIdeographicSpaceIsOwnedAndHasNoBitmap) {
  const EpdGlyphRef space = EpdFont(&ui).getGlyph(0x3000);
  ASSERT_TRUE(space);
  EXPECT_GT(space.advanceX, 0);
  const uint8_t* bitmap = reinterpret_cast<const uint8_t*>(1);
  EXPECT_TRUE(provider.ownsBitmap(space.sdRecord, bitmap));
  EXPECT_EQ(bitmap, nullptr);
}

TEST_F(FallbackFaceProviderTest, MeasuringDecodesNothing) {
  // Layout resolves every glyph of every row and draws none of them: only a drawn glyph may cost a
  // group decode.
  decompressor.resetStats();
  for (const uint32_t cp : {ZH_ONLY, JA_ONLY, HANGUL, ZH_AND_JA}) ASSERT_TRUE(EpdFont(&ui).getGlyph(cp));
  EXPECT_EQ(decompressor.getStats().getBitmapCalls, 0u);
}

TEST_F(FallbackFaceProviderTest, AFlatFaceIsServedStraightFromItsBitmap) {
  // The active UI language's face as CjkUiFonts serves it from the language pack slot.
  FlatFace zh;
  ASSERT_NO_FATAL_FAILURE(flatten(cjk_ui_zh_10, zh));
  const EpdFontData* faces[] = {&zh.data, &cjk_ui_ja_10, &cjk_ui_ko_10};
  ASSERT_TRUE(provider.setFaces(faces, 3));

  decompressor.resetStats();
  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph(ZH_ONLY);
  ASSERT_TRUE(glyph);
  const uint8_t* owned = nullptr;
  ASSERT_TRUE(provider.ownsBitmap(glyph.sdRecord, owned));
  const EpdGlyphRef own = EpdFont(&zh.data).findGlyph(ZH_ONLY);
  EXPECT_EQ(owned, zh.bitmap.data() + zh.offsets[own.index]) << "a flat face is read in place";
  EXPECT_EQ(decompressor.getStats().getBitmapCalls, 0u) << "and costs no decode";
  EXPECT_EQ(drawn(ui, glyph), faceBytes(cjk_ui_zh_10, ZH_ONLY));
  // A face behind it still decodes.
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(JA_ONLY)), faceBytes(cjk_ui_ja_10, JA_ONLY));
}

TEST_F(FallbackFaceProviderTest, AGlyphThatCannotBeDecodedIsOwnedAndDrawsNothing) {
  // A stream that ends at once: what a decode looks like to the provider when it fails. It must
  // still claim the record, or the renderer would read the UI face's own bitmap at this offset.
  EpdFontData broken = cjk_ui_zh_10;
  std::vector<EpdFontGroup> groups(cjk_ui_zh_10.groups, cjk_ui_zh_10.groups + cjk_ui_zh_10.groupCount);
  for (EpdFontGroup& g : groups) g.compressedSize = 1;
  broken.groups = groups.data();
  const EpdFontData* faces[] = {&broken};
  ASSERT_TRUE(provider.setFaces(faces, 1));

  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph(ZH_ONLY);
  ASSERT_TRUE(glyph);
  const uint8_t* bitmap = reinterpret_cast<const uint8_t*>(1);
  EXPECT_TRUE(provider.ownsBitmap(glyph.sdRecord, bitmap));
  EXPECT_EQ(bitmap, nullptr);
}

TEST(FallbackFaceProviderSetup, ACompressedFaceIsRefusedWithoutADecompressor) {
  FallbackFaceProvider provider;
  const EpdFontData* faces[] = {&cjk_ui_ja_10};
  EXPECT_FALSE(provider.setFaces(faces, 1));
  EXPECT_EQ(provider.missGlyph(JA_ONLY), nullptr);
}

TEST(FallbackFaceProviderSetup, MoreFacesThanItHoldsAreRefused) {
  FontDecompressor decompressor;
  FallbackFaceProvider provider;
  provider.setDecompressor(&decompressor);
  const EpdFontData* faces[FallbackFaceProvider::MAX_FACES + 1] = {};
  for (auto& face : faces) face = &cjk_ui_zh_10;
  EXPECT_FALSE(provider.setFaces(faces, FallbackFaceProvider::MAX_FACES + 1));
  EXPECT_EQ(provider.missGlyph(ZH_ONLY), nullptr);
}




TEST(FallbackFaceProviderPackInkMask, PackInkMaskConverts2BitTo1Bit) {
  // 4 pixels: values 3, 1, 0, 2 -> 0b11010010 (0xD2)
  // 4 pixels: values 0, 3, 0, 1 -> 0b00110001 (0x31)
  // Result 8 pixels: 1, 1, 0, 1, 0, 1, 0, 1 -> 0b11010101 (0xD5)
  const uint8_t src[2] = {0xD2, 0x31};
  uint8_t dst[1] = {0};
  packInkMask(src, 2, dst);
  EXPECT_EQ(dst[0], 0xD5);
}

TEST(FallbackFaceProviderPackInkMask, PackInkMaskPartialByte) {
  // 3 pixels: values 1, 0, 2 -> 0b01001000 in 2-bit
  // 1-bit: 1, 0, 1 in top 3 bits -> 0b10100000 (0xA0)
  const uint8_t src[1] = {0b01001000};
  uint8_t dst[1] = {0};
  packInkMask(src, 1, dst);
  EXPECT_EQ(dst[0], 0b10100000);
}

TEST_F(FallbackFaceProviderTest, Chained2BitFaceProducesPacked1BitBitmap) {
  // 4 pixels wide x 2 pixels high: 2-bit takes 2 bytes.
  // Pixel values: 3, 1, 0, 2 (0xD2) and 0, 3, 0, 1 (0x31).
  // Packed 1-bit takes 1 byte: 0xD5.
  EpdGlyph glyph{4, 2, 4, 0, 0, 1, 0};
  EpdUnicodeInterval interval{IN_NO_FACE, IN_NO_FACE, 0};
  uint8_t twoBitBmp[2] = {0xD2, 0x31};
  EpdFontData twoBitFace{};
  twoBitFace.bitmap = twoBitBmp;
  twoBitFace.glyph = &glyph;
  twoBitFace.intervals = &interval;
  twoBitFace.intervalCount = 1;
  twoBitFace.is2Bit = true;

  provider.setChainedFace(&twoBitFace, false);
  const EpdGlyphRef resolved = EpdFont(&ui).getGlyph(IN_NO_FACE);
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved.width, 4);
  EXPECT_EQ(resolved.height, 2);
  const Bytes bmp = drawn(ui, resolved);
  ASSERT_EQ(bmp.size(), 1u);
  EXPECT_EQ(bmp[0], 0xD5);
}

TEST_F(FallbackFaceProviderTest, ChainedFacePriorityOrdering) {
  // Synthetic chained face providing ZH_AND_JA with a distinct 1-bit bitmap
  EpdGlyph chainedGlyph{8, 8, 8, 0, 0, 8, 0};
  EpdUnicodeInterval chainedInterval{ZH_AND_JA, ZH_AND_JA, 0};
  uint8_t chainedBmp[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  EpdFontData chainedFace{};
  chainedFace.bitmap = chainedBmp;
  chainedFace.glyph = &chainedGlyph;
  chainedFace.intervals = &chainedInterval;
  chainedFace.intervalCount = 1;

  // 1. By default with afterFirstFace = true, face 0 (cjk_ui_zh_10) wins over chained face
  provider.setChainedFace(&chainedFace, true);
  EXPECT_TRUE(provider.hasChainedFace());
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_AND_JA)), faceBytes(cjk_ui_zh_10, ZH_AND_JA));

  // 2. When afterFirstFace is false (UI language is not CJK), chained face takes priority
  provider.setAfterFirstFace(false);
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_AND_JA)), Bytes(chainedBmp, chainedBmp + 8));

  // 3. When chained face is detached, face 0 wins
  provider.setChainedFace(nullptr);
  EXPECT_FALSE(provider.hasChainedFace());
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_AND_JA)), faceBytes(cjk_ui_zh_10, ZH_AND_JA));
}

TEST_F(FallbackFaceProviderTest, Oversize2BitGlyphFallsBackToStandIn) {
  provider.setStandIn(STAND_IN, &claimsHan);

  // A 2-bit glyph with width=64, height=33 -> 1-bit bytes = (64*33+7)/8 = 264 > PACKED_BUF_BYTES (256)
  EpdGlyph bigGlyph{64, 33, 64, 0, 0, 264, 0};
  EpdUnicodeInterval bigInterval{IN_NO_FACE, IN_NO_FACE, 0};
  EpdFontData bigFace{};
  bigFace.glyph = &bigGlyph;
  bigFace.intervals = &bigInterval;
  bigFace.intervalCount = 1;
  bigFace.is2Bit = true;

  provider.setChainedFace(&bigFace, false);
  // IN_NO_FACE is in bigFace, but exceeds PACKED_BUF_BYTES, so serveFace returns nullptr.
  // Since claimsHan(IN_NO_FACE) is true, missGlyph serves the stand-in from face 0!
  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph(IN_NO_FACE);
  ASSERT_TRUE(glyph);
  EXPECT_EQ(drawn(ui, glyph), faceBytes(cjk_ui_zh_10, STAND_IN));
}

TEST_F(FallbackFaceProviderTest, InvalidateRingClearsServedSlots) {
  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph(ZH_ONLY);
  ASSERT_TRUE(glyph);
  const uint8_t* bmp = nullptr;
  EXPECT_TRUE(provider.ownsBitmap(glyph.sdRecord, bmp));
  EXPECT_NE(bmp, nullptr);

  // setChainedFace invalidates the ring slots
  provider.setChainedFace(nullptr);
  bmp = reinterpret_cast<const uint8_t*>(1);
  EXPECT_TRUE(provider.ownsBitmap(glyph.sdRecord, bmp));
  EXPECT_EQ(bmp, nullptr);
}

TEST_F(FallbackFaceProviderTest, OnDemandHookAttachesChainedFaceWhenGlyphMissing) {
  static uint32_t lastCodepoint = 0;
  lastCodepoint = 0;

  // A synthetic chained face covering IN_NO_FACE and Greek Capital Omicron (0x039F)
  static EpdGlyph chainedGlyphs[2] = {
      {8, 8, 8, 0, 0, 8, 0},
      {8, 8, 8, 0, 0, 8, 0}
  };
  static EpdUnicodeInterval chainedIntervals[2] = {
      {0x039F, 0x039F, 0},
      {IN_NO_FACE, IN_NO_FACE, 1}
  };
  static uint8_t chainedBmp[16];
  for (int i = 0; i < 16; ++i) chainedBmp[i] = 0xAA;
  static EpdFontData chainedFace{};
  chainedFace.bitmap = chainedBmp;
  chainedFace.glyph = chainedGlyphs;
  chainedFace.intervals = chainedIntervals;
  chainedFace.intervalCount = 2;

  static FallbackFaceProvider* testProvider = &provider;
  testProvider = &provider;
  provider.setOnDemandHook([](uint32_t cp) {
    lastCodepoint = cp;
    testProvider->setChainedFace(&chainedFace, true);
  });

  // 1. A glyph already covered by faces_[0] (e.g. ZH_ONLY) does not call on-demand hook
  EXPECT_EQ(lastCodepoint, 0u);
  const EpdGlyphRef zh = EpdFont(&ui).getGlyph(ZH_ONLY);
  ASSERT_TRUE(zh);
  EXPECT_EQ(lastCodepoint, 0u);

  // 2. A CJK glyph not covered by faces_[0] (e.g. IN_NO_FACE) triggers on-demand hook with its codepoint and borrows
  const EpdGlyphRef borrowed = EpdFont(&ui).getGlyph(IN_NO_FACE);
  ASSERT_TRUE(borrowed);
  EXPECT_EQ(lastCodepoint, IN_NO_FACE);
  EXPECT_EQ(drawn(ui, borrowed), Bytes(chainedBmp, chainedBmp + 8));

  // 3. A non-CJK glyph missing from UI faces (e.g. Greek Capital Omicron 0x039F) triggers on-demand hook and borrows
  provider.setChainedFace(nullptr);
  lastCodepoint = 0;
  const EpdGlyphRef greek = EpdFont(&ui).getGlyph(0x039F);
  ASSERT_TRUE(greek);
  EXPECT_EQ(lastCodepoint, 0x039Fu);
  EXPECT_EQ(drawn(ui, greek), Bytes(chainedBmp, chainedBmp + 8));

  // 4. Detached font allows hook to be called again
  provider.setChainedFace(nullptr);
  lastCodepoint = 0;
  const EpdGlyphRef borrowedAgain = EpdFont(&ui).getGlyph(IN_NO_FACE);
  ASSERT_TRUE(borrowedAgain);
  EXPECT_EQ(lastCodepoint, IN_NO_FACE);

  // Cleanup
  provider.setOnDemandHook(nullptr);
  provider.setChainedFace(nullptr);
}

TEST_F(FallbackFaceProviderTest, AutomaticStringUniformityPrefersChainedWhenMissing) {
  // A chained face providing both ZH_AND_JA and IN_NO_FACE
  static EpdGlyph chainedGlyphs[2] = {
      {8, 8, 8, 0, 0, 8, 0},
      {8, 8, 8, 0, 0, 8, 0}
  };
  static EpdUnicodeInterval chainedIntervals[2] = {
      {ZH_AND_JA, ZH_AND_JA, 0},
      {IN_NO_FACE, IN_NO_FACE, 1}
  };
  static uint8_t chainedBmp[16];
  for (int i = 0; i < 16; ++i) chainedBmp[i] = 0xAA;
  static EpdFontData chainedFace{};
  chainedFace.bitmap = chainedBmp;
  chainedFace.glyph = chainedGlyphs;
  chainedFace.intervals = chainedIntervals;
  chainedFace.intervalCount = 2;

  provider.setChainedFace(&chainedFace, true);

  // Case A: string contains only characters in faces_[0] -> preferChained_ stays false, faces_[0] wins
  provider.prefetch("\xE5\xAE\x9A"); // ZH_AND_JA
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_AND_JA)), faceBytes(cjk_ui_zh_10, ZH_AND_JA));

  // Case B: string contains a character outside faces_[0] (IN_NO_FACE) alongside ZH_AND_JA
  // -> preferChained_ becomes true, so BOTH draw in the chained face!
  provider.prefetch("\xE5\xAE\x9A\xE9\xBE\x98"); // ZH_AND_JA + IN_NO_FACE
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_AND_JA)), Bytes(chainedBmp, chainedBmp + 8));

  provider.setChainedFace(nullptr);
}

TEST_F(FallbackFaceProviderTest, BatchPrefetchInvokesChainedProvider) {
  struct MockChainedProvider : public GlyphProvider {
    std::string prefetched;
    const EpdGlyph* missGlyph(uint32_t) override { return nullptr; }
    bool ownsBitmap(const EpdGlyph*, const uint8_t*&) const override { return false; }
    void prefetch(const char* utf8Text) override {
      if (utf8Text) prefetched += utf8Text;
    }
  } mockProvider;

  EpdFontData chainedFace{};
  chainedFace.provider = &mockProvider;
  provider.setChainedFace(&chainedFace);
  provider.setPrimaryFace(&inter_ui_10_regular);

  auto getter = [](void* ctx, int idx) -> const char* {
    auto* vec = static_cast<std::vector<std::string>*>(ctx);
    if (idx < 0 || idx >= static_cast<int>(vec->size())) return nullptr;
    return (*vec)[idx].c_str();
  };

  // 1. Strings with all CJK in faces_[0] (ZH) do not touch chained font
  std::vector<std::string> zhTitles = {
      "Hello World",
      "\xE4\xB9\xA6\xE5\x90\x8D\xE4\xB8\x80"  // 书名一 (all in cjk_ui_zh_10)
  };
  provider.prefetch(getter, &zhTitles, static_cast<int>(zhTitles.size()));
  EXPECT_EQ(mockProvider.prefetched, "");

  // 2. Strings containing a character not in faces_[0] (書) forward all CJK to chained font
  std::vector<std::string> mixedTitles = {
      "Hello World",
      "\xE4\xB9\xA6\xE5\x90\x8D\xE4\xB8\x80",
      "Second Title",
      "\xE6\x9B\xB8\xE5\x90\x8D\xE4\xBA\x8C"   // 書名二 (書 is not in cjk_ui_zh_10)
  };
  provider.prefetch(getter, &mixedTitles, static_cast<int>(mixedTitles.size()));
  EXPECT_EQ(mockProvider.prefetched, "\xE4\xB9\xA6\xE5\x90\x8D\xE4\xB8\x80\xE6\x9B\xB8\xE5\x90\x8D\xE4\xBA\x8C");
}

}  // namespace
