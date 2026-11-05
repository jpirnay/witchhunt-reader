// FontCacheManager::prewarmCache finds a page's font through the renderer's lookup, so a face the font resolver
// makes on demand (a TTF family's native size) is prewarmed even before anything looked it up.
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "EpdFont.h"
#include "EpdFontFamily.h"
#include "FontCacheManager.h"
#include "GlyphProvider.h"
#include "GlyphRun.h"

namespace {

struct CountingProvider final : GlyphProvider {
  int prefetches = 0;
  std::string text;
  const EpdGlyph* missGlyph(uint32_t) override { return nullptr; }
  void prefetch(const char* utf8Text) override {
    ++prefetches;
    text = utf8Text;
  }
};

struct Fixture : testing::Test {
  CountingProvider provider;
  EpdFontData data{};
  EpdFont font{&data};
  EpdFontFamily family{&font, nullptr, nullptr, nullptr};
  std::map<int, EpdFontFamily> fontMap;
  const std::map<int, SdCardFont*> noSdFonts;
  FontCacheManager manager{fontMap, noSdFonts, noSdFonts};
  int asked = 0;

  void SetUp() override { data.provider = &provider; }
};

}  // namespace

TEST_F(Fixture, AFontOnlyTheLookupSuppliesIsPrewarmed) {
  manager.setFontLookup(
      [](const void* ctx, const int fontId) -> const EpdFontFamily* {
        auto* self = static_cast<Fixture*>(const_cast<void*>(ctx));
        ++self->asked;
        return fontId == 4711 ? &self->family : nullptr;
      },
      this);
  manager.prewarmCache(4711, "Chapter One", 0x01);
  EXPECT_EQ(1, asked);
  EXPECT_EQ(1, provider.prefetches);
  EXPECT_EQ("Chapter One", provider.text);
  manager.prewarmCache(4712, "nothing", 0x01);  // a font nobody knows: nothing to do
  EXPECT_EQ(1, provider.prefetches);
}

TEST_F(Fixture, WithoutALookupTheFontMapServes) {
  fontMap.emplace(7, family);
  manager.prewarmCache(7, "Body", 0x01);
  EXPECT_EQ(1, provider.prefetches);
  manager.prewarmCache(4711, "Chapter One", 0x01);
  EXPECT_EQ(1, provider.prefetches);
}

// Small caps draw a lowercase letter with its capital's glyph, so the page prewarm must warm the
// capitals -- and not the lowercase glyphs, which are never drawn.
TEST_F(Fixture, SmallCapsTextIsPrewarmedAsTheCapitalsItDraws) {
  fontMap.emplace(7, family);
  {
    auto scope = manager.createPrewarmScope();
    manager.recordText("Łódź fine ß", 7, EpdFontFamily::SMALL_CAPS);
    manager.recordText(" plain", 7, EpdFontFamily::REGULAR);
    scope.endScanAndPrewarm();
  }
  EXPECT_EQ(1, provider.prefetches);
  EXPECT_EQ("ŁÓDŹ FINE ß plain", provider.text);  // ß has no single capital and is drawn as is
}

// Folding never lengthens the text (ı becomes a one-byte I), and bytes that do not decode are
// copied as they are, so the folded text never outgrows the scan's cap.
TEST_F(Fixture, SmallCapsFoldKeepsUndecodableBytes) {
  fontMap.emplace(7, family);
  {
    auto scope = manager.createPrewarmScope();
    manager.recordText("ı\x80z", 7, EpdFontFamily::SMALL_CAPS);
    scope.endScanAndPrewarm();
  }
  EXPECT_EQ("I\x80Z", provider.text);
}

// The scan keeps at most 4096 bytes per style. A letter the cap cuts in two is left out whole.
TEST_F(Fixture, SmallCapsFoldStopsAtTheScanCap) {
  fontMap.emplace(7, family);
  std::string polish;
  for (int i = 0; i < 2500; i++) polish += "ł";  // 5000 bytes
  {
    auto scope = manager.createPrewarmScope();
    manager.recordText("a", 7, EpdFontFamily::SMALL_CAPS);  // 4095 bytes left: an odd number
    manager.recordText(polish.c_str(), 7, EpdFontFamily::SMALL_CAPS);
    scope.endScanAndPrewarm();
  }
  std::string expected = "A";
  for (int i = 0; i < 2047; i++) expected += "Ł";
  EXPECT_EQ(expected, provider.text);
}

// --- The glyph-ID scan: a shaped run's glyphs, prewarmed by ID ---

namespace {

// A TrueType-like provider: it draws glyph keys, and counts the glyph-ID prefetches it is asked for.
struct GlyphKeyProvider final : GlyphProvider {
  int prefetches = 0;
  std::vector<uint16_t> glyphs;
  const EpdGlyph* missGlyph(uint32_t) override { return nullptr; }
  bool drawsGlyphKeys() const override { return true; }
  void prefetchGlyphs(const uint16_t* ids, const size_t count) override {
    ++prefetches;
    glyphs.assign(ids, ids + count);
  }
};

// A run's records (GlyphRun.h), every third glyph with an offset.
struct EncodedRun {
  std::vector<uint8_t> bytes;
  uint16_t count = 0;
  GlyphRun run() const { return {bytes.data(), count, static_cast<uint16_t>(bytes.size())}; }
};

EncodedRun encodedRun(const std::vector<uint16_t>& glyphs) {
  EncodedRun out;
  out.bytes.resize(glyphs.size() * 6);
  size_t at = 0;
  for (size_t i = 0; i < glyphs.size(); ++i) {
    GlyphRecord record;
    record.glyph = glyphs[i];
    record.advance = 160;
    record.dx = i % 3 == 2 ? -3 : 0;
    size_t written = 0;
    EXPECT_TRUE(glyph_run::encode(record, out.bytes.data() + at, out.bytes.size() - at, &written));
    at += written;
  }
  out.bytes.resize(at);
  out.count = static_cast<uint16_t>(glyphs.size());
  return out;
}

// A family whose regular and bold fonts each draw glyph keys, under font ID 7.
struct GlyphScan {
  GlyphKeyProvider regularProvider;
  GlyphKeyProvider boldProvider;
  EpdFontData regularData{};
  EpdFontData boldData{};
  EpdFont regular{&regularData};
  EpdFont bold{&boldData};
  std::map<int, EpdFontFamily> fontMap;
  const std::map<int, SdCardFont*> noSdFonts;
  FontCacheManager manager{fontMap, noSdFonts, noSdFonts};

  GlyphScan() {
    regularData.provider = &regularProvider;
    boldData.provider = &boldProvider;
    fontMap.emplace(7, EpdFontFamily(&regular, &bold));
  }
};

}  // namespace

TEST(FontCacheManager, AScannedGlyphRunIsPrewarmedByGlyphId) {
  GlyphScan scan;
  const EncodedRun word = encodedRun({200, 26, 513});
  {
    auto scope = scan.manager.createPrewarmScope();
    scan.manager.recordGlyphs(word.run(), 7, EpdFontFamily::BOLD);
    EXPECT_EQ(0, scan.boldProvider.prefetches);  // the scan only records
    scope.endScanAndPrewarm();
  }
  EXPECT_EQ(1, scan.boldProvider.prefetches);
  EXPECT_EQ((std::vector<uint16_t>{200, 26, 513}), scan.boldProvider.glyphs);
  EXPECT_EQ(0, scan.regularProvider.prefetches);
}

TEST(FontCacheManager, AGlyphIsHeldOncePerStyleHoweverOftenItIsDrawn) {
  GlyphScan scan;
  const EncodedRun word = encodedRun({200, 26, 513});
  {
    auto scope = scan.manager.createPrewarmScope();
    for (int i = 0; i < 200; ++i) scan.manager.recordGlyphs(word.run(), 7, EpdFontFamily::REGULAR);
    scope.endScanAndPrewarm();
  }
  EXPECT_EQ(1, scan.regularProvider.prefetches);
  EXPECT_EQ((std::vector<uint16_t>{200, 26, 513}), scan.regularProvider.glyphs);
}

// A font that draws no glyph keys is not scanned for IDs: its provider is never asked for any.
TEST(FontCacheManager, AFontThatDrawsNoGlyphKeysRecordsNoIds) {
  GlyphScan scan;
  CountingProvider plain;
  EpdFontData plainData{};
  plainData.provider = &plain;
  const EpdFont plainFont(&plainData);
  scan.fontMap.emplace(8, EpdFontFamily(&plainFont));
  const EncodedRun word = encodedRun({200, 26, 513});
  {
    auto scope = scan.manager.createPrewarmScope();
    scan.manager.recordGlyphs(word.run(), 8, EpdFontFamily::REGULAR);
    scope.endScanAndPrewarm();
  }
  EXPECT_EQ(0, plain.prefetches);
  EXPECT_EQ(0, scan.regularProvider.prefetches);
}

// The scan keeps 512 distinct IDs a style and hands them to the provider in one call; IDs past them are looked up as
// they are drawn. What the provider renders of the 512 is its own cap: a TrueType font renders the first 64 it lacks
// and leaves the rest to the draw (TtfGlyphKeys.APrefetchRendersAtMostSixtyFourNewGlyphs).
TEST(FontCacheManager, GlyphIdsPastTheCapAreLeftToTheDraw) {
  GlyphScan scan;
  std::vector<uint16_t> first;
  std::vector<uint16_t> second;
  for (uint16_t id = 1; id <= 300; ++id) first.push_back(id);
  for (uint16_t id = 301; id <= 600; ++id) second.push_back(id);
  const EncodedRun a = encodedRun(first);
  const EncodedRun b = encodedRun(second);
  {
    auto scope = scan.manager.createPrewarmScope();
    scan.manager.recordGlyphs(a.run(), 7, EpdFontFamily::REGULAR);
    scan.manager.recordGlyphs(b.run(), 7, EpdFontFamily::REGULAR);
    scope.endScanAndPrewarm();
  }
  ASSERT_EQ(1, scan.regularProvider.prefetches);
  ASSERT_EQ(512u, scan.regularProvider.glyphs.size());
  for (uint16_t i = 0; i < 512; ++i) EXPECT_EQ(i + 1, scan.regularProvider.glyphs[i]) << i;
}
