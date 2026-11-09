// The CJK UI packs (lib/EpdFont/scripts/build_ui_script_pack.py, src/CjkUiPack.cpp): the shipped .cpfont files hold
// exactly what CjkUiPacks.h says, in each region's forms and bit for bit as the cjk_ui_* faces in flash draw the
// characters both hold; and a UI face's CJK fallback (FallbackFaceProvider) draws a book title from them, in the region
// its kana or Hangul or the UI language name, before the last book's font, and as before when none is installed.
#include <Utf8.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "CjkUiPacks.h"
#include "CpfontFile.h"
#include "EpdFont.h"
#include "FallbackFaceProvider.h"
#include "FontDecompressor.h"
#include "SdCardFont.h"
#include "builtinFonts/cjk_ui_ja_10.h"
#include "builtinFonts/cjk_ui_ja_12.h"
#include "builtinFonts/cjk_ui_ko_10.h"
#include "builtinFonts/cjk_ui_zh_10.h"
#include "builtinFonts/inter_ui_10_regular.h"

namespace {

using cjk_ui_pack::Region;
using cpfont_file::Bytes;
using cpfont_file::FileFont;
using cpfont_file::readCpfont;

std::string packPath(const Region region, const unsigned points) {
  const std::string family = cjk_ui_pack::PACKS[static_cast<size_t>(region)].family;
  return std::string(SD_FONTS_DIR) + "/" + family + "/" + family + "_" + std::to_string(points) + ".cpfont";
}

std::vector<uint32_t> codepointsOf(const std::string& utf8) {
  std::vector<uint32_t> cps;
  const auto* p = reinterpret_cast<const uint8_t*>(utf8.c_str());
  while (const uint32_t cp = utf8NextCodepoint(&p)) cps.push_back(cp);
  return cps;
}

// The bitmap compressed `face` holds for `cp`, decoded.
Bytes flashBytes(const EpdFontData& face, const uint32_t cp) {
  static FontDecompressor decompressor;
  const EpdGlyphRef glyph = EpdFont(&face).findGlyph(cp);
  if (!glyph) return {};
  const uint8_t* bitmap = decompressor.getBitmap(&face, glyph, glyph.index);
  return bitmap ? Bytes(bitmap, bitmap + glyphDataBytes(glyph.width, glyph.height, false)) : Bytes{};
}

// What the renderer draws for a resolved glyph (GfxRenderer::getGlyphBitmap), copied out at once.
Bytes drawn(const EpdFontData& font, const EpdGlyphRef& glyph) {
  const uint8_t* bitmap = nullptr;
  if (font.provider && glyph.sdRecord && font.provider->ownsBitmap(glyph.sdRecord, bitmap)) {
    if (!bitmap) return {};
  } else {
    bitmap = &font.bitmap[epdGlyphBitmapOffset(&font, glyph)];
  }
  return Bytes(bitmap, bitmap + glyphDataBytes(glyph.width, glyph.height, font.is2Bit));
}

// src/CjkUiPack.cpp's hooks over packs loaded at 10 pt: the order rule of cjk_ui_pack, the installed packs in it.
struct Packs {
  std::unique_ptr<SdCardFont> fonts[cjk_ui_pack::REGION_COUNT];
  size_t uiRegion = cjk_ui_pack::REGION_COUNT;
  Region order[cjk_ui_pack::REGION_COUNT] = {};
  size_t orderCount = 0;
  int prepared = 0;

  void install(const Region region) {
    auto& font = fonts[static_cast<size_t>(region)];
    font = std::make_unique<SdCardFont>();
    ASSERT_TRUE(font->load(packPath(region, 10).c_str()));
  }
  bool installed(const Region region) const { return fonts[static_cast<size_t>(region)] != nullptr; }

  static const EpdFontData* face(void* ctx, const uint32_t cp) {
    auto* self = static_cast<Packs*>(ctx);
    for (size_t i = 0; i < self->orderCount; ++i) {
      if (cjk_ui_pack::covers(self->order[i], cp) && self->installed(self->order[i])) {
        return self->fonts[static_cast<size_t>(self->order[i])]->getEpdFont(0)->data;
      }
    }
    return nullptr;
  }
  static bool covers(void* ctx, const uint32_t cp) {
    const auto* self = static_cast<Packs*>(ctx);
    for (size_t r = 0; r < cjk_ui_pack::REGION_COUNT; ++r) {
      if (cjk_ui_pack::covers(static_cast<Region>(r), cp) && self->installed(static_cast<Region>(r))) return true;
    }
    return false;
  }
  static void prepare(void* ctx, const char* utf8Text) {
    auto* self = static_cast<Packs*>(ctx);
    ++self->prepared;
    bool kana = false, hangul = false;
    for (const uint32_t cp : codepointsOf(utf8Text)) {
      kana = kana || cjk_ui_pack::isKana(cp);
      hangul = hangul || cjk_ui_pack::isHangul(cp);
    }
    self->orderCount = cjk_ui_pack::order(kana, hangul, self->uiRegion, self->order);
  }
  FallbackFaceProvider::Packs hooks() {
    FallbackFaceProvider::Packs p;
    p.face = &face;
    p.covers = &covers;
    p.prepare = &prepare;
    p.ctx = this;
    return p;
  }
};

int readerAsks = 0;
const EpdFontData* countingReaderFace(void* /*ctx*/) {
  ++readerAsks;
  return nullptr;
}

bool claimsCjk(const uint32_t cp) { return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3040 && cp <= 0x30FF); }

// 吾輩は猫である, 微风 (Chinese), 한국어: book titles
const std::string JAPANESE = "\xE5\x90\xBE\xE8\xBC\xA9\xE3\x81\xAF\xE7\x8C\xAB\xE3\x81\xA7\xE3\x81\x82\xE3\x82\x8B";
const std::string CHINESE = "\xE5\xBE\xAE\xE9\xA3\x8E";
const std::string KOREAN = "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4";

class CjkUiPackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    provider.setDecompressor(&decompressor);
    const EpdFontData* faces[] = {&cjk_ui_zh_10, &cjk_ui_ja_10, &cjk_ui_ko_10};
    ASSERT_TRUE(provider.setFaces(faces, 3));
    provider.setPrimaryFace(&inter_ui_10_regular);
    provider.setReaderFace(&countingReaderFace, nullptr);
    provider.setStandIn(0x25A1, &claimsCjk);
    ui = inter_ui_10_regular;
    ui.provider = &provider;
    readerAsks = 0;
  }

  // Draws `title` through the UI face and expects every character of it the packs hold from `region`'s file.
  void expectDrawnFrom(const std::string& title, const Region region) {
    const FileFont file = readCpfont(packPath(region, 10));
    provider.beginText(title.c_str());
    for (const uint32_t cp : codepointsOf(title)) {
      const EpdGlyphRef glyph = EpdFont(&ui).getGlyph(cp);
      ASSERT_TRUE(glyph) << std::hex << cp;
      EXPECT_EQ(drawn(ui, glyph), file.styles[0].bitmapOf(cp)) << std::hex << cp;
    }
  }

  FontDecompressor decompressor;
  FallbackFaceProvider provider;
  Packs packs;
  EpdFontData ui{};
};

}  // namespace

TEST(CjkUiPackFiles, HoldExactlyWhatTheUiLooksUpThere) {
  for (const auto& pack : cjk_ui_pack::PACKS) {
    std::set<uint32_t> expected;
    for (size_t i = 0; i < pack.rangeCount; ++i) {
      for (uint32_t cp = pack.ranges[i].first; cp <= pack.ranges[i].last; ++cp) expected.insert(cp);
    }
    for (const uint8_t points : cjk_ui_pack::SIZES) {
      const FileFont font = readCpfont(packPath(pack.region, points));
      EXPECT_EQ(font.flags & 1, 0) << pack.family << " " << int{points} << " pt is not 1-bit";
      ASSERT_EQ(font.styleIds, std::vector<uint8_t>{0}) << pack.family << " " << int{points};
      const auto& cps = font.styles[0].codepoints;
      EXPECT_EQ(std::set<uint32_t>(cps.begin(), cps.end()), expected) << pack.family << " " << int{points};
    }
  }
}

TEST(CjkUiPackFiles, DrawWhatTheFlashFacesHoldBitForBit) {
  // Same source, same sizes, same pipeline: a character both hold is the same glyph, so a title mixing a
  // translation's characters and the pack's draws as one face.
  const struct {
    const EpdFontData* face;
    Region region;
    unsigned points;
  } pairs[] = {{&cjk_ui_ja_10, Region::Japanese, 10},
               {&cjk_ui_ja_12, Region::Japanese, 12},
               {&cjk_ui_zh_10, Region::SimplifiedChinese, 10},
               {&cjk_ui_ko_10, Region::Korean, 10}};
  for (const auto& pair : pairs) {
    const FileFont file = readCpfont(packPath(pair.region, pair.points));
    size_t compared = 0;
    for (uint32_t i = 0; i < pair.face->intervalCount; ++i) {
      const EpdUnicodeInterval& iv = pair.face->intervals[i];
      for (uint32_t cp = iv.first; cp <= iv.last; ++cp) {
        if (!cjk_ui_pack::covers(pair.region, cp) || cp == 0x3000) continue;
        EXPECT_EQ(flashBytes(*pair.face, cp), file.styles[0].bitmapOf(cp)) << std::hex << cp;
        ++compared;
      }
    }
    EXPECT_GT(compared, 100u);
  }
}

TEST(CjkUiPackOrder, KanaMakesItJapaneseHangulKoreanElseTheUiLanguageThenAFixedOrder) {
  Region order[cjk_ui_pack::REGION_COUNT];
  ASSERT_EQ(cjk_ui_pack::order(true, false, cjk_ui_pack::REGION_COUNT, order), cjk_ui_pack::REGION_COUNT);
  EXPECT_EQ(order[0], Region::Japanese);
  EXPECT_EQ(order[1], Region::SimplifiedChinese);
  cjk_ui_pack::order(false, true, static_cast<size_t>(Region::Japanese), order);
  EXPECT_EQ(order[0], Region::Korean);
  EXPECT_EQ(order[1], Region::Japanese);
  cjk_ui_pack::order(false, false, static_cast<size_t>(Region::TraditionalChinese), order);
  EXPECT_EQ(order[0], Region::TraditionalChinese);
  EXPECT_EQ(order[1], Region::SimplifiedChinese);
  cjk_ui_pack::order(false, false, cjk_ui_pack::REGION_COUNT, order);
  EXPECT_EQ(order[0], Region::SimplifiedChinese);
  EXPECT_EQ(order[3], Region::Korean);
}

TEST_F(CjkUiPackTest, AJapaneseTitleDrawsItsKanjiInJapaneseFormsThoughChineseIsInstalledToo) {
  packs.install(Region::SimplifiedChinese);
  packs.install(Region::Japanese);
  provider.setPacks(packs.hooks());
  expectDrawnFrom(JAPANESE, Region::Japanese);
  EXPECT_EQ(readerAsks, 0);  // the last book's font is not opened for what the packs draw
}

TEST_F(CjkUiPackTest, AChineseTitleTakesTheFixedOrdersFirstPackAndTheUiLanguagesWhenItHasOne) {
  packs.install(Region::SimplifiedChinese);
  packs.install(Region::Japanese);
  provider.setPacks(packs.hooks());
  expectDrawnFrom(CHINESE, Region::SimplifiedChinese);
  packs.uiRegion = static_cast<size_t>(Region::Japanese);
  expectDrawnFrom(CHINESE, Region::Japanese);
}

TEST_F(CjkUiPackTest, AKoreanTitleDrawsFromTheKoreanPack) {
  packs.install(Region::Korean);
  provider.setPacks(packs.hooks());
  expectDrawnFrom(KOREAN, Region::Korean);
}

TEST_F(CjkUiPackTest, WithoutAPackTitlesDrawAsBefore) {
  provider.setPacks(packs.hooks());                      // none installed
  const std::string title = "\xE9\xBE\x98\xE8\xAE\xBE";  // 龘, in no flash face; 设, a Chinese UI string's
  provider.beginText(title.c_str());
  EXPECT_GT(readerAsks, 0);  // book text: the last book's font is asked, as before
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(0x9F98)), flashBytes(cjk_ui_zh_10, 0x25A1));  // the stand-in box
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(0x8BBE)), flashBytes(cjk_ui_zh_10, 0x8BBE));
}

TEST_F(CjkUiPackTest, ACharacterNoFlashFaceHoldsDrawsFromThePackInsteadOfTheBox) {
  packs.install(Region::SimplifiedChinese);
  provider.setPacks(packs.hooks());
  expectDrawnFrom("\xE9\xBE\x98", Region::SimplifiedChinese);  // 龘
}

TEST_F(CjkUiPackTest, ATranslationInACjkUiLanguageStaysWithItsOwnFaceAndReadsNothing) {
  packs.install(Region::Japanese);
  provider.setPacks(packs.hooks());
  provider.setReaderFaceAfterFirst(true);          // a Chinese UI: its own face first
  const std::string translation = "\xE8\xAE\xBE";  // 设, which the Chinese face holds
  provider.beginText(translation.c_str());
  EXPECT_EQ(packs.prepared, 0);
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph(0x8BBE)), flashBytes(cjk_ui_zh_10, 0x8BBE));
}
