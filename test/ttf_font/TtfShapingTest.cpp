// TrueType faces that shape (writing modes step 2): what the install writes for the shaper, each style block's CPac.
#include <OtFace.h>
#include <OtFilterWriter.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "GlyphStoreFormat.h"
#include "SimNorFlash.h"
#include "TtfBuilder.h"
#include "TtfFamily.h"
#include "TtfFont.h"
#include "TtfFontTestUtil.h"
#include "TtfLayoutBuilder.h"
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
std::unique_ptr<TtfFontSet> load(SimNorFlash& chip, const std::vector<MemoryFile*>& families, const uint8_t* identity,
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
  const uint32_t at = glyphstore::align4(style.fileSize);
  ASSERT_GE(style.size, at + table.size());
  EXPECT_EQ(table, chipBytes(chip, style.offset + at, static_cast<uint32_t>(table.size())));
  EXPECT_TRUE(erased(chipBytes(chip, style.offset + at + static_cast<uint32_t>(table.size()),
                               style.size - at - static_cast<uint32_t>(table.size()))));
}

// Whether an ot::Face over a flash-mode block, its file's tables and the CPac in its room, takes the CPac's filters.
bool cpacAccepted(const SimNorFlash& chip, const glyphstore::StyleInfo& style) {
  const std::vector<uint8_t> block = chipBytes(chip, style.offset, style.size);
  ot::FaceTables tables;
  EXPECT_TRUE(ot::tablesFromSfnt(block.data(), style.fileSize, &tables));
  const uint32_t at = glyphstore::align4(style.fileSize);
  tables[ot::FaceTables::CPAC] = ot::Table(block.data() + at, style.size - at);
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

// A GPOS of `count` lookups, a pair adjustment subtable each (A kerns B).
Bytes gposOf(const uint16_t count) {
  LayoutLookup lookup;
  PairSubtable pairs;
  pairs.pairs = {{1, {{2, -100}}}};
  lookup.pairSubtables = {pairs};
  LayoutFeature kern{"kern", {}};
  for (uint16_t i = 0; i < count; ++i) kern.lookups.push_back(i);
  return layoutTable({{"latn", {0}}}, {kern}, std::vector<LayoutLookup>(count, lookup));
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

}  // namespace

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
  const uint32_t tableAt = torn.offset + glyphstore::align4(torn.fileSize);
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
  // A file 8 bytes short of a page, with a GPOS of 10 lookups: its CPac (536 B) takes its block to a second page.
  constexpr uint32_t kFileBytes = kPage - 8;
  Font font = boxes();
  font.extraTables = {{"GPOS", gposOf(10)}};
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
  for (const uint32_t word : {1u, 0u, 0u, 0u, 10u}) put16(gsub, word);  // no scripts or features; lookups at 10
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
