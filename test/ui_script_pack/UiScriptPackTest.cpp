// The Arabic UI pack (lib/EpdFont/scripts/build_ui_script_pack.py, src/UiScriptPack.cpp): the shipped .cpfont files
// hold exactly what the UI looks up there, and a built-in UI face draws a formed Arabic title from them through its
// UiFaceProvider, at its own size and weight, both before and after the string's prewarm; without the pack, Arabic
// falls to what served it before.
#include <Utf8.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "ArabicUiPack.h"
#include "BidiUtils.h"
#include "EpdFont.h"
#include "FallbackFaceProvider.h"
#include "SdCardFont.h"
#include "UiFaceProvider.h"
#include "builtinFonts/inter_ui_10_bold.h"
#include "builtinFonts/inter_ui_10_regular.h"

namespace {

using Bytes = std::vector<uint8_t>;

std::string packPath(const unsigned points) {
  return std::string(ARABIC_UI_DIR) + "/ArabicUI_" + std::to_string(points) + ".cpfont";
}

Bytes readFile(const std::string& path) {
  Bytes bytes;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    uint8_t buffer[4096];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
    std::fclose(f);
  }
  return bytes;
}

uint32_t u32(const Bytes& b, const size_t at) {
  return b[at] | b[at + 1] << 8 | b[at + 2] << 16 | uint32_t{b[at + 3]} << 24;
}
uint16_t u16(const Bytes& b, const size_t at) { return static_cast<uint16_t>(b[at] | b[at + 1] << 8); }

// One style of a v4 .cpfont, read straight from the file: what the generator wrote.
struct FileStyle {
  std::vector<uint32_t> codepoints;  // in glyph order
  std::vector<EpdGlyph> glyphs;
  Bytes bitmaps;

  Bytes bitmapOf(const uint32_t cp, EpdGlyph* record = nullptr) const {
    for (size_t i = 0; i < codepoints.size(); ++i) {
      if (codepoints[i] != cp) continue;
      if (record) *record = glyphs[i];
      return Bytes(bitmaps.begin() + glyphs[i].dataOffset,
                   bitmaps.begin() + glyphs[i].dataOffset + glyphs[i].dataLength);
    }
    return {};
  }
};

struct FileFont {
  uint16_t flags = 0;
  std::vector<uint8_t> styleIds;
  std::vector<FileStyle> styles;
};

FileFont readCpfont(const std::string& path) {
  const Bytes b = readFile(path);
  FileFont font;
  if (b.size() < 32) return font;
  font.flags = u16(b, 10);
  const uint8_t count = b[12];
  for (uint8_t s = 0; s < count; ++s) {
    const size_t toc = 32 + s * 32;
    const uint32_t intervals = u32(b, toc + 4);
    const uint32_t glyphs = u32(b, toc + 8);
    const uint16_t kernLeft = u16(b, toc + 17), kernRight = u16(b, toc + 19);
    const uint8_t leftClasses = b[toc + 21], rightClasses = b[toc + 22], ligatures = b[toc + 23];
    const uint32_t data = u32(b, toc + 24);
    FileStyle style;
    for (uint32_t i = 0; i < intervals; ++i) {
      const uint32_t first = u32(b, data + i * 12), last = u32(b, data + i * 12 + 4);
      for (uint32_t cp = first; cp <= last; ++cp) style.codepoints.push_back(cp);
    }
    const size_t glyphAt = data + intervals * 12;
    for (uint32_t i = 0; i < glyphs; ++i) {
      EpdGlyph g{};
      std::memcpy(&g, &b[glyphAt + i * 16], sizeof(EpdGlyph));
      style.glyphs.push_back(g);
    }
    const size_t bitmapAt =
        glyphAt + glyphs * 16 + (kernLeft + kernRight) * 3 + leftClasses * rightClasses + ligatures * 8;
    style.bitmaps.assign(b.begin() + bitmapAt, b.end());
    font.styleIds.push_back(b[toc]);
    font.styles.push_back(std::move(style));
  }
  return font;
}

std::vector<uint32_t> codepointsOf(const std::string& utf8) {
  std::vector<uint32_t> cps;
  const auto* p = reinterpret_cast<const uint8_t*>(utf8.c_str());
  while (const uint32_t cp = utf8NextCodepoint(&p)) cps.push_back(cp);
  return cps;
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

// src/UiScriptPack.cpp's hooks over one loaded file and style.
struct PackHooks {
  SdCardFont* font = nullptr;
  uint8_t style = 0;
  bool installed = true;
  int prepared = 0;
};

const EpdFontData* hookFace(void* ctx) {
  const auto* h = static_cast<PackHooks*>(ctx);
  return h->installed ? h->font->getEpdFont(h->style)->data : nullptr;
}
bool hookInstalled(void* ctx) { return static_cast<PackHooks*>(ctx)->installed; }
void hookPrepare(void* ctx, const char* utf8Text) {
  auto* h = static_cast<PackHooks*>(ctx);
  if (!h->installed) return;
  std::string wanted;
  for (const uint32_t cp : codepointsOf(utf8Text)) {
    if (arabic_ui_pack::covers(cp)) utf8AppendCodepoint(cp, wanted);
  }
  if (wanted.empty()) return;
  ++h->prepared;
  h->font->prewarm(wanted.c_str(), static_cast<uint8_t>(1u << h->style));
}

UiFaceProvider::Pack packOf(PackHooks& hooks) {
  UiFaceProvider::Pack pack;
  pack.face = &hookFace;
  pack.covers = &arabic_ui_pack::covers;
  pack.installed = &hookInstalled;
  pack.prepare = &hookPrepare;
  pack.ctx = &hooks;
  return pack;
}

// A book font behind the UI faces that counts how often it is asked for: FallbackFaceProvider's reader face hook.
int readerAsks = 0;
const EpdFontData* countingReaderFace(void* /*ctx*/) {
  ++readerAsks;
  return nullptr;
}

// السلام عليكم, formed as the UI forms it.
const std::string TITLE = "\xD8\xA7\xD9\x84\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xD8\xB9\xD9\x84\xD9\x8A\xD9\x83\xD9\x85";

class UiScriptPackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(pack.load(packPath(10).c_str()));
    file = readCpfont(packPath(10));
    ASSERT_EQ(file.styles.size(), 2u);
    regularHooks = PackHooks{&pack, 0};
    boldHooks = PackHooks{&pack, 1};
    regular.bind(&shared, packOf(regularHooks));
    bold.bind(&shared, packOf(boldHooks));
    ui = inter_ui_10_regular;
    ui.provider = &regular;
    uiBold = inter_ui_10_bold;
    uiBold.provider = &bold;
  }

  void expectDrawnFromThePack(const EpdFontData& face, const FileStyle& style) {
    for (const uint32_t cp : codepointsOf(BidiUtils::arabicUiForms(TITLE))) {
      if (cp == ' ') continue;
      const EpdGlyphRef glyph = EpdFont(&face).getGlyph(cp);
      ASSERT_TRUE(glyph) << std::hex << cp;
      EpdGlyph record{};
      const Bytes expected = style.bitmapOf(cp, &record);
      ASSERT_FALSE(expected.empty()) << std::hex << cp;
      EXPECT_EQ(glyph.width, record.width) << std::hex << cp;
      EXPECT_EQ(glyph.height, record.height) << std::hex << cp;
      EXPECT_EQ(glyph.advanceX, record.advanceX) << std::hex << cp;
      EXPECT_EQ(drawn(face, glyph), expected) << std::hex << cp;
    }
  }

  SdCardFont pack;
  FileFont file;
  FallbackFaceProvider shared;
  PackHooks regularHooks, boldHooks;
  UiFaceProvider regular, bold;
  EpdFontData ui{}, uiBold{};
};

}  // namespace

TEST(UiScriptPackFiles, HoldExactlyWhatTheUiLooksUpThereAtEverySize) {
  std::set<uint32_t> expected;
  for (const auto& range : arabic_ui_pack::RANGES) {
    for (uint32_t cp = range.first; cp <= range.last; ++cp) expected.insert(cp);
  }
  for (const unsigned points : {6u, 8u, 10u, 12u, 14u}) {
    const FileFont font = readCpfont(packPath(points));
    EXPECT_EQ(font.flags & 1, 0) << points << " pt is not 1-bit";
    EXPECT_EQ(font.styleIds, points == 6 ? (std::vector<uint8_t>{0}) : (std::vector<uint8_t>{0, 1})) << points;
    for (const FileStyle& style : font.styles) {
      EXPECT_EQ(std::set<uint32_t>(style.codepoints.begin(), style.codepoints.end()), expected) << points << " pt";
    }
  }
}

TEST(UiScriptPackFiles, CoverEveryFormTheMapperEmits) {
  // Every Arabic letter with a joining type, in every position: what arabicUiForms() writes is the pack's or a letter
  // with no form at all, which stays in the Arabic block.
  for (uint32_t letter = 0x0620; letter <= 0x06FF; ++letter) {
    std::string word;
    for (int i = 0; i < 3; ++i) utf8AppendCodepoint(letter, word);
    for (const uint32_t cp : codepointsOf(BidiUtils::arabicUiForms(word))) {
      EXPECT_TRUE(arabic_ui_pack::covers(cp) || (cp >= 0x0600 && cp <= 0x06FF)) << std::hex << cp;
    }
  }
}

TEST_F(UiScriptPackTest, AFormedTitleDrawsFromThePackGlyphByGlyph) {
  // No prewarm: each glyph through the font's miss ring.
  expectDrawnFromThePack(ui, file.styles[0]);
}

TEST_F(UiScriptPackTest, AFormedTitleDrawsFromThePackAfterItsPrewarm) {
  const std::string formed = BidiUtils::arabicUiForms(TITLE);
  ui.provider->beginText(formed.c_str());
  EXPECT_EQ(regularHooks.prepared, 1);
  expectDrawnFromThePack(ui, file.styles[0]);
}

TEST_F(UiScriptPackTest, ABoldFaceDrawsThePacksBold) {
  const std::string formed = BidiUtils::arabicUiForms(TITLE);
  uiBold.provider->beginText(formed.c_str());
  expectDrawnFromThePack(uiBold, file.styles[1]);
  EXPECT_NE(file.styles[0].bitmapOf(0xFEFB), file.styles[1].bitmapOf(0xFEFB));
}

TEST_F(UiScriptPackTest, LatinStaysWithTheUiFaceAndPreparesNothing) {
  ui.provider->beginText("Pride and Prejudice");
  EXPECT_EQ(regularHooks.prepared, 0);
  EXPECT_EQ(drawn(ui, EpdFont(&ui).getGlyph('P')),
            drawn(inter_ui_10_regular, EpdFont(&inter_ui_10_regular).getGlyph('P')));
}

TEST_F(UiScriptPackTest, WithThePackArabicIsFormedAndIsNoBookText) {
  EXPECT_TRUE(ui.provider->formsArabic());
  shared.setReaderFace(&countingReaderFace, nullptr);
  readerAsks = 0;
  const std::string formed = BidiUtils::arabicUiForms(TITLE) + " (2nd ed.)";
  ui.provider->beginText(formed.c_str());
  EXPECT_EQ(readerAsks, 0);  // the last book's font is not opened for a title the pack draws
}

TEST_F(UiScriptPackTest, WithoutThePackArabicFallsToWhatServedItBefore) {
  regularHooks.installed = false;
  EXPECT_FALSE(ui.provider->formsArabic());  // the letters as they are, as before the pack
  shared.setReaderFace(&countingReaderFace, nullptr);
  readerAsks = 0;
  ui.provider->beginText(TITLE.c_str());
  EXPECT_GT(readerAsks, 0);  // book text: the last book's font goes first, as before
  EXPECT_EQ(regularHooks.prepared, 0);
  EXPECT_FALSE(EpdFont(&ui).getGlyph(0x0644));  // no book font: nothing draws it (Inter has no U+FFFD)
  EXPECT_FALSE(EpdFont(&ui).getGlyph(0xFEFB));
}
