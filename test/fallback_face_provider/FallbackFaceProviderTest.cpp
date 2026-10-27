// FallbackFaceProvider: the CJK UI faces serving what the Inter UI faces lack, through the same
// EpdFont lookup and bitmap hand-off the renderer uses.
//
// The CJK faces ship compressed (compress_ui_face.py), so the provider decodes their glyphs through
// a FontDecompressor; and the active UI language's faces are served flat, out of the language pack
// slot (CjkUiFonts). Both are exercised: a "flat" face here is a RAM copy of a compressed one that
// points at its inflated bitmap, which is exactly what CjkUiFonts builds over the mapped slot.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
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

// A reader font's face as TtfFont::nativeFonts() hands it out: 2-bit, no interval table, every glyph from its
// provider, which owns the glyph's bitmap.
struct ReaderFace final : GlyphProvider {
  std::vector<uint32_t> codepoints;
  EpdGlyph record{};
  Bytes bitmap;
  EpdFontData data{};
  int asked = 0;  // how often the provider's source asked for the face

  ReaderFace(std::vector<uint32_t> covered, const uint8_t width, const uint8_t height)
      : codepoints(std::move(covered)) {
    record = EpdGlyph{width,
                      height,
                      static_cast<uint16_t>(width << 4),
                      1,
                      static_cast<int16_t>(height),
                      glyphDataBytes(width, height, true),
                      0};
    bitmap.resize(record.dataLength);
    // Every level in every position: 0 (no ink), 1, 2 and 3.
    for (size_t i = 0; i < bitmap.size(); ++i) bitmap[i] = static_cast<uint8_t>(0x1B * (i + 1));
    data.is2Bit = true;
    data.advanceY = height;
    data.provider = this;
  }
  const EpdGlyph* missGlyph(const uint32_t cp) override {
    return std::find(codepoints.begin(), codepoints.end(), cp) != codepoints.end() ? &record : nullptr;
  }
  bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& out) const override {
    if (glyph != &record) return false;
    out = bitmap.data();
    return true;
  }
  void prefetch(const char* utf8Text) override { prefetched.emplace_back(utf8Text); }
  std::vector<std::string> prefetched;  // what each prefetch() was given
  Bytes ink() const {
    Bytes mask(glyphDataBytes(record.width, record.height, false));
    packInkMask(bitmap.data(), static_cast<uint32_t>(bitmap.size()), mask.data());
    return mask;
  }
  static const EpdFontData* source(void* ctx) {
    auto* face = static_cast<ReaderFace*>(ctx);
    ++face->asked;
    return &face->data;
  }
};

const EpdFontData* noFace(void* /*ctx*/) { return nullptr; }

TEST_F(FallbackFaceProviderTest, TheReaderFaceServesWhatTheUiFacesLackAsItsInk) {
  ReaderFace reader({GREEK_ALPHA}, 11, 14);
  provider.setReaderFace(&ReaderFace::source, &reader);
  const EpdGlyphRef glyph = EpdFont(&ui).getGlyph(GREEK_ALPHA);
  ASSERT_TRUE(glyph);
  EXPECT_EQ(glyph.width, 11);
  EXPECT_EQ(glyph.height, 14);
  EXPECT_EQ(glyph.advanceX, 11 << 4);
  EXPECT_EQ(drawn(ui, glyph), reader.ink());
}

TEST_F(FallbackFaceProviderTest, ForAnotherUiLanguageTheReaderFaceGoesBeforeTheCjkFaces) {
  ReaderFace reader({ZH_ONLY}, 12, 12);
  provider.setReaderFace(&ReaderFace::source, &reader);
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_ONLY)), reader.ink());
}

TEST_F(FallbackFaceProviderTest, ForACjkUiLanguageItsOwnFaceGoesFirstAndTheReaderFaceNext) {
  ReaderFace reader({ZH_ONLY, JA_ONLY}, 12, 12);
  provider.setReaderFace(&ReaderFace::source, &reader);
  provider.setReaderFaceAfterFirst(true);  // Chinese UI: its face is the first
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_ONLY)), faceBytes(cjk_ui_zh_10, ZH_ONLY));
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(JA_ONLY)), reader.ink());  // before the Japanese face
}

TEST_F(FallbackFaceProviderTest, WithoutAReaderFaceNowTheUiFacesServeAsBefore) {
  provider.setReaderFace(&noFace, nullptr);
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_ONLY)), faceBytes(cjk_ui_zh_10, ZH_ONLY));
  EXPECT_FALSE(EpdFont(&ui).getGlyph(GREEK_ALPHA));
}

TEST_F(FallbackFaceProviderTest, AReaderGlyphTooLargeToPackIsLeftToTheNextFace) {
  ReaderFace reader({ZH_ONLY}, 40, 40);  // 200 B of ink
  provider.setReaderFace(&ReaderFace::source, &reader);
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_ONLY)), faceBytes(cjk_ui_zh_10, ZH_ONLY));
}

TEST_F(FallbackFaceProviderTest, TextTheUiFaceCoversNeverAsksForTheReaderFace) {
  ReaderFace reader({GREEK_ALPHA}, 11, 14);
  provider.setReaderFace(&ReaderFace::source, &reader);
  for (const uint32_t cp : {static_cast<uint32_t>('A'), static_cast<uint32_t>('z'), 0xE9u}) {
    ASSERT_TRUE(EpdFont(&ui).getGlyph(cp));
  }
  EXPECT_EQ(reader.asked, 0);
}

// --- Per string (beginText): book text in one font, its glyphs prepared in one batch ---

std::string utf8(const uint32_t cp) {
  std::string out;
  if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
  } else {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  }
  out += static_cast<char>(0x80 | (cp & 0x3F));
  return out;
}

TEST_F(FallbackFaceProviderTest, ForACjkUiLanguageBookTextTakesTheReaderFaceForEveryCharacter) {
  ReaderFace reader({ZH_ONLY, JA_ONLY}, 12, 12);
  provider.setPrimaryFace(&inter_ui_10_regular);
  provider.setReaderFace(&ReaderFace::source, &reader);
  provider.setReaderFaceAfterFirst(true);  // Chinese UI
  // 書 is no Chinese UI face's: the string is book text, and 设, which the Chinese face has, comes from the book's
  // font.
  const std::string title = "A " + utf8(ZH_ONLY) + utf8(JA_ONLY);
  provider.beginText(title.c_str());
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_ONLY)), reader.ink());
  ASSERT_EQ(reader.prefetched.size(), 1u);
  EXPECT_EQ(reader.prefetched[0], utf8(ZH_ONLY) + utf8(JA_ONLY));  // what Inter lacks, in one batch
}

TEST_F(FallbackFaceProviderTest, ForACjkUiLanguageATranslationKeepsItsOwnFace) {
  ReaderFace reader({ZH_ONLY}, 12, 12);
  provider.setPrimaryFace(&inter_ui_10_regular);
  provider.setReaderFace(&ReaderFace::source, &reader);
  provider.setReaderFaceAfterFirst(true);
  const std::string string = utf8(ZH_ONLY) + utf8(ZH_AND_JA);  // every character the Chinese face's
  provider.beginText(string.c_str());
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(ZH_ONLY)), faceBytes(cjk_ui_zh_10, ZH_ONLY));
  EXPECT_TRUE(reader.prefetched.empty());
}

TEST_F(FallbackFaceProviderTest, TextTheUiFaceCoversPreparesNothing) {
  ReaderFace reader({GREEK_ALPHA}, 11, 14);
  provider.setPrimaryFace(&inter_ui_10_regular);
  provider.setReaderFace(&ReaderFace::source, &reader);
  provider.beginText("Caf\xC3\xA9 au lait");
  EXPECT_EQ(reader.asked, 0);
  EXPECT_TRUE(reader.prefetched.empty());
}

// --- glyph_miss::LendScope: a pass's misses borrow the secondary framebuffer once, until its return ---

int lendCalls = 0;
BuildArena* const kRegion = reinterpret_cast<BuildArena*>(0x1000);  // never dereferenced
BuildArena* countingLender() {
  ++lendCalls;
  return kRegion;
}

TEST(GlyphMissLend, TheFirstMissLendsAndTheReturnEndsItForTheRestOfThePass) {
  lendCalls = 0;
  {
    const glyph_miss::LendScope scope(&countingLender);
    EXPECT_EQ(glyph_miss::arena(), nullptr);  // nothing is lent before a miss asks
    EXPECT_EQ(lendCalls, 0);
    EXPECT_EQ(glyph_miss::lendingArena(), kRegion);
    EXPECT_EQ(glyph_miss::lendingArena(), kRegion);
    EXPECT_EQ(lendCalls, 1);
    glyph_miss::regionReturned(kRegion);  // the pass released the RenderLock: the display has it again
    EXPECT_EQ(glyph_miss::lendingArena(), nullptr);
    EXPECT_EQ(lendCalls, 1);
  }
  EXPECT_EQ(glyph_miss::arena(), nullptr);
}

TEST(GlyphMissLend, ARegionLentByItsOwnerIsUsedWithoutLending) {
  lendCalls = 0;
  BuildArena* const owners = reinterpret_cast<BuildArena*>(0x2000);
  const glyph_miss::Scope owner(owners);
  {
    const glyph_miss::LendScope scope(&countingLender);
    EXPECT_EQ(glyph_miss::lendingArena(), owners);
    EXPECT_EQ(lendCalls, 0);
  }
  EXPECT_EQ(glyph_miss::arena(), owners);
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

}  // namespace
