// TrueType faces that shape (writing modes step 2): what the install writes for the shaper, each style block's CPac.
#include <OtFace.h>
#include <OtFilterWriter.h>
#include <OtShaper.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "EpdFontFamily.h"
#include "ExpectedGlyphRuns.h"
#include "FixtureShapingFace.h"
#include "GlyphKey.h"
#include "GlyphProvider.h"
#include "GlyphRun.h"
#include "GlyphStoreFormat.h"
#include "MovingNorFlash.h"
#include "OtShaperTestHelpers.h"
#include "ShapingFace.h"
#include "SimNorFlash.h"
#include "TtfBuilder.h"
#include "TtfFamily.h"
#include "TtfFont.h"
#include "TtfFontTestUtil.h"
#include "TtfLayoutBuilder.h"
#include "TtfShaping.h"
#include "TtfTestUtil.h"

using namespace ttf_test;

namespace {

constexpr uint32_t kPage = glyphstore::kPageBytes;
constexpr uint32_t kSector = glyphstore::kSectorBytes;
constexpr uint32_t kFlashBytes = 3538944 - 65536;  // the X3's font area: spiffs below the language slot
constexpr uint8_t kRegular = 0;
constexpr uint8_t kBold = 1;
constexpr uint8_t kIdentityA[glyphstore::kIdentityBytes] = {'s', 'h', 'a', 'p', 'e', '-', 'A'};
constexpr uint8_t kIdentityB[glyphstore::kIdentityBytes] = {'s', 'h', 'a', 'p', 'e', '-', 'B'};
// Code points of the Devanagari subsets: KA, HA and PA.
constexpr uint32_t kDevanagari[] = {0x0915, 0x0939, 0x092A};

// The families (each four styles' files, by style) loaded as one set on `chip`, each at 14 pt. Null when it fails.
std::unique_ptr<TtfFontSet> load(NorFlash& chip, const std::vector<MemoryFile*>& families, const uint8_t* identity,
                                 int& busy) {
  auto set = std::make_unique<TtfFontSet>(chip);
  std::vector<std::array<ttf::FileSource, 4>> sources(families.size());
  for (size_t f = 0; f < families.size(); ++f) {
    for (int s = 0; s < 4; ++s) {
      if (!families[f][s].bytes.empty()) sources[f][s] = families[f][s].source();
    }
  }
  static_assert(sizeof(std::array<ttf::FileSource, 4>) == sizeof(TtfFontSet::Files), "one family's files");
  if (!set->load(reinterpret_cast<const TtfFontSet::Files*>(sources.data()), static_cast<uint8_t>(families.size()),
                 identity, [&busy] { ++busy; })) {
    return nullptr;
  }
  for (uint8_t f = 0; f < families.size(); ++f) {
    if (!set->font(f)->setBodySize(14)) return nullptr;
  }
  return set;
}

glyphstore::Header headerOf(const SimNorFlash& chip) {
  glyphstore::Header header{};
  EXPECT_TRUE(chip.read(0, &header, sizeof header));
  return header;
}

std::vector<uint8_t> chipBytes(const SimNorFlash& chip, const uint32_t offset, const uint32_t length) {
  std::vector<uint8_t> out(length);
  EXPECT_TRUE(chip.read(offset, out.data(), length));
  return out;
}

bool erased(const std::vector<uint8_t>& bytes) {
  return std::all_of(bytes.begin(), bytes.end(), [](const uint8_t b) { return b == 0xFF; });
}

// What ot::FilterTableWriter writes over the tables of `font`, a file or a block, its CPac and CPpl records left out.
std::vector<uint8_t> writerTable(const std::vector<uint8_t>& font) {
  ot::FaceTables tables;
  EXPECT_TRUE(ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables));
  tables[ot::FaceTables::CPAC] = ot::Table();
  tables[ot::FaceTables::CPPL] = ot::Table();
  ot::Face face;
  EXPECT_TRUE(face.init(tables));
  ot::FilterTableWriter writer(face);
  std::vector<uint8_t> out(writer.bytes());
  EXPECT_EQ(writer.bytes(), writer.write(out.data(), writer.bytes()));
  return out;
}

// One record of a table directory, a file's or a block's.
struct DirRecord {
  bool found = false;
  uint32_t offset = 0;
  uint32_t length = 0;
};

uint32_t read32(const std::vector<uint8_t>& bytes, const size_t at) {
  return static_cast<uint32_t>(bytes[at]) << 24 | static_cast<uint32_t>(bytes[at + 1]) << 16 |
         static_cast<uint32_t>(bytes[at + 2]) << 8 | bytes[at + 3];
}

DirRecord recordOf(const std::vector<uint8_t>& font, const char* name) {
  const size_t at = findRecord(font, name);
  if (at == 0) return {};
  return {true, read32(font, at + 8), read32(font, at + 12)};
}

// A flash-mode block's CPac room, from align4(fileSize) to the block's end: `table`, then erased flash.
void expectRoomHolds(const SimNorFlash& chip, const glyphstore::StyleInfo& style, const std::vector<uint8_t>& table) {
  const glyphstore::CpacRoom room = glyphstore::cpacRoomOf(style.fileSize, style.size);
  ASSERT_GE(room.bytes, table.size());
  EXPECT_EQ(table, chipBytes(chip, style.offset + room.at, static_cast<uint32_t>(table.size())));
  EXPECT_TRUE(erased(chipBytes(chip, style.offset + room.at + static_cast<uint32_t>(table.size()),
                               room.bytes - static_cast<uint32_t>(table.size()))));
}

// Whether an ot::Face over a flash-mode block, its file's tables and the CPac in its room, takes the CPac's filters.
bool cpacAccepted(const SimNorFlash& chip, const glyphstore::StyleInfo& style) {
  const std::vector<uint8_t> block = chipBytes(chip, style.offset, style.size);
  ot::FaceTables tables;
  EXPECT_TRUE(ot::tablesFromSfnt(block.data(), style.fileSize, &tables));
  const glyphstore::CpacRoom room = glyphstore::cpacRoomOf(style.fileSize, style.size);
  tables[ot::FaceTables::CPAC] = ot::Table(block.data() + room.at, room.bytes);
  ot::Face face;
  EXPECT_TRUE(face.init(tables));
  return face.filters(ot::GSUB) != nullptr && face.filters(ot::GPOS) != nullptr;
}

uint32_t erasesBetween(const SimNorFlash& chip, const uint32_t from, const uint32_t to) {
  uint32_t total = 0;
  for (uint32_t sector = from / kSector; sector < glyphstore::alignUp(to, kSector) / kSector; ++sector) {
    total += chip.sectorErases(sector);
  }
  return total;
}

// Glyphs 1-4 for A-D, a box each.
Font boxes() {
  Font font;
  font.glyphs.resize(5);
  for (auto& glyph : font.glyphs) glyph.contours = {rect(0, 0, 400, 600)};
  for (uint16_t g = 1; g <= 4; ++g) font.cmap[static_cast<uint32_t>('A' + g - 1)] = g;
  return font;
}

// A GPOS of `count` lookups, a pair adjustment subtable each (A kerns B), under `script` alone: by default dev2, a
// script the shaper handles, so its font gets a CPac.
Bytes gposOf(const uint16_t count, const char* script = "dev2") {
  LayoutLookup lookup;
  PairSubtable pairs;
  pairs.pairs = {{1, {{2, -100}}}};
  lookup.pairSubtables = {pairs};
  LayoutFeature kern{"kern", {}};
  for (uint16_t i = 0; i < count; ++i) kern.lookups.push_back(i);
  return layoutTable({{script, {0}}}, {kern}, std::vector<LayoutLookup>(count, lookup));
}

uint32_t timeNow = 0;
uint32_t steppingClock() { return timeNow += 5; }

class TtfShapingInstall : public ::testing::Test {
 protected:
  void SetUp() override {
    subset[kRegular].bytes = readFixture("NotoSansDevanagari-Shaping-Regular.ttf");
    subset[kBold].bytes = readFixture("NotoSansDevanagari-Shaping-Bold.ttf");
    ASSERT_FALSE(subset[kRegular].bytes.empty());
    ASSERT_FALSE(subset[kBold].bytes.empty());
  }
  void TearDown() override { TtfFont::setHooks({}); }

  // The subset's glyphs, drawn by `set`'s family `family` as the rasterizer draws them from the files.
  void expectSubsetDrawn(const TtfFontSet& set, const uint8_t family) {
    TtfFont& font = *set.font(family);
    for (const uint8_t style : {kRegular, kBold}) {
      for (const uint32_t cp : kDevanagari) {
        expectDrawnAs(reference(subset[style].bytes, cp, 14), font, style, font.glyph(style, cp));
      }
    }
  }

  MemoryFile subset[4];
  int busy = 0;
};

// The glyph IDs: of a code point, and how many the file has.
uint16_t gidOf(const std::vector<uint8_t>& file, const uint32_t cp) {
  FontBytes bytes(file.data(), static_cast<uint32_t>(file.size()));
  ttf::Face face;
  EXPECT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  return ttf::glyphIndex(bytes, face, cp);
}

uint16_t glyphCountOf(const std::vector<uint8_t>& file) {
  FontBytes bytes(file.data(), static_cast<uint32_t>(file.size()));
  ttf::Face face;
  EXPECT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  return face.numGlyphs;
}

// What the rasterizer draws for glyph `gid` at `ppem` (26.6), by index.
Reference referenceByGlyph(const std::vector<uint8_t>& file, const uint16_t gid, const uint32_t ppem) {
  FontBytes bytes(file.data(), static_cast<uint32_t>(file.size()));
  ttf::Face face;
  EXPECT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  ttf::Rasterizer rasterizer;
  EXPECT_TRUE(rasterizer.allocate(
      ttf::planScratch(face, static_cast<uint32_t>(TtfFont::kMaxNativePx) << 6, ttf::kPoolBytesFlash)));
  Reference ref;
  EXPECT_EQ(ttf::Status::Ok, rasterizer.render(bytes, face, gid, ppem, &ref.metrics));
  ref.bitmap.assign(rasterizer.output(), rasterizer.output() + ref.metrics.dataLength);
  return ref;
}

int lockChecks = 0;
bool countedLock() {
  ++lockChecks;
  return true;
}

// A provider that counts what it is asked and draws no glyph keys.
class CountingProvider final : public GlyphProvider {
 public:
  int missed = 0;
  int prefetches = 0;
  const EpdGlyph* missGlyph(const uint32_t) override {
    ++missed;
    return nullptr;
  }
  void prefetchGlyphs(const uint16_t*, const size_t) override { ++prefetches; }
};

class TtfGlyphKeys : public ::testing::Test {
 protected:
  void SetUp() override {
    subset[kRegular].bytes = readFixture("NotoSansDevanagari-Shaping-Regular.ttf");
    ASSERT_FALSE(subset[kRegular].bytes.empty());
    lockChecks = 0;
    TtfFont::Hooks hooks;
    hooks.renderLockHeld = countedLock;
    TtfFont::setHooks(hooks);
  }
  void TearDown() override { TtfFont::setHooks({}); }

  MemoryFile subset[4];
  int busy = 0;
};

}  // namespace

TEST_F(TtfGlyphKeys, AGlyphKeyDrawsThatGlyph) {
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  TtfFont& font = *set->font(0);
  const uint16_t gid = gidOf(subset[kRegular].bytes, 0x0915);
  ASSERT_NE(0, gid);
  expectDrawnAs(reference(subset[kRegular].bytes, 0x0915, 14), font, kRegular,
                font.glyph(kRegular, glyph_key::of(gid)));
  const uint32_t renders = font.renders();
  EXPECT_NE(nullptr, font.glyph(kRegular, glyph_key::of(gid)));
  EXPECT_EQ(renders, font.renders());
}

TEST_F(TtfGlyphKeys, AKeyPastTheFontsGlyphsIsMissingOnce) {
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  TtfFont& font = *set->font(0);
  const uint32_t key = glyph_key::of(glyphCountOf(subset[kRegular].bytes));
  EXPECT_EQ(nullptr, font.glyph(kRegular, key));
  EXPECT_EQ(nullptr, font.glyph(kRegular, key));
  EXPECT_EQ(1, lockChecks);  // the font was asked once; the second answer is the store's
  EXPECT_EQ(0u, font.renders());
}

TEST_F(TtfGlyphKeys, ASyntheticBoldKeyDrawsEmboldened) {
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);  // the Regular alone: Bold is synthesized
  ASSERT_NE(nullptr, set);
  TtfFont& font = *set->font(0);
  const uint16_t gid = gidOf(subset[kRegular].bytes, 0x0915);
  expectDrawnAs(emboldenedAt(subset[kRegular].bytes, 0x0915, ttf::ppemFromPoints(14)), font, kBold,
                font.glyph(kBold, glyph_key::of(gid)));
}

TEST_F(TtfGlyphKeys, KeysDrawInRingMode) {
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  TtfFont& font = *set->font(0);
  ASSERT_TRUE(set->markFull());
  ASSERT_TRUE(font.ringMode());
  const ExpectedRun& run = kRegular1867Runs[0];  // हिन्दी
  ASSERT_GT(run.count, 0);
  for (uint8_t i = 0; i < run.count; ++i) {
    const uint16_t gid = run.glyphs[i].gid;
    expectDrawnAs(referenceByGlyph(subset[kRegular].bytes, gid, ttf::ppemFromPoints(14)), font, kRegular,
                  font.glyph(kRegular, glyph_key::of(gid)));
  }
  EXPECT_TRUE(font.ringMode());
}

TEST_F(TtfGlyphKeys, PrefetchGlyphsStoresTheRunInABatch) {
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  TtfFont& font = *set->font(0);
  const EpdFontFamily family(font.font(kRegular));
  const ExpectedRun& run = kRegular1867Runs[0];  // हिन्दी
  std::vector<uint16_t> gids;
  for (uint8_t i = 0; i < run.count; ++i) gids.push_back(run.glyphs[i].gid);
  ASSERT_TRUE(family.drawsGlyphKeys());

  family.prefetchGlyphs(gids.data(), gids.size(), EpdFontFamily::REGULAR);
  EXPECT_EQ(gids.size(), font.renders());  // each once
  const uint32_t hits = font.storeHits();
  for (const uint16_t gid : gids) {
    expectDrawnAs(referenceByGlyph(subset[kRegular].bytes, gid, ttf::ppemFromPoints(14)), font, kRegular,
                  font.glyph(kRegular, glyph_key::of(gid)));
  }
  EXPECT_EQ(hits + gids.size(), font.storeHits());  // the lookups after it are hits
  EXPECT_EQ(gids.size(), font.renders());

  family.prefetchGlyphs(gids.data(), gids.size(), EpdFontFamily::REGULAR);
  EXPECT_EQ(gids.size(), font.renders());  // nothing new

  const ExpectedRun& other = kRegular1867Runs[1];  // पत्रिका: glyphs the first run lacks
  std::vector<uint16_t> more;
  for (uint8_t i = 0; i < other.count; ++i) more.push_back(other.glyphs[i].gid);
  ASSERT_TRUE(set->markFull());
  const uint32_t used = set->stats().logUsed;
  family.prefetchGlyphs(more.data(), more.size(), EpdFontFamily::REGULAR);
  EXPECT_EQ(gids.size(), font.renders());  // ring mode: nothing rendered, nothing written
  EXPECT_EQ(used, set->stats().logUsed);
}

TEST_F(TtfGlyphKeys, APrefetchRendersAtMostSixtyFourNewGlyphs) {
  // A page's scan hands over up to 512 IDs a style in one call. One call renders the first 64 the store does not hold
  // (TtfFontSet::kPrefetchGlyphs), in one batch; the others render one by one as they are drawn.
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  TtfFont& font = *set->font(0);
  const EpdFontFamily family(font.font(kRegular));
  constexpr uint16_t kAsked = 100;
  ASSERT_GT(glyphCountOf(subset[kRegular].bytes), kAsked);
  ASSERT_EQ(64u, TtfFontSet::kPrefetchGlyphs);
  std::vector<uint16_t> ids;
  for (uint16_t gid = 1; gid <= kAsked; ++gid) ids.push_back(gid);

  family.prefetchGlyphs(ids.data(), ids.size(), EpdFontFamily::REGULAR);
  EXPECT_EQ(64u, font.renders());
  for (uint16_t gid = 1; gid <= 64; ++gid) ASSERT_NE(nullptr, font.glyph(kRegular, glyph_key::of(gid))) << gid;
  EXPECT_EQ(64u, font.renders());  // the batch's: store hits
  for (uint16_t gid = 65; gid <= kAsked; ++gid) ASSERT_NE(nullptr, font.glyph(kRegular, glyph_key::of(gid))) << gid;
  EXPECT_EQ(uint32_t{kAsked}, font.renders());  // the rest, as drawn
}

TEST_F(TtfGlyphKeys, AFontThatDrawsNoGlyphKeysIsNotAsked) {
  CountingProvider provider;
  EpdFontData data{};
  data.provider = &provider;
  const EpdFont font(&data);
  const EpdFontFamily family(&font);
  EXPECT_FALSE(family.drawsGlyphKeys());
  EXPECT_FALSE(static_cast<bool>(family.findGlyphKey(glyph_key::of(5))));
  EXPECT_EQ(0, provider.missed);
  const uint16_t ids[] = {5, 6};
  family.prefetchGlyphs(ids, 2, EpdFontFamily::REGULAR);
  EXPECT_EQ(0, provider.prefetches);  // the glyph-ID prefetch is not passed on either

  const EpdFontData builtIn{};  // no provider
  const EpdFont plain(&builtIn);
  const EpdFontFamily builtInFamily(&plain);
  EXPECT_FALSE(builtInFamily.drawsGlyphKeys());
  EXPECT_FALSE(static_cast<bool>(builtInFamily.findGlyphKey(glyph_key::of(5))));
  const uint16_t glyphs[] = {5};
  builtInFamily.prefetchGlyphs(glyphs, 1, EpdFontFamily::REGULAR);  // nothing to ask
}

TEST_F(TtfShapingInstall, TheInstallWritesEachBlocksCpacAfterItsFile) {
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  ASSERT_FALSE(set->font(0)->readsCard());
  const glyphstore::Header header = headerOf(chip);
  ASSERT_EQ(2, header.geometry.styleCount);
  for (const uint8_t style : {kRegular, kBold}) {
    const std::vector<uint8_t>& file = subset[style].bytes;
    const glyphstore::StyleInfo& block = header.styles[style];
    EXPECT_EQ(glyphstore::kModeFlash, block.mode);
    EXPECT_EQ(file.size(), block.fileSize);
    const std::vector<uint8_t> table = writerTable(file);
    ASSERT_GT(table.size(), 0u);
    EXPECT_EQ(ttf::flashBlockBytes(block.fileSize, static_cast<uint32_t>(table.size())), block.size);
    EXPECT_EQ(file, chipBytes(chip, block.offset, block.fileSize));  // the file, then at align4(fileSize) its CPac
    expectRoomHolds(chip, block, table);
    EXPECT_TRUE(cpacAccepted(chip, block));
  }
  expectSubsetDrawn(*set, 0);
  EXPECT_EQ(0u, chip.violations());
  EXPECT_EQ(0u, chip.reprograms());
}

TEST_F(TtfShapingInstall, ALatinFamilyInstallsWithoutACpac) {
  // Bookerly has lookups, but its layout names no script the shaper handles: each flash-mode block is its file alone,
  // as before CPac, and nothing is written past it.
  MemoryFile bookerly[4];
  bookerly[0].bytes = readFont("Bookerly/Bookerly-Regular.ttf");
  bookerly[1].bytes = readFont("Bookerly/Bookerly-Bold.ttf");
  bookerly[2].bytes = readFont("Bookerly/Bookerly-Italic.ttf");
  bookerly[3].bytes = readFont("Bookerly/Bookerly-BoldItalic.ttf");
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {bookerly}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  ASSERT_FALSE(set->font(0)->readsCard());
  const glyphstore::Header header = headerOf(chip);
  ASSERT_EQ(4, header.geometry.styleCount);
  for (uint8_t s = 0; s < 4; ++s) {
    const glyphstore::StyleInfo& block = header.styles[s];
    EXPECT_EQ(glyphstore::kModeFlash, block.mode) << int{s};
    EXPECT_EQ(bookerly[s].bytes.size(), block.fileSize) << int{s};
    EXPECT_EQ(block.fileSize, block.size) << int{s};
    EXPECT_EQ(bookerly[s].bytes, chipBytes(chip, block.offset, block.size)) << int{s};
    const uint32_t end = block.offset + block.size;
    EXPECT_TRUE(erased(chipBytes(chip, end, glyphstore::alignUp(end, kSector) - end))) << int{s};
    EXPECT_GT(writerTable(bookerly[s].bytes).size(), 0u) << int{s};  // a table it would take, never reserved
  }
  expectDrawnAs(reference(bookerly[0].bytes, 'g', 14), *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'g'));
  EXPECT_EQ(0u, chip.violations());
  EXPECT_EQ(0u, chip.reprograms());
}

TEST_F(TtfShapingInstall, AnSdModeBlocksCpacIsATableOfItsDirectory) {
  // Bookerly as the body on 26 pages: whole it does not fit, so every family's tables go to the card, the subset's
  // with them (TtfFontSetTest.ABodyTooLargeForFlashModeTakesTheAccentFamiliesToTheCardWithIt).
  MemoryFile bookerly[4];
  bookerly[0].bytes = readFont("Bookerly/Bookerly-Regular.ttf");
  bookerly[1].bytes = readFont("Bookerly/Bookerly-Bold.ttf");
  bookerly[2].bytes = readFont("Bookerly/Bookerly-Italic.ttf");
  bookerly[3].bytes = readFont("Bookerly/Bookerly-BoldItalic.ttf");
  MemoryFile accent[4];
  accent[kRegular].bytes = subset[kRegular].bytes;
  SimNorFlash chip{26 * kPage};
  const auto set = load(chip, {bookerly, accent}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_TRUE(set->font(0)->readsCard());
  ASSERT_TRUE(set->font(1)->readsCard());
  const glyphstore::Header header = headerOf(chip);
  ASSERT_EQ(5, header.geometry.styleCount);
  for (uint8_t s = 0; s < 4; ++s) {  // Bookerly's blocks: its tables, no CPac record
    const glyphstore::StyleInfo& latin = header.styles[s];
    ASSERT_EQ(glyphstore::kModeSd, latin.mode) << int{s};
    EXPECT_FALSE(recordOf(chipBytes(chip, latin.offset, latin.size), "CPac").found) << int{s};
  }
  const glyphstore::StyleInfo& style = header.styles[4];
  ASSERT_EQ(glyphstore::kModeSd, style.mode);
  EXPECT_EQ(accent[kRegular].bytes.size(), style.fileSize);
  const std::vector<uint8_t> block = chipBytes(chip, style.offset, style.size);

  const DirRecord cpac = recordOf(block, "CPac");
  ASSERT_TRUE(cpac.found);
  const std::vector<uint8_t> table = writerTable(block);
  ASSERT_GT(table.size(), 0u);
  ASSERT_LE(cpac.offset + table.size(), block.size());
  ASSERT_LE(table.size(), cpac.length);
  EXPECT_EQ(table, std::vector<uint8_t>(block.begin() + cpac.offset, block.begin() + cpac.offset + table.size()));
  EXPECT_TRUE(erased(
      std::vector<uint8_t>(block.begin() + cpac.offset + table.size(), block.begin() + cpac.offset + cpac.length)));
  EXPECT_EQ(writerTable(accent[kRegular].bytes), table);  // the block's lookups are the file's
  ot::FaceTables tables;
  ASSERT_TRUE(ot::tablesFromSfnt(block.data(), static_cast<uint32_t>(block.size()), &tables));
  ot::Face face;
  ASSERT_TRUE(face.init(tables));
  EXPECT_NE(nullptr, face.filters(ot::GSUB));  // the block's own CPac record, taken
  EXPECT_NE(nullptr, face.filters(ot::GPOS));

  const DirRecord gdef = recordOf(block, "GDEF");
  const DirRecord gdefInFile = recordOf(accent[kRegular].bytes, "GDEF");
  ASSERT_TRUE(gdef.found);
  ASSERT_TRUE(gdefInFile.found);
  ASSERT_EQ(gdefInFile.length, gdef.length);
  EXPECT_TRUE(std::equal(block.begin() + gdef.offset, block.begin() + gdef.offset + gdef.length,
                         accent[kRegular].bytes.begin() + gdefInFile.offset));

  for (const uint32_t cp : kDevanagari) {
    expectDrawnAs(reference(accent[kRegular].bytes, cp, 14), *set->font(1), kRegular,
                  set->font(1)->glyph(kRegular, cp));
  }
  EXPECT_EQ(0u, chip.violations());
  EXPECT_EQ(0u, chip.reprograms());
}

TEST_F(TtfShapingInstall, AKeptBlockKeepsItsCpac) {
  // The subset beside one book's accent family, then beside another's: the subset's blocks hold their files and
  // their CPac where the new layout puts them, and are neither erased nor written again.
  MemoryFile inter[4];
  MemoryFile ubuntu[4];
  inter[kRegular].bytes = readFont("Inter/Inter-Regular.ttf");
  ubuntu[kRegular].bytes = readFont("Ubuntu/Ubuntu-Regular.ttf");
  SimNorFlash chip{kFlashBytes};
  ASSERT_NE(nullptr, load(chip, {subset, inter}, kIdentityA, busy));
  const glyphstore::Header first = headerOf(chip);
  const uint32_t from = first.styles[0].offset;
  const uint32_t to = first.styles[1].offset + first.styles[1].size;
  const uint32_t before = erasesBetween(chip, from, to);

  const auto set = load(chip, {subset, ubuntu}, kIdentityB, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(2, busy);  // installed again: another set
  const glyphstore::Header second = headerOf(chip);
  for (const uint8_t style : {kRegular, kBold}) {
    EXPECT_EQ(first.styles[style].offset, second.styles[style].offset);
    EXPECT_EQ(first.styles[style].size, second.styles[style].size);
    expectRoomHolds(chip, second.styles[style], writerTable(subset[style].bytes));
  }
  EXPECT_EQ(before, erasesBetween(chip, from, to));  // the subset's blocks kept
  EXPECT_EQ(0u, chip.reprograms());                  // and nothing written over them
  expectSubsetDrawn(*set, 0);
  expectDrawnAs(reference(ubuntu[kRegular].bytes, 'W', 14), *set->font(1), kRegular,
                set->font(1)->glyph(kRegular, 'W'));  // where Inter was: written anew
}

TEST_F(TtfShapingInstall, AKeptBlockWithATornCpacIsWrittenAgain) {
  // A cut while the first block's CPac was written leaves its last bytes erased, and the header erased with them:
  // finishInstall() comes after every block. The next load finds the files whole but that CPac torn.
  SimNorFlash chip{kFlashBytes};
  ASSERT_NE(nullptr, load(chip, {subset}, kIdentityA, busy));
  const glyphstore::Header header = headerOf(chip);
  const glyphstore::StyleInfo torn = header.styles[0];
  const glyphstore::StyleInfo whole = header.styles[1];
  const std::vector<uint8_t> table = writerTable(subset[kRegular].bytes);
  ASSERT_GE(table.size(), 64u);
  const uint32_t tableAt = torn.offset + glyphstore::cpacRoomOf(torn.fileSize, torn.size).at;
  std::fill_n(chip.raw() + tableAt + table.size() - 64, 64, 0xFF);
  ASSERT_TRUE(chip.erase(0, kSector));
  const uint32_t firstSector = torn.offset / kSector;
  const uint32_t sectors = glyphstore::alignUp(torn.size, kSector) / kSector;
  std::vector<uint32_t> erases(sectors);
  for (uint32_t i = 0; i < sectors; ++i) erases[i] = chip.sectorErases(firstSector + i);
  const uint32_t wholeErases = erasesBetween(chip, whole.offset, whole.offset + whole.size);

  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(2, busy);
  for (uint32_t i = 0; i < sectors; ++i) EXPECT_EQ(erases[i] + 1, chip.sectorErases(firstSector + i)) << i;
  EXPECT_EQ(wholeErases, erasesBetween(chip, whole.offset, whole.offset + whole.size));  // the other block kept
  expectRoomHolds(chip, headerOf(chip).styles[0], table);
  expectSubsetDrawn(*set, 0);
  EXPECT_EQ(0u, chip.reprograms());
}

TEST_F(TtfShapingInstall, AFamilyThatFitsWholeOnlyWithoutItsCpacGoesToTheCard) {
  // A file 8 bytes short of a page, with a GPOS of 10 lookups under arab, which the joining shaper will read: its CPac
  // (536 B) takes its block to a second page.
  constexpr uint32_t kFileBytes = kPage - 8;
  Font font = boxes();
  font.extraTables = {{"GPOS", gposOf(10, "arab")}};
  const Bytes unpadded = buildFont(font);
  ASSERT_LT(unpadded.size() + 16, kFileBytes);
  font.extraTables.push_back({"zzzz", Bytes(kFileBytes - unpadded.size() - 16, 0)});  // its record, then its bytes
  MemoryFile padded[4];
  padded[kRegular].bytes = buildFont(font);
  ASSERT_EQ(kFileBytes, padded[kRegular].bytes.size());
  uint32_t room = 0;
  ASSERT_TRUE(ttf::filterTableBytes(padded[kRegular].source(), &room));
  ASSERT_EQ(16u + 28u * 10 + 24u * 10, room);

  // The smallest font area of whole pages that holds the file whole, without its CPac, with a chip's mapping pages.
  const uint32_t freeMapPages = SimNorFlash{kPage}.freeMapPages();
  const auto fits = [freeMapPages](const uint32_t pages, const uint32_t bytes) {
    glyphstore::PlanInput in{};
    in.styleCount = 1;
    in.styleMode[0] = glyphstore::kModeFlash;
    in.styleBytes[0] = bytes;
    in.usableBytes = pages * kPage;
    in.freeMapPages = freeMapPages;
    glyphstore::Layout layout{};
    return glyphstore::planLayout(in, layout);
  };
  uint32_t pages = 1;
  while (pages < 64 && !fits(pages, kFileBytes)) ++pages;
  ASSERT_LT(pages, 64u);
  ASSERT_FALSE(fits(pages, ttf::flashBlockBytes(kFileBytes, room)));  // whole with its CPac, it does not

  SimNorFlash chip{pages * kPage};
  const auto set = load(chip, {padded}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_TRUE(set->font(0)->readsCard());
  for (const uint32_t cp : {uint32_t{'A'}, uint32_t{'D'}}) {
    expectDrawnAs(reference(padded[kRegular].bytes, cp, 14), *set->font(0), kRegular,
                  set->font(0)->glyph(kRegular, cp));
  }
  const glyphstore::StyleInfo style = headerOf(chip).styles[0];
  ASSERT_EQ(glyphstore::kModeSd, style.mode);
  const std::vector<uint8_t> block = chipBytes(chip, style.offset, style.size);
  const DirRecord cpac = recordOf(block, "CPac");
  ASSERT_TRUE(cpac.found);
  ASSERT_EQ(room, cpac.length);
  const std::vector<uint8_t> table = writerTable(padded[kRegular].bytes);
  ASSERT_EQ(room, table.size());
  EXPECT_EQ(table, std::vector<uint8_t>(block.begin() + cpac.offset, block.begin() + cpac.offset + cpac.length));
  EXPECT_EQ(0u, chip.violations());
  EXPECT_EQ(0u, chip.reprograms());
}

TEST_F(TtfShapingInstall, ACpacThatDoesNotFitItsRoomIsLeftErased) {
  // Two GPOS records: the probe counts the first (one lookup, a 68-byte room), the shaper's face reads the last (ten
  // lookups, a 536-byte table). The table is not written: the room stays erased, which the face's check refuses, so
  // it shapes on computed filters.
  Font font = boxes();
  font.extraTables = {{"GPOS", gposOf(1)}, {"GPOT", gposOf(10)}};
  MemoryFile twice[4];
  twice[kRegular].bytes = buildFont(font);
  const size_t renamed = findRecord(twice[kRegular].bytes, "GPOT");
  ASSERT_NE(0u, renamed);
  twice[kRegular].bytes[renamed + 3] = 'S';
  uint32_t room = 0;
  ASSERT_TRUE(ttf::filterTableBytes(twice[kRegular].source(), &room));
  ASSERT_EQ(16u + 28u + 24u, room);
  ASSERT_EQ(16u + 28u * 10 + 24u * 10, writerTable(twice[kRegular].bytes).size());

  SimNorFlash chip{kFlashBytes};
  ASSERT_NE(nullptr, load(chip, {twice}, kIdentityA, busy));
  const glyphstore::StyleInfo style = headerOf(chip).styles[0];
  ASSERT_EQ(glyphstore::kModeFlash, style.mode);
  EXPECT_EQ(ttf::flashBlockBytes(style.fileSize, room), style.size);
  expectRoomHolds(chip, style, {});
  EXPECT_FALSE(cpacAccepted(chip, style));
  // Another set with it first keeps its block: the erased room is what the install would leave there.
  const uint32_t before = erasesBetween(chip, style.offset, style.offset + style.size);
  MemoryFile accent[4];
  accent[kRegular].bytes = subset[kRegular].bytes;
  const auto set = load(chip, {twice, accent}, kIdentityB, busy);
  ASSERT_NE(nullptr, set);
  EXPECT_EQ(2, busy);
  EXPECT_EQ(before, erasesBetween(chip, style.offset, style.offset + style.size));
  expectDrawnAs(reference(twice[kRegular].bytes, 'A', 14), *set->font(0), kRegular, set->font(0)->glyph(kRegular, 'A'));

  // A list the probe gives up on (capped: no room), whose lookups the face reads all the same: the block is the file.
  constexpr uint32_t kLookups = 4096;
  const uint32_t bodyAt = 2 + 2 * kLookups;
  Bytes body(64 * 256, 0);
  for (uint32_t k = 0; k < 64; ++k) {
    Bytes lookup;
    for (const uint32_t word : {1u, 0u, 1u}) put16(lookup, word);
    std::copy(lookup.begin(), lookup.end(), body.begin() + 256 * k);
  }
  Bytes gsub;
  for (const uint32_t word : {1u, 0u, 10u, 0u, 18u}) put16(gsub, word);  // scripts at 10, no features, lookups at 18
  for (const uint32_t word : {1u, uint32_t{'d' << 8 | 'e'}, uint32_t{'v' << 8 | '2'}, 0u}) put16(gsub, word);  // dev2
  put16(gsub, kLookups);
  for (uint32_t i = 0; i < kLookups; ++i) put16(gsub, bodyAt + 256 * (i % 64));
  gsub.insert(gsub.end(), body.begin(), body.end());
  Font scattered = boxes();
  scattered.extraTables = {{"GSUB", gsub}};
  MemoryFile capped[4];
  capped[kRegular].bytes = buildFont(scattered);
  ASSERT_TRUE(ttf::filterTableBytes(capped[kRegular].source(), &room));
  ASSERT_EQ(0u, room);
  ASSERT_GT(writerTable(capped[kRegular].bytes).size(), 0u);
  SimNorFlash other{kFlashBytes};
  int otherBusy = 0;
  const auto alone = load(other, {capped}, kIdentityA, otherBusy);
  ASSERT_NE(nullptr, alone);
  const glyphstore::StyleInfo block = headerOf(other).styles[0];
  EXPECT_EQ(capped[kRegular].bytes.size(), block.fileSize);
  EXPECT_EQ(block.fileSize, block.size);
  const uint32_t end = block.offset + block.size;
  EXPECT_TRUE(erased(chipBytes(other, end, glyphstore::alignUp(end, kSector) - end)));  // nothing past it
  expectDrawnAs(reference(capped[kRegular].bytes, 'B', 14), *alone->font(0), kRegular,
                alone->font(0)->glyph(kRegular, 'B'));
  EXPECT_EQ(0u, chip.reprograms());
  EXPECT_EQ(0u, other.reprograms());
}

TEST_F(TtfShapingInstall, TheInstallTimeIsReported) {
  SimNorFlash untimed{kFlashBytes};
  ASSERT_NE(nullptr, load(untimed, {subset}, kIdentityA, busy));
  EXPECT_EQ(0u, TtfFontSet::storeEvents().lastInstallMs);  // without the nowMs hook

  timeNow = 0;
  TtfFont::Hooks hooks;
  hooks.nowMs = steppingClock;
  TtfFont::setHooks(hooks);
  SimNorFlash chip{kFlashBytes};
  ASSERT_NE(nullptr, load(chip, {subset}, kIdentityA, busy));
  EXPECT_EQ(2, busy);
  EXPECT_GT(TtfFontSet::storeEvents().lastInstallMs, 0u);
}

// --- The CPac room of a block ---

TEST(GlyphStoreCpacRoom, ARoomIsWhatLiesAfterTheAlignedFile) {
  using glyphstore::cpacRoomOf;
  EXPECT_EQ(1000u, cpacRoomOf(1000, 1200).at);
  EXPECT_EQ(200u, cpacRoomOf(1000, 1200).bytes);
  EXPECT_EQ(1004u, cpacRoomOf(1001, 1200).at);  // after the file's last byte, 4-aligned
  EXPECT_EQ(196u, cpacRoomOf(1001, 1200).bytes);
  EXPECT_EQ(0u, cpacRoomOf(1000, 1000).bytes);  // a block the file's size: no CPac
  EXPECT_EQ(1000u, cpacRoomOf(1000, 1000).at);
}

TEST(GlyphStoreCpacRoom, ABlockTooShortToAlignItsFileHasNoRoomAndDoesNotWrap) {
  using glyphstore::cpacRoomOf;
  // Exactly an unaligned file: align4(fileSize) is past the block, and size - align4(fileSize) would wrap.
  EXPECT_EQ(0u, cpacRoomOf(1001, 1001).bytes);
  EXPECT_LE(cpacRoomOf(1001, 1001).at, 1001u);
  // Between the file and its alignment: no room, and `at` says the block cannot hold the file's CPac place.
  EXPECT_EQ(0u, cpacRoomOf(1001, 1003).bytes);
  EXPECT_GT(cpacRoomOf(1001, 1003).at, 1003u);
  EXPECT_EQ(0u, cpacRoomOf(1001, 1004).bytes);  // the aligned end exactly: an empty room, inside the block
  EXPECT_EQ(1004u, cpacRoomOf(1001, 1004).at);
}

TEST(TtfShapingTables, AnUnalignedFileTheSizeOfItsBlockHasNoCpac) {
  std::vector<uint8_t> file = readFixture("NotoSansDevanagari-Shaping-Regular.ttf");
  ASSERT_FALSE(file.empty());
  while (file.size() % 4 != 1) file.push_back(0);
  const uint32_t size = static_cast<uint32_t>(file.size());
  ot::FaceTables tables;
  ASSERT_TRUE(ttf::shapingTables(file.data(), size, size, &tables));
  EXPECT_TRUE(tables[ot::FaceTables::CPAC].empty());
  EXPECT_TRUE(tables[ot::FaceTables::CPPL].empty());
  EXPECT_FALSE(tables[ot::FaceTables::HEAD].empty());
  EXPECT_FALSE(ttf::shapingTables(file.data(), size - 1, size, &tables));  // a file larger than its block
}

// --- ShapingFace over a scripted mapping ---

namespace {

using ottest::StoredGlyph;

std::vector<StoredGlyph> storedOf(const ExpectedRun& run) {
  std::vector<StoredGlyph> out;
  for (unsigned i = 0; i < run.count; ++i) {
    out.push_back({run.glyphs[i].gid, run.glyphs[i].advance12_4, run.glyphs[i].dx, run.glyphs[i].dy});
  }
  return out;
}

const RunSet& runSetOf(const char* file, const uint32_t ppem26_6) {
  for (const RunSet& set : kDevanagariRunSets) {
    if (std::strcmp(set.file, file) == 0 && set.ppem26_6 == ppem26_6) return set;
  }
  ADD_FAILURE() << file << " at " << ppem26_6;
  return kDevanagariRunSets[0];
}

const char* const kSubsetFiles[2] = {"NotoSansDevanagari-Shaping-Regular.ttf", "NotoSansDevanagari-Shaping-Bold.ttf"};

// A plan per run of `set`, built on the face `shaping` gives now.
std::vector<std::unique_ptr<ot::Plan>> plansOf(ShapingFace& shaping, const RunSet& set) {
  std::vector<std::unique_ptr<ot::Plan>> plans;
  const ot::Face* face = shaping.current();
  EXPECT_NE(nullptr, face);
  for (size_t r = 0; face && r < set.count; ++r) {
    plans.push_back(std::make_unique<ot::Plan>());
    EXPECT_TRUE(plans.back()->build(*face, ot::Script::Devanagari, ot::languageTagsFor(set.runs[r].language))) << r;
  }
  return plans;
}

// Every run of `set` shaped on the face `shaping` gives now, with the plans built before (plansOf), as the oracle says.
void expectPlansShape(ShapingFace& shaping, const RunSet& set, const std::vector<std::unique_ptr<ot::Plan>>& plans) {
  ASSERT_EQ(set.count, plans.size());
  for (size_t r = 0; r < set.count; ++r) {
    const ot::Face* face = shaping.current();
    ASSERT_NE(nullptr, face);
    EXPECT_EQ(storedOf(set.runs[r]), ottest::shapeAsStored(*face, *plans[r], set.runs[r].utf8, set.ppem26_6))
        << set.file << " at " << set.ppem26_6 << ", run " << r << ": " << set.runs[r].utf8;
  }
}

// Every run of `set` shaped through `shaping`, a plan built on its face for each, as the oracle says.
void expectRunsShape(ShapingFace& shaping, const RunSet& set) {
  for (size_t r = 0; r < set.count; ++r) {
    const ot::Face* face = shaping.current();
    ASSERT_NE(nullptr, face);
    ot::Plan plan;
    ASSERT_TRUE(plan.build(*face, ot::Script::Devanagari, ot::languageTagsFor(set.runs[r].language))) << r;
    EXPECT_EQ(storedOf(set.runs[r]), ottest::shapeAsStored(*face, plan, set.runs[r].utf8, set.ppem26_6))
        << set.file << " at " << set.ppem26_6 << ", run " << r << ": " << set.runs[r].utf8;
  }
}

// A ShapingFace over bytes in memory, its mapping answering from a script of bases.
class ScriptedFace final : public ShapingFace {
 public:
  std::vector<const uint8_t*> bases;  // map()'s answers in turn; the last repeats
  uint32_t size = 0;
  bool dropHead = false;
  int mapCalls = 0;
  int tablesCalls = 0;

 protected:
  const uint8_t* map() override {
    if (bases.empty()) return nullptr;
    const size_t at = std::min<size_t>(static_cast<size_t>(mapCalls++), bases.size() - 1);
    return bases[at];
  }
  bool tables(const uint8_t* base, ot::FaceTables* out) override {
    ++tablesCalls;
    if (!ot::tablesFromSfnt(base, size, out)) return false;
    out->tables[ot::FaceTables::CPPL] = ot::Table();
    if (dropHead) out->tables[ot::FaceTables::HEAD] = ot::Table();
    return true;
  }
};

const ShapingFixture& devanagariFixture() {
  for (const ShapingFixture& fx : kShapingFixtures) {
    if (std::strcmp(fx.script, "Devanagari") == 0) return fx;
  }
  ADD_FAILURE() << "no Devanagari fixture";
  return kShapingFixtures[0];
}

}  // namespace

TEST(ShapingFace, TheFirstCallBuildsAndAMoveRepoints) {
  const ShapingFixture& fx = devanagariFixture();
  std::vector<uint8_t> a = ottest::readFixture(fx.layoutFile);
  ASSERT_FALSE(a.empty());
  std::vector<uint8_t> b = a;
  ScriptedFace face;
  face.size = static_cast<uint32_t>(a.size());
  face.bases = {a.data(), a.data(), b.data()};

  const ot::Face* first = face.current();
  ASSERT_NE(nullptr, first);
  std::vector<std::unique_ptr<ot::Plan>> plans;
  for (size_t w = 0; w < fx.count; ++w) {
    plans.push_back(std::make_unique<ot::Plan>());
    ASSERT_TRUE(plans.back()->build(*first, ot::Script::Devanagari, ot::languageTagsFor(fx.words[w].language))) << w;
  }
  EXPECT_EQ(first, face.current());  // the same base: nothing moved
  EXPECT_EQ(first, face.current());  // B: re-pointed, the same face
  std::fill(a.begin(), a.end(), uint8_t{0xA5});
  EXPECT_EQ(1, face.tablesCalls);
  for (size_t w = 0; w < fx.count; ++w) {
    const ExpectedShaping& word = fx.words[w];
    std::vector<StoredGlyph> expected;
    for (unsigned i = 0; i < word.count; ++i) {
      expected.push_back({word.glyphs[i].gid, word.glyphs[i].advance12_4, word.glyphs[i].dx, word.glyphs[i].dy});
    }
    EXPECT_EQ(expected, ottest::shapeAsStored(*face.current(), *plans[w], word.utf8)) << w << ": " << word.utf8;
  }
  EXPECT_EQ(1, face.tablesCalls);
}

TEST(ShapingFace, AnUnmappedBlockGivesNoFaceForNow) {
  const ShapingFixture& fx = devanagariFixture();
  std::vector<uint8_t> bytes = ottest::readFixture(fx.layoutFile);
  ASSERT_FALSE(bytes.empty());
  ScriptedFace face;
  face.size = static_cast<uint32_t>(bytes.size());
  EXPECT_EQ(nullptr, face.current());  // nothing mapped
  EXPECT_EQ(0, face.tablesCalls);
  face.bases = {bytes.data()};
  EXPECT_NE(nullptr, face.current());
  EXPECT_EQ(1, face.tablesCalls);
}

TEST(ShapingFace, RefusedTablesAreNotAskedAgain) {
  const ShapingFixture& fx = devanagariFixture();
  std::vector<uint8_t> bytes = ottest::readFixture(fx.layoutFile);
  ASSERT_FALSE(bytes.empty());
  ScriptedFace face;
  face.size = static_cast<uint32_t>(bytes.size());
  face.bases = {bytes.data()};
  face.dropHead = true;
  EXPECT_EQ(nullptr, face.current());
  EXPECT_EQ(nullptr, face.current());
  EXPECT_EQ(1, face.tablesCalls);
}

// --- A TrueType family's shaping faces ---

namespace {

class TtfShapingFace : public ::testing::Test {
 protected:
  void SetUp() override {
    subset[kRegular].bytes = readFixture(kSubsetFiles[kRegular]);
    subset[kBold].bytes = readFixture(kSubsetFiles[kBold]);
    ASSERT_FALSE(subset[kRegular].bytes.empty());
    ASSERT_FALSE(subset[kBold].bytes.empty());
  }
  void TearDown() override { TtfFont::setHooks({}); }

  // The face of `font`'s `style` through the family API, and the pixels per em it says its glyphs are drawn at.
  static ShapingFace* faceOf(const TtfFont& font, const uint8_t style, uint32_t* ppem) {
    const EpdFontFamily family(font.font(kRegular), font.font(kBold));
    return family.shapingFace(static_cast<EpdFontFamily::Style>(style), ppem);
  }

  // Both styles of `set`'s family `family` shape as the oracle says at the body size and at a native size of 40 pixels.
  void expectShapesAsHarfBuzz(TtfFontSet& set, const uint8_t family) {
    TtfFont& font = *set.font(family);
    const EpdFont* native[4] = {};
    ASSERT_TRUE(font.nativeFonts(40, native));
    for (const uint8_t style : {kRegular, kBold}) {
      uint32_t ppem = 0;
      ShapingFace* body = faceOf(font, style, &ppem);
      ASSERT_NE(nullptr, body);
      EXPECT_EQ(1867u, ppem);
      expectRunsShape(*body, runSetOf(kSubsetFiles[style], 1867));

      const EpdFontFamily nativeFamily(native[kRegular], native[kBold]);
      ShapingFace* big = nativeFamily.shapingFace(static_cast<EpdFontFamily::Style>(style), &ppem);
      EXPECT_EQ(body, big);  // one face a file, whatever the size
      EXPECT_EQ(2560u, ppem);
      ASSERT_NE(nullptr, big);
      expectRunsShape(*big, runSetOf(kSubsetFiles[style], 2560));
    }
  }

  // The Bookerly body on 26 pages, whole it does not fit: every family's tables go to the card, the subset's with
  // them (AnSdModeBlocksCpacIsATableOfItsDirectory).
  std::unique_ptr<TtfFontSet> loadSdMode(SimNorFlash& chip) {
    bookerly[0].bytes = readFont("Bookerly/Bookerly-Regular.ttf");
    bookerly[1].bytes = readFont("Bookerly/Bookerly-Bold.ttf");
    bookerly[2].bytes = readFont("Bookerly/Bookerly-Italic.ttf");
    bookerly[3].bytes = readFont("Bookerly/Bookerly-BoldItalic.ttf");
    accent[kRegular].bytes = subset[kRegular].bytes;
    accent[kBold].bytes = subset[kBold].bytes;
    return load(chip, {bookerly, accent}, kIdentityA, busy);
  }

  // `set`'s family 1 of loadSdMode(): the blocks of the subset's two styles.
  static constexpr uint8_t kAccentFirstBlock = 4;

  // Whether the shaping face of `style` takes its filters from the CPac, and shapes alike when it is left out: a face
  // over the same tables without it, its filters computed.
  void expectCpacOnlySavesWork(ShapingFace& shaping, const std::vector<uint8_t>& block, const uint32_t fileBytes,
                               const RunSet& runs) {
    const ot::Face* with = shaping.current();
    ASSERT_NE(nullptr, with);
    EXPECT_NE(nullptr, with->filters(ot::GSUB));
    EXPECT_NE(nullptr, with->filters(ot::GPOS));
    ot::FaceTables tables;
    ASSERT_TRUE(ttf::shapingTables(block.data(), static_cast<uint32_t>(block.size()), fileBytes, &tables));
    ASSERT_FALSE(tables[ot::FaceTables::CPAC].empty());
    tables[ot::FaceTables::CPAC] = ot::Table();
    ot::Face without;
    ASSERT_TRUE(without.init(tables));
    EXPECT_EQ(nullptr, without.filters(ot::GSUB));
    EXPECT_EQ(nullptr, without.filters(ot::GPOS));
    for (size_t r = 0; r < runs.count; ++r) {
      const ExpectedRun& run = runs.runs[r];
      const uint32_t* const language = ot::languageTagsFor(run.language);
      ot::Plan planWith;
      ot::Plan planWithout;
      ASSERT_TRUE(planWith.build(*with, ot::Script::Devanagari, language)) << r;
      ASSERT_TRUE(planWithout.build(without, ot::Script::Devanagari, language)) << r;
      const std::vector<StoredGlyph> stored = ottest::shapeAsStored(*with, planWith, run.utf8, runs.ppem26_6);
      const std::vector<StoredGlyph> computed = ottest::shapeAsStored(without, planWithout, run.utf8, runs.ppem26_6);
      EXPECT_EQ(computed, stored) << r << ": " << run.utf8;
      EXPECT_EQ(storedOf(run), stored) << r << ": " << run.utf8;
    }
  }

  MemoryFile subset[4];
  MemoryFile bookerly[4];
  MemoryFile accent[4];
  int busy = 0;
};

}  // namespace

TEST_F(TtfShapingFace, ShapesAsHarfBuzzInFlashMode) {
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  ASSERT_FALSE(set->font(0)->readsCard());
  expectShapesAsHarfBuzz(*set, 0);
  EXPECT_EQ(0u, chip.violations());
}

TEST_F(TtfShapingFace, ShapesAsHarfBuzzInSdMode) {
  SimNorFlash chip{26 * kPage};
  const auto set = loadSdMode(chip);
  ASSERT_NE(nullptr, set);
  ASSERT_TRUE(set->font(1)->readsCard());
  ASSERT_EQ(glyphstore::kModeSd, headerOf(chip).styles[kAccentFirstBlock].mode);
  expectShapesAsHarfBuzz(*set, 1);
  EXPECT_EQ(0u, chip.violations());
}

TEST_F(TtfShapingFace, TheFaceWithAndWithoutCpacShapesAlikeInFlashMode) {
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  const glyphstore::Header header = headerOf(chip);
  for (const uint8_t style : {kRegular, kBold}) {
    const glyphstore::StyleInfo& info = header.styles[style];
    ASSERT_EQ(glyphstore::kModeFlash, info.mode);
    ShapingFace* face = set->font(0)->shapingFace(style);
    ASSERT_NE(nullptr, face);
    expectCpacOnlySavesWork(*face, chipBytes(chip, info.offset, info.size), info.fileSize,
                            runSetOf(kSubsetFiles[style], 1867));
  }
}

TEST_F(TtfShapingFace, TheFaceWithAndWithoutCpacShapesAlikeInSdMode) {
  SimNorFlash chip{26 * kPage};
  const auto set = loadSdMode(chip);
  ASSERT_NE(nullptr, set);
  const glyphstore::Header header = headerOf(chip);
  for (const uint8_t style : {kRegular, kBold}) {
    const glyphstore::StyleInfo& info = header.styles[kAccentFirstBlock + style];
    ASSERT_EQ(glyphstore::kModeSd, info.mode);
    ShapingFace* face = set->font(1)->shapingFace(style);
    ASSERT_NE(nullptr, face);
    expectCpacOnlySavesWork(*face, chipBytes(chip, info.offset, info.size), 0, runSetOf(kSubsetFiles[style], 1867));
  }
}

TEST_F(TtfShapingFace, AFaceRepointedAfterAnotherStyleMappedShapesAlike) {
  SimNorFlash chip{kFlashBytes};
  MovingNorFlash moving(chip);  // every mapping lands at a new address, and an unmapped one is poisoned
  const auto set = load(moving, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  TtfFont& font = *set->font(0);
  const RunSet& runs = runSetOf(kSubsetFiles[kRegular], 1867);
  ShapingFace* regular = font.shapingFace(kRegular);
  ASSERT_NE(nullptr, regular);
  const ot::Face* before = regular->current();
  ASSERT_NE(nullptr, before);
  const std::vector<std::unique_ptr<ot::Plan>> plans = plansOf(*regular, runs);
  expectPlansShape(*regular, runs, plans);

  const uint32_t mapsBefore = moving.maps();
  ASSERT_NE(nullptr, font.glyph(kBold, kDevanagari[0]));  // maps the Bold block: the Regular's is unmapped
  EXPECT_GT(moving.maps(), mapsBefore);

  const ot::Face* after = regular->current();
  EXPECT_EQ(before, after);
  expectPlansShape(*regular, runs, plans);
  EXPECT_EQ(before, regular->current());
}

TEST_F(TtfShapingFace, MappingAnotherStylesBlockMakesTheNextCallRepoint) {
  SimNorFlash chip{kFlashBytes};
  MovingNorFlash moving(chip);
  const auto set = load(moving, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  TtfFont& font = *set->font(0);
  ShapingFace* regular = font.shapingFace(kRegular);
  ShapingFace* bold = font.shapingFace(kBold);
  ASSERT_NE(nullptr, regular);
  ASSERT_NE(nullptr, bold);
  ASSERT_NE(nullptr, regular->current());
  EXPECT_FALSE(regular->repointed());  // built, not re-pointed
  ASSERT_NE(nullptr, regular->current());
  EXPECT_FALSE(regular->repointed());   // nothing else mapped: no move
  ASSERT_NE(nullptr, bold->current());  // maps the Bold block, unmapping the Regular's
  ASSERT_NE(nullptr, regular->current());
  EXPECT_TRUE(regular->repointed());
  ASSERT_NE(nullptr, regular->current());
  EXPECT_FALSE(regular->repointed());
}

TEST_F(TtfShapingFace, TheFacesCpacIsTheWritersTableInFlashMode) {
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  ShapingFace* face = set->font(0)->shapingFace(kRegular);
  ASSERT_NE(nullptr, face);
  ASSERT_NE(nullptr, face->current());
  const std::vector<uint8_t> table = writerTable(subset[kRegular].bytes);
  ASSERT_GT(table.size(), 0u);
  EXPECT_EQ(table.size(), face->cpacBytes());
}

TEST_F(TtfShapingFace, TheFacesCpacIsTheWritersTableInSdMode) {
  SimNorFlash chip{26 * kPage};
  const auto set = loadSdMode(chip);
  ASSERT_NE(nullptr, set);
  const glyphstore::StyleInfo& info = headerOf(chip).styles[kAccentFirstBlock + kRegular];
  ASSERT_EQ(glyphstore::kModeSd, info.mode);
  ShapingFace* face = set->font(1)->shapingFace(kRegular);
  ASSERT_NE(nullptr, face);
  ASSERT_NE(nullptr, face->current());
  const std::vector<uint8_t> table = writerTable(chipBytes(chip, info.offset, info.size));
  ASSERT_GT(table.size(), 0u);
  EXPECT_EQ(table.size(), face->cpacBytes());
}

TEST_F(TtfShapingFace, ABlockWhoseCpacFailsValidationShapesWithComputedFilters) {
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  const glyphstore::StyleInfo info = headerOf(chip).styles[kRegular];
  const glyphstore::CpacRoom room = glyphstore::cpacRoomOf(info.fileSize, info.size);
  ASSERT_GE(room.bytes, 16u);
  uint8_t* const version = chip.raw() + info.offset + room.at;
  ASSERT_EQ(0, version[0]);
  ASSERT_EQ(1, version[1]);
  version[1] = 2;  // no reinstall: the block is as the install left it, bar its CPac's version
  ShapingFace* face = set->font(0)->shapingFace(kRegular);
  ASSERT_NE(nullptr, face);
  const ot::Face* shaping = face->current();
  ASSERT_NE(nullptr, shaping);
  EXPECT_EQ(nullptr, shaping->filters(ot::GSUB));
  EXPECT_EQ(nullptr, shaping->filters(ot::GPOS));
  expectRunsShape(*face, runSetOf(kSubsetFiles[kRegular], 1867));
}

TEST_F(TtfShapingFace, ASyntheticStyleSharesItsSourceFilesFace) {
  SimNorFlash chip{kFlashBytes};
  MemoryFile regularOnly[4];
  regularOnly[kRegular].bytes = subset[kRegular].bytes;
  const auto set = load(chip, {regularOnly}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  TtfFont& font = *set->font(0);
  ASSERT_TRUE(font.synthetic(kBold));
  ShapingFace* regular = font.shapingFace(kRegular);
  ASSERT_NE(nullptr, regular);
  EXPECT_EQ(regular, font.shapingFace(kBold));
  uint32_t ppem = 0;
  EXPECT_EQ(regular, faceOf(font, kBold, &ppem));  // through the synthetic bold's font
  EXPECT_EQ(1867u, ppem);
}

TEST_F(TtfShapingFace, ALoadedSetHoldsNoFaceUntilAsked) {
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  const size_t loaded = heapBytesHeld;
  ShapingFace* face = set->font(0)->shapingFace(kRegular);
  ASSERT_NE(nullptr, face);
  const size_t faceBytes = heapBytesHeld - loaded;
  EXPECT_GT(faceBytes, 0u);
  EXPECT_LE(faceBytes, 512u);  // about 130 B on the C3
  EXPECT_EQ(face, set->font(0)->shapingFace(kRegular));
  EXPECT_EQ(loaded + faceBytes, heapBytesHeld);  // a second ask holds nothing more
  set->releaseShapingFaces();
  EXPECT_EQ(loaded, heapBytesHeld);
  EXPECT_NE(nullptr, set->font(0)->shapingFace(kRegular));  // made again when asked
  EXPECT_EQ(loaded + faceBytes, heapBytesHeld);
}

TEST_F(TtfShapingFace, AFaceIsMappedAndFreedUnderTheRenderLock) {
  // current() maps the glyph store, and releaseShapingFaces() frees what plans read: both are RenderLock's, as a glyph
  // lookup is, and say so once when it is not held.
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  ShapingFace* face = set->font(0)->shapingFace(kRegular);
  ASSERT_NE(nullptr, face);
  lockChecks = 0;
  TtfFont::Hooks hooks;
  hooks.renderLockHeld = countedLock;
  TtfFont::setHooks(hooks);
  ASSERT_NE(nullptr, face->current());
  EXPECT_EQ(1, lockChecks);
  ASSERT_NE(nullptr, face->current());
  EXPECT_EQ(2, lockChecks);  // every mapping, not only the first
  set->releaseShapingFaces();
  EXPECT_EQ(3, lockChecks);
}

TEST_F(TtfShapingFace, ABitmapFontHasNoShapingFace) {
  CountingProvider provider;
  EpdFontData data{};
  data.provider = &provider;
  const EpdFont font(&data);
  const EpdFontFamily family(&font);
  uint32_t ppem = 7;
  EXPECT_EQ(nullptr, family.shapingFace(EpdFontFamily::REGULAR, &ppem));
  EXPECT_EQ(7u, ppem);
}

// --- A shaped run drawn by glyph ID ---

namespace {

constexpr int kCanvasWidth = 400;
constexpr int kCanvasHeight = 120;
constexpr int kRunX = 20;
constexpr int kBaseline = 80;

// A 2-bit canvas, an ink level (0-3) a byte. A glyph composites by keeping the darker level, so a mark drawn over its
// base shows in both.
struct Canvas {
  std::vector<uint8_t> levels = std::vector<uint8_t>(kCanvasWidth * kCanvasHeight, 0);
  bool clipped = false;  // a glyph reached past the canvas: the comparison would miss those pixels

  // A glyph's 2-bit bitmap (rows back to back, four pixels a byte, the first in the high bits) at (x0, y0).
  void draw(const std::vector<uint8_t>& bitmap, const int width, const int height, const int x0, const int y0) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const int pos = y * width + x;
        const uint8_t level = (bitmap[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3;
        if (level == 0) continue;
        if (x0 + x < 0 || y0 + y < 0 || x0 + x >= kCanvasWidth || y0 + y >= kCanvasHeight) {
          clipped = true;
          continue;
        }
        uint8_t& at = levels[(y0 + y) * kCanvasWidth + x0 + x];
        at = std::max(at, level);
      }
    }
  }
};

// The records of the oracle's run (GlyphRun.h).
std::vector<uint8_t> recordsOf(const ExpectedRun& run) {
  std::vector<uint8_t> bytes(run.count * 6u);
  size_t at = 0;
  for (unsigned i = 0; i < run.count; ++i) {
    GlyphRecord record;
    record.glyph = run.glyphs[i].gid;
    record.advance = run.glyphs[i].advance12_4;
    record.dx = run.glyphs[i].dx;
    record.dy = run.glyphs[i].dy;
    size_t written = 0;
    EXPECT_TRUE(glyph_run::encode(record, bytes.data() + at, bytes.size() - at, &written)) << i;
    at += written;
  }
  bytes.resize(at);
  return bytes;
}

class TtfGlyphRun : public ::testing::Test {
 protected:
  void SetUp() override {
    subset[kRegular].bytes = readFixture(kSubsetFiles[kRegular]);
    subset[kBold].bytes = readFixture(kSubsetFiles[kBold]);
    ASSERT_FALSE(subset[kRegular].bytes.empty());
    ASSERT_FALSE(subset[kBold].bytes.empty());
  }
  void TearDown() override { TtfFont::setHooks({}); }

  // What the rasterizer draws for glyph `gid` of `style`'s file at `ppem`, rendered once.
  const Reference& referenceOf(const uint8_t style, const uint32_t ppem, const uint16_t gid) {
    const uint64_t key = uint64_t{style} << 48 | uint64_t{ppem} << 16 | gid;
    auto it = references.find(key);
    if (it == references.end()) it = references.emplace(key, referenceByGlyph(subset[style].bytes, gid, ppem)).first;
    return it->second;
  }

  MemoryFile subset[4];
  int busy = 0;
  std::map<uint64_t, Reference> references;
};

}  // namespace

// The walk drawGlyphRun drives (forEachGlyph, the unscaled pen) and the lookup it draws with (findGlyphKey) put the
// oracle's glyphs where HarfBuzz puts them. GfxRenderer.cpp does not build on the host; the blit is renderCharImpl's,
// which every line draw already uses, so the canvas composites the looked-up glyph's bitmap at its metrics.
TEST_F(TtfGlyphRun, AGlyphRunDrawsTheOraclesGlyphsAtItsPositions) {
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  TtfFont& font = *set->font(0);
  ASSERT_EQ(1867u, font.bodyPpem());
  const EpdFont* native[4] = {};
  uint32_t nativePpem = 0;
  ASSERT_TRUE(font.nativeFonts(40, native, &nativePpem));
  ASSERT_EQ(2560u, nativePpem);
  const EpdFontFamily body(font.font(kRegular), font.font(kBold));
  const EpdFontFamily large(native[kRegular], native[kBold]);

  int runsDrawn = 0;
  for (const uint8_t style : {kRegular, kBold}) {
    for (const uint32_t ppem : {1867u, 2560u}) {
      const RunSet& runs = runSetOf(kSubsetFiles[style], ppem);
      const EpdFontFamily& family = ppem == 1867u ? body : large;
      const auto fontStyle = static_cast<EpdFontFamily::Style>(style);
      for (size_t r = 0; r < runs.count; ++r) {
        const ExpectedRun& expected = runs.runs[r];
        const std::vector<uint8_t> records = recordsOf(expected);
        const GlyphRun run{records.data(), expected.count, static_cast<uint16_t>(records.size())};

        Canvas drawn;
        forEachGlyph(run, kRunX, false, [&](const GlyphRecord& record, const int penX) {
          const EpdGlyphRef glyph = family.findGlyphKey(glyph_key::of(record.glyph), fontStyle);
          ASSERT_TRUE(static_cast<bool>(glyph)) << "glyph " << record.glyph;
          drawn.draw(bitmapOf(font, style, glyph.sdRecord), glyph.width, glyph.height, penX + record.dx + glyph.left,
                     kBaseline + record.dy - glyph.top);
        });

        Canvas reference;
        int pen = kRunX;
        for (unsigned i = 0; i < expected.count; ++i) {
          const RunGlyph& g = expected.glyphs[i];
          const Reference& ref = referenceOf(style, ppem, g.gid);
          reference.draw(ref.bitmap, ref.metrics.width, ref.metrics.height, pen + g.dx + ref.metrics.left,
                         kBaseline + g.dy - ref.metrics.top);
          pen += fp4::toPixel(g.advance12_4);
        }

        const char* const where = runs.file;
        EXPECT_FALSE(reference.clipped) << where << " at " << ppem << ", run " << r;
        EXPECT_TRUE(std::any_of(reference.levels.begin(), reference.levels.end(), [](const uint8_t l) { return l; }))
            << where << " at " << ppem << ", run " << r << ": no ink";
        EXPECT_TRUE(reference.levels == drawn.levels)
            << where << " at " << ppem << ", run " << r << ": " << expected.utf8;
        EXPECT_EQ(pen, kRunX + glyphRunAdvance(run, false)) << where << " at " << ppem << ", run " << r;
        ++runsDrawn;
      }
    }
  }
  EXPECT_EQ(4 * 16, runsDrawn);
}

// --- A face's own plans ---

namespace {

class ShapingFacePlans : public TtfShapingFace {};

// ot::setHeapCheck's refusal, and its reset when the test ends however it ends.
bool refuseAll(size_t) { return false; }
struct HeapRefusal {
  HeapRefusal() { ot::setHeapCheck(refuseAll); }
  ~HeapRefusal() { ot::setHeapCheck(nullptr); }
};

ot::Script scriptNamed(const char* name) {
  ot::Script script = ot::Script::Devanagari;
  EXPECT_TRUE(ottest::scriptOf(name, &script)) << name;
  return script;
}

std::vector<StoredGlyph> storedOf(const ExpectedShaping& word) {
  std::vector<StoredGlyph> out;
  for (unsigned i = 0; i < word.count; ++i) {
    out.push_back({word.glyphs[i].gid, word.glyphs[i].advance12_4, word.glyphs[i].dx, word.glyphs[i].dy});
  }
  return out;
}

// KA, SSA: the conjunct that differs between the default language system and Marathi's.
constexpr const char* kLaksha = "\xE0\xA4\xB2\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7";

}  // namespace

TEST_F(ShapingFacePlans, APlanIsBuiltOnceAndShapesAsHarfBuzz) {
  for (const ShapingFixture& fx : kShapingFixtures) {
    FixtureShapingFace face(ottest::readFixture(fx.layoutFile));
    const ot::Script script = scriptNamed(fx.script);
    std::vector<const uint32_t*> languages;  // the distinct language systems, in the order the words give them
    for (size_t w = 0; w < fx.count; ++w) {
      const uint32_t* tags = ot::languageTagsFor(fx.words[w].language);
      if (std::find(languages.begin(), languages.end(), tags) == languages.end()) languages.push_back(tags);
    }
    uint16_t builds = 0;
    for (const uint32_t* tags : languages) {
      const ot::Plan* plan = face.plan(script, tags);
      ASSERT_NE(nullptr, plan) << fx.script;
      EXPECT_EQ(plan, face.plan(script, tags)) << fx.script;  // asked again: the same plan, nothing built
      EXPECT_EQ(++builds, face.planBuilds()) << fx.script;
      for (size_t w = 0; w < fx.count; ++w) {
        const ExpectedShaping& word = fx.words[w];
        if (ot::languageTagsFor(word.language) != tags) continue;
        EXPECT_EQ(storedOf(word), ottest::shapeAsStored(*face.current(), *plan, word.utf8))
            << fx.script << " " << w << ": " << word.utf8;
      }
    }
  }
}

TEST_F(ShapingFacePlans, EachLanguageHasItsOwnPlan) {
  FixtureShapingFace face(ottest::readFixture(devanagariFixture().layoutFile));
  const ot::Plan* none = face.plan(ot::Script::Devanagari, ot::languageTagsFor(""));
  const ot::Plan* marathi = face.plan(ot::Script::Devanagari, ot::languageTagsFor("mr"));
  ASSERT_NE(nullptr, none);
  ASSERT_NE(nullptr, marathi);
  EXPECT_NE(none, marathi);
  EXPECT_EQ(2, face.planBuilds());
  const ot::Face* shaping = face.current();
  EXPECT_NE(ottest::shapeAsStored(*shaping, *none, kLaksha), ottest::shapeAsStored(*shaping, *marathi, kLaksha));
  EXPECT_EQ(none, face.plan(ot::Script::Devanagari, ot::languageTagsFor("")));  // both kept
  EXPECT_EQ(marathi, face.plan(ot::Script::Devanagari, ot::languageTagsFor("mr")));
  EXPECT_EQ(2, face.planBuilds());
}

TEST_F(ShapingFacePlans, PastTwoPlansTheLeastRecentGoes) {
  FixtureShapingFace face(ottest::readFixture(devanagariFixture().layoutFile));
  const uint32_t* const none = ot::languageTagsFor("");
  const uint32_t* const marathi = ot::languageTagsFor("mr");
  const uint32_t* const nepali = ot::languageTagsFor("ne");
  ASSERT_NE(none, nepali);
  ASSERT_NE(marathi, nepali);
  ASSERT_EQ(2, ShapingFace::kMaxPlans);
  ASSERT_NE(nullptr, face.plan(ot::Script::Devanagari, none));
  ASSERT_NE(nullptr, face.plan(ot::Script::Devanagari, marathi));
  ASSERT_NE(nullptr, face.plan(ot::Script::Devanagari, nepali));  // frees the plan of "": the least recent
  EXPECT_EQ(3, face.planBuilds());
  ASSERT_NE(nullptr, face.plan(ot::Script::Devanagari, nepali));
  EXPECT_EQ(3, face.planBuilds());
  ASSERT_NE(nullptr, face.plan(ot::Script::Devanagari, none));
  EXPECT_EQ(4, face.planBuilds());
}

TEST_F(ShapingFacePlans, APlanBuiltBeforeAMoveShapesAfterIt) {
  const ShapingFixture& fx = devanagariFixture();
  std::vector<uint8_t> a = ottest::readFixture(fx.layoutFile);
  ASSERT_FALSE(a.empty());
  std::vector<uint8_t> b = a;
  ScriptedFace face;
  face.size = static_cast<uint32_t>(a.size());
  face.bases = {a.data(), b.data()};  // the first ask maps A, every later one B

  const char* const language = fx.words[0].language;
  const uint32_t* const tags = ot::languageTagsFor(language);
  const ot::Plan* const plan = face.plan(ot::Script::Devanagari, tags);
  ASSERT_NE(nullptr, plan);
  std::fill(a.begin(), a.end(), uint8_t{0xA5});              // A is unmapped
  EXPECT_EQ(plan, face.plan(ot::Script::Devanagari, tags));  // re-pointed to B, the same plan
  EXPECT_TRUE(face.repointed());
  EXPECT_EQ(1, face.planBuilds());
  int shaped = 0;
  for (size_t w = 0; w < fx.count; ++w) {
    const ExpectedShaping& word = fx.words[w];
    if (std::strcmp(word.language, language) != 0) continue;
    EXPECT_EQ(storedOf(word), ottest::shapeAsStored(*face.current(), *plan, word.utf8)) << w << ": " << word.utf8;
    ++shaped;
  }
  EXPECT_GT(shaped, 0);
  EXPECT_EQ(1, face.tablesCalls);
}

TEST_F(ShapingFacePlans, AFaceFreedBetweenTwoTokensLeavesNoPlanBehind) {
  SimNorFlash chip{kFlashBytes};
  const auto set = load(chip, {subset}, kIdentityA, busy);
  ASSERT_NE(nullptr, set);
  const RunSet& runs = runSetOf(kSubsetFiles[kRegular], 1867);
  const size_t loaded = heapBytesHeld;

  ShapingFace* face = set->font(0)->shapingFace(kRegular);
  ASSERT_NE(nullptr, face);
  const size_t withFace = heapBytesHeld;
  ASSERT_NE(nullptr, face->plan(ot::Script::Devanagari, ot::languageTagsFor(runs.runs[0].language)));
  EXPECT_GT(heapBytesHeld, withFace);

  set->releaseShapingFaces();
  EXPECT_EQ(loaded, heapBytesHeld);  // the face and its plan both went

  face = set->font(0)->shapingFace(kRegular);
  ASSERT_NE(nullptr, face);
  EXPECT_EQ(0, face->planBuilds());  // a new face has built nothing
  for (size_t r = 0; r < runs.count; ++r) {
    const ExpectedRun& run = runs.runs[r];
    const ot::Plan* const plan = face->plan(ot::Script::Devanagari, ot::languageTagsFor(run.language));
    ASSERT_NE(nullptr, plan) << r;
    EXPECT_EQ(storedOf(run), ottest::shapeAsStored(*face->current(), *plan, run.utf8, runs.ppem26_6))
        << r << ": " << run.utf8;
  }
}

TEST_F(ShapingFacePlans, ARefusedPlanIsAskedAgain) {
  FixtureShapingFace face(ottest::readFixture(devanagariFixture().layoutFile));
  const uint32_t* const tags = ot::languageTagsFor("");
  ASSERT_NE(nullptr, face.current());
  {
    const HeapRefusal refusal;
    EXPECT_EQ(nullptr, face.plan(ot::Script::Devanagari, tags));
    EXPECT_EQ(0, face.planBuilds());
  }
  EXPECT_NE(nullptr, face.plan(ot::Script::Devanagari, tags));
  EXPECT_EQ(1, face.planBuilds());
}

TEST_F(ShapingFacePlans, AnUnmappedFaceGivesNoPlan) {
  FixtureShapingFace face(ottest::readFixture(devanagariFixture().layoutFile));
  face.mapped = false;
  EXPECT_EQ(nullptr, face.plan(ot::Script::Devanagari, ot::languageTagsFor("")));
  face.mapped = true;
  EXPECT_NE(nullptr, face.plan(ot::Script::Devanagari, ot::languageTagsFor("")));
}

TEST_F(ShapingFacePlans, TheFixtureFaceDropsWhatItsOptionsSay) {
  const std::vector<uint8_t> font = ottest::readFixture(devanagariFixture().layoutFile);
  FixtureShapingFace whole(font);
  FixtureShapingFace noCpac(font, {.keepCpac = false});
  FixtureShapingFace noLayout(font, {.keepLayout = false});
  const uint32_t* const tags = ot::languageTagsFor("");
  const ot::Plan* const wholePlan = whole.plan(ot::Script::Devanagari, tags);
  ASSERT_NE(nullptr, wholePlan);
  const std::vector<StoredGlyph> shaped = ottest::shapeAsStored(*whole.current(), *wholePlan, kLaksha);
  const ot::Plan* const noCpacPlan = noCpac.plan(ot::Script::Devanagari, tags);
  ASSERT_NE(nullptr, noCpacPlan);
  EXPECT_EQ(shaped, ottest::shapeAsStored(*noCpac.current(), *noCpacPlan, kLaksha));
  ASSERT_NE(nullptr, noLayout.current());  // a face without layout still stands
  const ot::Plan* const noLayoutPlan = noLayout.plan(ot::Script::Devanagari, tags);
  if (noLayoutPlan) {  // with no lookups to apply, the conjunct stays unshaped
    EXPECT_NE(shaped, ottest::shapeAsStored(*noLayout.current(), *noLayoutPlan, kLaksha));
  }
}
