// What the registry learns from a TTF file before loading it (its style), and the family identity.
#include <OtFace.h>
#include <OtFilterWriter.h>
#include <OtPlan.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "TtfBuilder.h"
#include "TtfFace.h"
#include "TtfFamily.h"
#include "TtfFontTestUtil.h"
#include "TtfLayoutBuilder.h"
#include "TtfTestUtil.h"

using namespace ttf_test;

namespace {

int probe(const Bytes& bytes) {
  MemoryFile file{bytes};
  return ttf::probeFile(file.source()).style;
}

Bytes builtFont(const uint16_t macStyle = 0) {
  Font font;
  font.macStyle = macStyle;
  font.glyphs.resize(1);
  font.glyphs[0].contours = {rect(0, 0, 64, 64)};
  return buildFont(font);
}

std::array<uint8_t, glyphstore::kIdentityBytes> identityOf(const ttf::FileStamp (&styles)[4]) {
  std::array<uint8_t, glyphstore::kIdentityBytes> out{};
  ttf::familyIdentity(styles, out.data());
  return out;
}

// The identity as docs/file-formats.md describes it, with the render version a parameter.
std::array<uint8_t, glyphstore::kIdentityBytes> identityAt(const ttf::FileStamp (&styles)[4], const uint8_t version) {
  std::array<uint8_t, glyphstore::kIdentityBytes> out{};
  for (uint32_t lane = 0; lane < glyphstore::kIdentityBytes / 4; ++lane) {
    uint32_t hash = 2166136261u ^ (lane * 0x9E3779B9u);
    for (uint8_t style = 0; style < 4; ++style) {
      const ttf::FileStamp& file = styles[style];
      const uint8_t marker = file.name ? style : static_cast<uint8_t>(0x80 | style);
      hash = glyphstore::fnv1a(&marker, 1, hash);
      if (!file.name) continue;
      hash = glyphstore::fnv1a(file.name, std::strlen(file.name) + 1, hash);
      const uint8_t fields[8] = {static_cast<uint8_t>(file.size),       static_cast<uint8_t>(file.size >> 8),
                                 static_cast<uint8_t>(file.size >> 16), static_cast<uint8_t>(file.size >> 24),
                                 static_cast<uint8_t>(file.date),       static_cast<uint8_t>(file.date >> 8),
                                 static_cast<uint8_t>(file.time),       static_cast<uint8_t>(file.time >> 8)};
      hash = glyphstore::fnv1a(fields, sizeof fields, hash);
    }
    hash = glyphstore::fnv1a(&version, 1, hash);
    for (int byte = 0; byte < 4; ++byte) out[4 * lane + byte] = static_cast<uint8_t>(hash >> (8 * byte));
  }
  return out;
}

}  // namespace

TEST(TtfFamily, TheBundledBookerlyFilesDeclareTheirStyles) {
  EXPECT_EQ(0, probe(readFont("Bookerly/Bookerly-Regular.ttf")));
  EXPECT_EQ(1, probe(readFont("Bookerly/Bookerly-Bold.ttf")));
  EXPECT_EQ(2, probe(readFont("Bookerly/Bookerly-Italic.ttf")));
  EXPECT_EQ(3, probe(readFont("Bookerly/Bookerly-BoldItalic.ttf")));
}

TEST(TtfFamily, TheStyleIsTheLowTwoBitsOfMacStyle) {
  EXPECT_EQ(0, probe(builtFont(0)));
  EXPECT_EQ(1, probe(builtFont(1)));
  EXPECT_EQ(2, probe(builtFont(2)));
  EXPECT_EQ(3, probe(builtFont(3 | 0x40)));  // other macStyle bits (condensed, ...) do not matter
}

TEST(TtfFamily, ACffFontIsAFamilyAndACollectionIsNot) {
  // The style is head.macStyle's; OpenDyslexic's Italic leaves it clear.
  const char* const names[] = {"Regular", "Bold", "Italic", "BoldItalic"};
  const int styles[] = {0, 1, 0, 3};
  for (int i = 0; i < 4; ++i) {
    MemoryFile file{readFont(std::string("OpenDyslexic/OpenDyslexic-") + names[i] + ".otf")};
    const ttf::FileProbe probed = ttf::probeFile(file.source());
    EXPECT_EQ(styles[i], probed.style) << names[i];
    // Its SD-mode block, the CFF table rebuilt in it, as an install plans it.
    ttf::SdBlock block;
    ASSERT_EQ(ttf::SdPlan::Ok, block.plan(file.source())) << names[i];
    EXPECT_EQ(block.bytes(), probed.sdBytes) << names[i];
    EXPECT_LT(probed.sdBytes, file.bytes.size()) << names[i];
  }
  // A CFF table the engine cannot read (major version 2): parseFace() refuses the face, so the load could draw it in
  // neither mode, and the file is not listed.
  Bytes unread = readFont("OpenDyslexic/OpenDyslexic-Regular.otf");
  const size_t record = findRecord(unread, "CFF ") + 8;  // its offset
  unread[static_cast<size_t>(unread[record]) << 24 | unread[record + 1] << 16 | unread[record + 2] << 8 |
         unread[record + 3]] = 2;
  FontBytes unreadBytes(unread.data(), static_cast<uint32_t>(unread.size()));
  ttf::Face face;
  EXPECT_EQ(ttf::FaceStatus::Malformed, ttf::parseFace(unreadBytes, &face));
  MemoryFile broken{unread};
  EXPECT_EQ(-1, ttf::probeFile(broken.source()).style);
  Bytes collection = builtFont();
  set32(collection, 0, 0x74746366);  // 'ttcf'
  EXPECT_EQ(-1, probe(collection));
}

TEST(TtfFamily, ACffFontMissingATableTheEngineNeedsIsRefused) {
  for (const char* table : {"CFF ", "cmap", "head", "hhea", "hmtx", "maxp"}) {
    Bytes font = readFont("OpenDyslexic/OpenDyslexic-Regular.otf");
    const size_t record = findRecord(font, table);
    ASSERT_NE(0u, record) << table;
    font[record] = 'X';
    EXPECT_EQ(-1, probe(font)) << table;
  }
  // glyf and loca are TrueType's; a CFF font does not need them, and lists with them too.
  EXPECT_EQ(0, probe(readFont("OpenDyslexic/OpenDyslexic-Regular.otf")));
}

namespace {

// A copy of the OpenDyslexic font whose GDEF and GPOS records are renamed `first` and `second`.
Bytes cffFontWithRecords(const char* first, const char* second) {
  Bytes font = readFont("OpenDyslexic/OpenDyslexic-Regular.otf");
  std::memcpy(font.data() + findRecord(font, "GDEF"), first, 4);
  std::memcpy(font.data() + findRecord(font, "GPOS"), second, 4);
  return font;
}

// A TrueType font with a "CFF " record of `length` bytes at `offset`.
Bytes trueTypeFontWithCffRecord(const uint32_t offset, const uint32_t length) {
  Font font;
  font.glyphs.resize(1);
  font.glyphs[0].contours = {rect(0, 0, 64, 64)};
  font.extraTables = {{"CFF ", Bytes(16, 0)}};
  Bytes bytes = buildFont(font);
  set32(bytes, findRecord(bytes, "CFF ") + 8, offset);
  set32(bytes, findRecord(bytes, "CFF ") + 12, length);
  return bytes;
}

}  // namespace

TEST(TtfFamily, ATrueTypeFontWithACffRecordIsStillListed) {
  // parseFace() reads a TrueType font's glyf and loca whatever else the directory holds.
  Font font;
  font.glyphs.resize(1);
  font.glyphs[0].contours = {rect(0, 0, 64, 64)};
  font.extraTables = {{"CFF ", Bytes(16, 0)}};
  EXPECT_EQ(0, probe(buildFont(font)));
}

TEST(TtfFamily, ACffFontWithGlyfAndLocaRecordsIsListed) {
  const Bytes font = cffFontWithRecords("glyf", "loca");
  EXPECT_NE(0u, findRecord(font, "glyf"));
  EXPECT_NE(0u, findRecord(font, "loca"));
  EXPECT_EQ(0, probe(font));
  MemoryFile file{font};
  EXPECT_NE(UINT32_MAX, ttf::probeFile(file.source()).sdBytes) << "its SD-mode block is still planned";
}

TEST(TtfFamily, AnOutOfFileRecordTheKindDoesNotNeedDoesNotRefuse) {
  const uint32_t past = 1u << 30;
  // TrueType with a CFF record outside the file: the CFF table is not read.
  EXPECT_EQ(0, probe(trueTypeFontWithCffRecord(past, 16)));
  EXPECT_EQ(0, probe(trueTypeFontWithCffRecord(0, past)));
  // CFF with glyf and loca records outside the file.
  Bytes font = cffFontWithRecords("glyf", "loca");
  set32(font, findRecord(font, "glyf") + 8, past);
  set32(font, findRecord(font, "loca") + 12, past);
  EXPECT_EQ(0, probe(font));
  // What the kind does need still refuses.
  Bytes noGlyf = builtFont();
  set32(noGlyf, findRecord(noGlyf, "glyf") + 8, past);
  EXPECT_EQ(-1, probe(noGlyf));
}

TEST(TtfFamily, ACffTableOutsideTheFileIsRefused) {
  Bytes font = readFont("OpenDyslexic/OpenDyslexic-Regular.otf");
  set32(font, findRecord(font, "CFF ") + 12, static_cast<uint32_t>(font.size()));
  EXPECT_EQ(-1, probe(font));
}

TEST(TtfFamily, ATrueTypeFontWithoutGlyfIsRefusedWhateverElseItHas) {
  Bytes font = readFont("OpenDyslexic/OpenDyslexic-Regular.otf");
  set32(font, 0, 0x00010000);  // sfnt 1.0 over CFF tables: glyf and loca are what it needs
  EXPECT_EQ(-1, probe(font));
}

TEST(TtfFamily, ACff2RecordIsReadAsTheFaceReadsIt) {
  // Any table slot, renamed CFF2: a real one makes the face Unsupported (so the probe says -1), an empty one or one
  // outside the file is skipped and the CFF table draws.
  const Bytes original = readFont("OpenDyslexic/OpenDyslexic-Regular.otf");
  const size_t record = findRecord(original, "OS/2");
  ASSERT_NE(0u, record);
  Bytes real = original;
  std::memcpy(real.data() + record, "CFF2", 4);
  EXPECT_EQ(-1, probe(real));
  Bytes empty = real;
  set32(empty, record + 12, 0);
  EXPECT_EQ(0, probe(empty));
  Bytes outside = real;
  set32(outside, record + 8, static_cast<uint32_t>(outside.size()) + 4);
  EXPECT_EQ(0, probe(outside));
}

TEST(TtfFamily, AFontMissingATableTheEngineNeedsIsRefused) {
  for (const char* table : {"cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp"}) {
    Bytes font = builtFont();
    const size_t record = findRecord(font, table);
    ASSERT_NE(0u, record) << table;
    font[record] = 'X';  // the table is no longer in the directory
    EXPECT_EQ(-1, probe(font)) << table;
  }
}

TEST(TtfFamily, ATableOutsideTheFileIsRefused) {
  Bytes font = builtFont();
  const size_t record = findRecord(font, "glyf");
  set32(font, record + 12, static_cast<uint32_t>(font.size()));  // its length runs past the end
  EXPECT_EQ(-1, probe(font));
}

TEST(TtfFamily, ATruncatedFileIsRefused) {
  const Bytes font = builtFont();
  for (const size_t keep : {size_t{0}, size_t{11}, size_t{20}, font.size() / 2, font.size() - 1}) {
    EXPECT_EQ(-1, probe(Bytes(font.begin(), font.begin() + static_cast<std::ptrdiff_t>(keep)))) << keep;
  }
}

TEST(TtfFamily, AHeadWithoutItsMagicIsRefused) {
  Bytes font = builtFont();
  const size_t record = findRecord(font, "head");
  const uint32_t head = static_cast<uint32_t>(font[record + 8] << 24 | font[record + 9] << 16 | font[record + 10] << 8 |
                                              font[record + 11]);
  set32(font, head + 12, 0x12345678);
  EXPECT_EQ(-1, probe(font));
}

// --- What parseFace() refuses, the probe refuses: the load would install the family at every pick, then fall back ---

namespace {

ttf::FaceStatus parsed(const Bytes& bytes) {
  FontBytes font(bytes.data(), static_cast<uint32_t>(bytes.size()));
  ttf::Face face;
  return ttf::parseFace(font, &face);
}

// Where the table `name` starts in `font`, from its directory record.
uint32_t tableAt(const Bytes& font, const char* name) {
  const size_t record = findRecord(font, name);
  return static_cast<uint32_t>(font[record + 8]) << 24 | font[record + 9] << 16 | font[record + 10] << 8 |
         font[record + 11];
}

// A font whose one glyph a symbol font would map at U+F041, with the character map `cmap` holds.
Bytes symbolFont(const bool unicodeMap) {
  Font font;
  font.glyphs.resize(2);
  font.glyphs[1].contours = {rect(0, 0, 64, 64)};
  font.cmap = {{0xF041, 1}};
  if (!unicodeMap) font.extraTables = {{"cmap", cmapTable({{3, 0, cmapFormat4(font.cmap, false)}})}};
  return buildFont(font);
}

}  // namespace

TEST(TtfFamily, AFontWithOnlyASymbolMapIsRefused) {
  // A (3,0) symbol map is no Unicode map parseFace() reads: the face is Unsupported, and its file is not listed.
  const Bytes symbol = symbolFont(false);
  EXPECT_EQ(ttf::FaceStatus::Unsupported, parsed(symbol));
  EXPECT_EQ(-1, probe(symbol));
  const Bytes unicode = symbolFont(true);  // the same glyph under a (3,1) map
  EXPECT_EQ(ttf::FaceStatus::Ok, parsed(unicode));
  EXPECT_EQ(0, probe(unicode));
}

TEST(TtfFamily, AHeadHheaOrMaxpTheFaceRefusesIsRefused) {
  struct Break {
    const char* what;
    const char* table;
    uint32_t at;
    uint8_t bytes;
    uint32_t value;
  };
  const Break breaks[] = {
      {"unitsPerEm 15", "head", 18, 2, 15},
      {"unitsPerEm 16385", "head", 18, 2, 16385},
      {"indexToLocFormat 2", "head", 50, 2, 2},
      {"numHMetrics 0", "hhea", 34, 2, 0},
      {"maxp 0.5, the CFF form, over TrueType outlines", "maxp", 0, 4, 0x00005000},
      {"maxp 2.0", "maxp", 0, 4, 0x00020000},
      {"no glyphs", "maxp", 4, 2, 0},
  };
  const Bytes good = builtFont();
  ASSERT_EQ(ttf::FaceStatus::Ok, parsed(good));
  ASSERT_EQ(0, probe(good));
  for (const Break& broken : breaks) {
    Bytes font = good;
    const uint32_t at = tableAt(font, broken.table) + broken.at;
    if (broken.bytes == 2) {
      set16(font, at, broken.value);
    } else {
      set32(font, at, broken.value);
    }
    EXPECT_EQ(ttf::FaceStatus::Malformed, parsed(font)) << broken.what;
    EXPECT_EQ(-1, probe(font)) << broken.what;
  }
}

TEST(TtfFamily, AStyleTheFaceRefusesLeavesItsPlaceToTheOthers) {
  // A folder whose regular file has only a symbol map: the bold serves as the regular, as when a folder has no regular
  // file, rather than a family whose regular the load refuses.
  const Bytes bold = builtFont(1);
  MemoryFile boldFile{bold};
  MemoryFile symbolFile{symbolFont(false)};
  const ttf::FileProbe files[] = {ttf::probeFile(boldFile.source()), ttf::probeFile(symbolFile.source())};
  ASSERT_EQ(1, files[0].style);
  ASSERT_EQ(-1, files[1].style);
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(files, 2, chosen));
  EXPECT_EQ(0, chosen[0]);
  EXPECT_EQ(-1, chosen[1]);
}

TEST(TtfFamily, AReadErrorIsRefused) {
  MemoryFile file{readFont("Bookerly/Bookerly-Bold.ttf")};
  file.failReads = true;
  EXPECT_EQ(-1, ttf::probeFile(file.source()).style);
}

TEST(TtfFamily, TheProbeCountsLookupsInCoalescedReads) {
  // Noto Sans draws Devanagari too: its GSUB and GPOS name dev2 and deva, so it can shape and its lookups are counted.
  MemoryFile file{readFont("NotoSans/NotoSans-Regular.ttf")};
  const ttf::FileProbe probed = ttf::probeFile(file.source());
  ASSERT_EQ(0, probed.style);
  EXPECT_GT(probed.cpacBytes, 0u);
  const uint32_t probeReads = file.reads;
  // The header, the directory 8 records at a time, head and OS/2 (5 reads), then per table its first 128 bytes (its
  // header, and GSUB's script list behind it), its LookupList count, the lookups' offsets 64 at a time and a window of
  // headers. The lookups are packed, so a window serves many: well under a read per lookup (about 311 reads when each
  // header was read alone). Measured: 44, and 4 more for parseFace()'s pages (48).
  uint32_t lookups = 0;
  for (const char* table : {"GSUB", "GPOS"}) {
    const size_t at = findRecord(file.bytes, table);
    const uint32_t offset = static_cast<uint32_t>(file.bytes[at + 8]) << 24 | file.bytes[at + 9] << 16 |
                            file.bytes[at + 10] << 8 | file.bytes[at + 11];
    const uint32_t length = static_cast<uint32_t>(file.bytes[at + 12]) << 24 | file.bytes[at + 13] << 16 |
                            file.bytes[at + 14] << 8 | file.bytes[at + 15];
    ttf::LayoutCounts counts;
    ASSERT_TRUE(ttf::countLookups(file.source(), offset, length, &counts));
    lookups += counts.lookups;
  }
  EXPECT_GT(lookups, 100u);
  EXPECT_LE(probeReads, 50u);
}

TEST(TtfFamily, ALatinFontsProbeReadsItsScriptListsAndNoLookups) {
  // Bookerly's GSUB and GPOS name DFLT, cyrl, grek and latn only: no script the shaper handles, so it reserves no CPac
  // and its lookups are never counted. The header, the directory 8 records at a time (21 tables: 3 reads), head and
  // OS/2, which is what the probe read before the CPac (6); then the first 128 bytes of each layout table, its header
  // and its script list (2); last parseFace()'s reading of the face, the 512-byte pages that hold the directory and
  // the tables it reads (4).
  MemoryFile file{readFont("Bookerly/Bookerly-Regular.ttf")};
  const ttf::FileProbe probed = ttf::probeFile(file.source());
  ASSERT_EQ(0, probed.style);
  EXPECT_EQ(0u, probed.cpacBytes);
  EXPECT_EQ(12u, file.reads);
  // One read in each layout table, at its start.
  for (const char* table : {"GSUB", "GPOS"}) {
    const size_t at = findRecord(file.bytes, table);
    const uint32_t offset = static_cast<uint32_t>(file.bytes[at + 8]) << 24 | file.bytes[at + 9] << 16 |
                            file.bytes[at + 10] << 8 | file.bytes[at + 11];
    const uint32_t length = static_cast<uint32_t>(file.bytes[at + 12]) << 24 | file.bytes[at + 13] << 16 |
                            file.bytes[at + 14] << 8 | file.bytes[at + 15];
    std::vector<uint32_t> inside;
    for (const uint32_t read : file.offsets) {
      if (read >= offset && read < offset + length) inside.push_back(read);
    }
    EXPECT_EQ(std::vector<uint32_t>{offset}, inside) << table;
  }
}

TEST(TtfFamily, AFileSourceReadsThroughSeekAndRead) {
  struct SeekReadFile {  // HalFile's two calls
    std::vector<uint8_t> bytes;
    size_t pos = 0;
    bool seekSet(const size_t to) {
      if (to > bytes.size()) return false;
      pos = to;
      return true;
    }
    int read(void* dst, size_t n) {
      n = std::min(n, bytes.size() - pos);
      std::memcpy(dst, bytes.data() + pos, n);
      pos += n;
      return static_cast<int>(n);
    }
  };
  SeekReadFile file{readFont("Bookerly/Bookerly-Italic.ttf")};
  EXPECT_EQ(2, ttf::probeFile(ttf::fileSource(file, static_cast<uint32_t>(file.bytes.size()))).style);
  const auto size = static_cast<uint32_t>(file.bytes.size());
  file.bytes.resize(100);  // the file shrank under the source: short reads fail
  EXPECT_EQ(-1, ttf::probeFile(ttf::fileSource(file, size)).style);
}

TEST(TtfFamily, TheIdentityChangesWithAnyFileDetail) {
  const ttf::FileStamp base[4] = {{"Lit-Regular.ttf", 450048, 0x5A21, 0x6400},
                                  {"Lit-Bold.ttf", 452000, 0x5A21, 0x6400}};
  const auto identity = identityOf(base);
  EXPECT_EQ(identity, identityOf(base));

  ttf::FileStamp renamed[4] = {base[0], base[1]};
  renamed[1].name = "Lit-Heavy.ttf";
  ttf::FileStamp resized[4] = {base[0], base[1]};
  resized[0].size += 1;
  ttf::FileStamp redated[4] = {base[0], base[1]};
  redated[0].date += 1;
  ttf::FileStamp retimed[4] = {base[0], base[1]};
  retimed[1].time += 2;
  ttf::FileStamp moved[4] = {base[0], {}, base[1]};  // the second file now serves italic, not bold
  ttf::FileStamp fewer[4] = {base[0]};
  for (const auto* other : {&renamed, &resized, &redated, &retimed, &moved, &fewer}) {
    EXPECT_NE(identity, identityOf(*other));
  }
}

TEST(TtfFamily, TheIdentityChangesWithTheRenderVersion) {
  // Writing modes step 2: a store written before holds no CPac in its style blocks; at version 4 it is installed again.
  const ttf::FileStamp styles[4] = {{"Lit-Regular.ttf", 450048, 0x5A21, 0x6400}};
  EXPECT_EQ(4, ttf::kRenderVersion);
  EXPECT_EQ(identityAt(styles, 4), identityOf(styles));
  EXPECT_NE(identityAt(styles, 3), identityOf(styles));
}

TEST(TtfFamily, TheIdentityIsSixteenIndependentBytes) {
  const ttf::FileStamp styles[4] = {{"A.ttf", 1, 2, 3}};
  const auto identity = identityOf(styles);
  // Four lanes with different seeds: no lane repeats another.
  for (int a = 0; a < 4; ++a) {
    for (int b = a + 1; b < 4; ++b) {
      EXPECT_NE(0, std::memcmp(identity.data() + 4 * a, identity.data() + 4 * b, 4)) << a << " " << b;
    }
  }
  EXPECT_EQ(glyphstore::fnv1a(identity.data(), identity.size()), ttf::identityHash(identity.data()));
}

TEST(TtfFamily, ASetsIdentityChangesWithItsFamiliesAndTheirOrder) {
  const uint8_t families[3][glyphstore::kIdentityBytes] = {{'b', 'o', 'd', 'y'}, {'a', 'c', 'c'}, {'o', 't', 'h'}};
  const uint8_t swapped[2][glyphstore::kIdentityBytes] = {{'a', 'c', 'c'}, {'b', 'o', 'd', 'y'}};
  const auto identityOfSet = [](const uint8_t (*set)[glyphstore::kIdentityBytes], const uint8_t count) {
    std::array<uint8_t, glyphstore::kIdentityBytes> out{};
    ttf::setIdentity(set, count, out.data());
    return out;
  };
  const auto pair = identityOfSet(families, 2);
  EXPECT_EQ(pair, identityOfSet(families, 2));
  EXPECT_NE(pair, identityOfSet(families, 1));  // the body alone
  EXPECT_NE(pair, identityOfSet(families, 3));  // another accent family beside it
  EXPECT_NE(pair, identityOfSet(swapped, 2));   // a family's index is part of every key it stored
  EXPECT_NE(identityOfSet(families, 1), identityOfSet(families + 1, 1));
}

TEST(TtfFamily, OutlineFontFileNamesAreRecognisedInAnyCase) {
  EXPECT_TRUE(ttf::isOutlineFontFileName("Literata-Regular.ttf"));
  EXPECT_TRUE(ttf::isOutlineFontFileName("LITERATA.TTF"));
  EXPECT_TRUE(ttf::isOutlineFontFileName("x.TtF"));
  EXPECT_TRUE(ttf::isOutlineFontFileName("Literata.otf"));
  EXPECT_TRUE(ttf::isOutlineFontFileName("LITERATA.OTF"));
  EXPECT_TRUE(ttf::isOutlineFontFileName("x.OtF"));
  EXPECT_FALSE(ttf::isOutlineFontFileName(".ttf"));  // no name
  EXPECT_FALSE(ttf::isOutlineFontFileName(".otf"));
  EXPECT_FALSE(ttf::isOutlineFontFileName("Literata.ttf.tmp"));  // an upload in progress
  EXPECT_FALSE(ttf::isOutlineFontFileName("Literata.otf.tmp"));
  EXPECT_FALSE(ttf::isOutlineFontFileName("Literata_14.cpfont"));
  EXPECT_FALSE(ttf::isOutlineFontFileName("Literatattf"));
  EXPECT_FALSE(ttf::isOutlineFontFileName("Literataotf"));
  EXPECT_FALSE(ttf::isOutlineFontFileName("Literata.ttc"));
}

TEST(TtfFamily, AmongEqualWeightsTheFirstFileServes) {
  // Sorted by name: Bold, BoldCondensed (also bold), Italic, Regular.
  const ttf::FileProbe files[] = {{1, 700}, {1, 700}, {2, 400}, {0, 400}};
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(files, 4, chosen));
  EXPECT_EQ(3, chosen[0]);
  EXPECT_EQ(0, chosen[1]);  // not the condensed one that sorts after it
  EXPECT_EQ(2, chosen[2]);
  EXPECT_EQ(-1, chosen[3]);  // no bold italic: it falls back
}

TEST(TtfFamily, TheWeightClosestToRegularOrBoldServesEachStyle) {
  // A folder of static weights, sorted by name. Only Bold and BoldItalic set macStyle's bold bit, as Google
  // Fonts' static files do, so every other weight reads as regular or italic.
  const ttf::FileProbe files[] = {
      {3, 900},  // BlackItalic, which marks itself bold
      {1, 700},  // Bold
      {3, 700},  // BoldItalic
      {0, 800},  // ExtraBold
      {2, 300},  // LightItalic
      {0, 300},  // Light
      {2, 400},  // Italic
      {0, 400},  // Regular
  };
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(files, 8, chosen));
  EXPECT_EQ(7, chosen[0]);  // Regular, not the ExtraBold that sorts first
  EXPECT_EQ(1, chosen[1]);
  EXPECT_EQ(6, chosen[2]);  // Italic, not LightItalic
  EXPECT_EQ(2, chosen[3]);  // BoldItalic at 700, not 900
}

TEST(TtfFamily, WithoutAnExactWeightTheNearestServes) {
  const ttf::FileProbe files[] = {{0, 800}, {0, 300}};  // ExtraBold, Light
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(files, 2, chosen));
  EXPECT_EQ(1, chosen[0]);  // 300 is nearer 400 than 800 is
}

TEST(TtfFamily, AStaticFileWinsOverAVariableOne) {
  const ttf::FileProbe files[] = {{0, 400, true}, {0, 500}};  // the variable font sorts first
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(files, 2, chosen));
  EXPECT_EQ(1, chosen[0]);
  const ttf::FileProbe onlyVariable[] = {{0, 400, true}};
  ASSERT_TRUE(ttf::assignStyles(onlyVariable, 1, chosen));
  EXPECT_EQ(0, chosen[0]);  // drawn as its default instance (spec 5.6)
}

TEST(TtfFamily, FilesTheEngineCannotDrawAreIgnored) {
  const ttf::FileProbe files[] = {{-1}, {0}, {-1}};
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(files, 3, chosen));
  EXPECT_EQ(1, chosen[0]);
}

TEST(TtfFamily, WithoutARegularFileTheNearestStyleServesAsRegular) {
  // A script embedded as its italic alone, a display face as its bold alone: drawn in that face, held once.
  const ttf::FileProbe italicOnly[] = {{2}};
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(italicOnly, 1, chosen));
  EXPECT_EQ(0, chosen[0]);
  EXPECT_EQ(-1, chosen[2]);  // falls back to the regular, the same face

  const ttf::FileProbe boldOnly[] = {{1, 700}};
  ASSERT_TRUE(ttf::assignStyles(boldOnly, 1, chosen));
  EXPECT_EQ(0, chosen[0]);
  EXPECT_EQ(-1, chosen[1]);

  const ttf::FileProbe noRegular[] = {{1}, {2}, {3}, {-1}};  // italic first, then bold, then bold italic
  ASSERT_TRUE(ttf::assignStyles(noRegular, 4, chosen));
  EXPECT_EQ(1, chosen[0]);
  EXPECT_EQ(0, chosen[1]);
  EXPECT_EQ(-1, chosen[2]);
  EXPECT_EQ(2, chosen[3]);
}

TEST(TtfFamily, ADirectoryWithNothingTheEngineDrawsIsNoFamily) {
  const ttf::FileProbe files[] = {{-1}, {-1}};
  int chosen[4];
  EXPECT_FALSE(ttf::assignStyles(files, 2, chosen));
  EXPECT_FALSE(ttf::assignStyles(nullptr, 0, chosen));
}

TEST(TtfFamily, TheProbeReadsTheWeightClassAndSpotsVariableFonts) {
  Font font;
  font.glyphs.resize(1);
  font.glyphs[0].contours = {rect(0, 0, 64, 64)};
  font.os2 = true;
  font.weightClass = 800;
  MemoryFile extraBold{buildFont(font)};
  ttf::FileProbe probe = ttf::probeFile(extraBold.source());
  EXPECT_EQ(0, probe.style);
  EXPECT_EQ(800, probe.weight);
  EXPECT_FALSE(probe.variable);

  font.extraTables = {{"fvar", Bytes(16, 0)}};
  MemoryFile variable{buildFont(font)};
  EXPECT_TRUE(ttf::probeFile(variable.source()).variable);

  font.os2 = false;  // no OS/2 table: the weight follows the style
  font.extraTables.clear();
  font.macStyle = 1;
  MemoryFile bold{buildFont(font)};
  probe = ttf::probeFile(bold.source());
  EXPECT_EQ(1, probe.style);
  EXPECT_EQ(700, probe.weight);
}

TEST(TtfFamily, ABoldWeightDeclaringRegularServesBold) {
  // Inter's static Bold marks itself bold only by its OS/2 weight class: macStyle 0, fsSelection REGULAR.
  MemoryFile interRegular{readFont("Inter/Inter-Regular.ttf")};
  MemoryFile interBold{readFont("Inter/Inter-Bold.ttf")};
  const ttf::FileProbe inter[] = {ttf::probeFile(interBold.source()), ttf::probeFile(interRegular.source())};
  ASSERT_EQ(0, inter[0].style);
  ASSERT_EQ(700, inter[0].weight);
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(inter, 2, chosen));
  EXPECT_EQ(1, chosen[0]);  // Regular, though Bold sorts first
  EXPECT_EQ(0, chosen[1]);

  const ttf::FileProbe italics[] = {{0, 400}, {2, 400}, {2, 700}};  // the same for an italic
  ASSERT_TRUE(ttf::assignStyles(italics, 3, chosen));
  EXPECT_EQ(1, chosen[2]);
  EXPECT_EQ(2, chosen[3]);
  EXPECT_EQ(-1, chosen[1]);
}

TEST(TtfFamily, AFileDeclaringBoldWinsOverOneOfTheSameWeightThatDoesNot) {
  const ttf::FileProbe files[] = {{0, 700}, {1, 700}, {0, 400}};
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(files, 3, chosen));
  EXPECT_EQ(2, chosen[0]);
  EXPECT_EQ(1, chosen[1]);
}

TEST(TtfFamily, ASingleHeavyFaceIsARegularWithoutBold) {
  const ttf::FileProbe display[] = {{0, 900}};  // a display family with one Black face
  int chosen[4];
  ASSERT_TRUE(ttf::assignStyles(display, 1, chosen));
  EXPECT_EQ(0, chosen[0]);
  EXPECT_EQ(-1, chosen[1]);  // falls back to the regular, not the same file twice in the store

  const ttf::FileProbe semiAndBlack[] = {{0, 650}, {0, 900}};  // the heavier one is still free to be the bold
  ASSERT_TRUE(ttf::assignStyles(semiAndBlack, 2, chosen));
  EXPECT_EQ(0, chosen[0]);
  EXPECT_EQ(1, chosen[1]);
}

TEST(TtfFamily, TheBundledFilesCarryTheirWeights) {
  MemoryFile regular{readFont("NotoSans/NotoSans-Regular.ttf")};
  MemoryFile bold{readFont("NotoSans/NotoSans-Bold.ttf")};
  EXPECT_EQ(400, ttf::probeFile(regular.source()).weight);
  EXPECT_EQ(700, ttf::probeFile(bold.source()).weight);
}

TEST(TtfFamily, AFamilyFitsTheStoreInFlashModeElseInSdMode) {
  constexpr uint32_t kX3FontArea = 3538944 - 65536;
  const uint32_t bookerly[4] = {450048, 452000, 460000, 455000};
  const uint32_t bookerlyTables[4] = {176356, 178372, 170664, 184176};
  EXPECT_TRUE(ttf::fitsStore(bookerly, bookerlyTables, 4, kX3FontArea, 32));
  // One CJK style is larger than the partition; its tables are not (DroidSansFallbackFull: 4 MB, 357 KB of tables).
  const uint32_t cjk[1] = {10u * 1024 * 1024};
  const uint32_t cjkTables[1] = {356752};
  EXPECT_TRUE(ttf::fitsStore(cjk, cjkTables, 1, kX3FontArea, 32));
  const uint32_t noTables[1] = {0};  // a file whose SD-mode block cannot be made
  EXPECT_FALSE(ttf::fitsStore(cjk, noTables, 1, kX3FontArea, 32));
  // Four styles of 1 MB with 450 KB of tables each: too large whole, and their tables, mapped together, pass the
  // mapping budget.
  const uint32_t wide[4] = {1000000, 1000000, 1000000, 1000000};
  const uint32_t wideTables[4] = {450000, 450000, 450000, 450000};
  EXPECT_FALSE(ttf::fitsStore(wide, wideTables, 4, kX3FontArea, 32));
  EXPECT_FALSE(ttf::fitsStore(bookerly, bookerlyTables, 4, kX3FontArea, 12));  // too few mapping pages either way
  EXPECT_FALSE(ttf::fitsStore(bookerly, bookerlyTables, 0, kX3FontArea, 32));
}

TEST(TtfFamily, AStyleWithoutAnSdBlockFitsOnlyInFlashMode) {
  constexpr uint32_t kX3FontArea = 3538944 - 65536;
  const uint32_t none[1] = {UINT32_MAX};  // a CFF file: no SD-mode block yet
  const uint32_t small[1] = {1000000};
  EXPECT_TRUE(ttf::fitsStore(small, none, 1, kX3FontArea, 32));
  const uint32_t big[1] = {10u * 1024 * 1024};
  EXPECT_FALSE(ttf::fitsStore(big, none, 1, kX3FontArea, 32));  // not past a wrapped page count
  const uint32_t mixed[2] = {450000, 1000000};
  const uint32_t mixedTables[2] = {176356, UINT32_MAX};
  EXPECT_TRUE(ttf::fitsStore(mixed, mixedTables, 2, kX3FontArea, 32));
  const uint32_t mixedBig[2] = {450000, 10u * 1024 * 1024};
  EXPECT_FALSE(ttf::fitsStore(mixedBig, mixedTables, 2, kX3FontArea, 32));
}

TEST(TtfFamily, TheProbeSaysWhatTheSdModeBlockTakes) {
  for (const char* name : {"Bookerly/Bookerly-Regular.ttf", "Inter/Inter-Bold.ttf", "NotoSans/NotoSans-Italic.ttf",
                           "Ubuntu/Ubuntu-Regular.ttf"}) {
    MemoryFile file{readFont(name)};
    ttf::SdBlock block;
    ASSERT_EQ(ttf::SdPlan::Ok, block.plan(file.source())) << name;
    EXPECT_EQ(block.bytes(), ttf::probeFile(file.source()).sdBytes) << name;
  }
}

// --- SD mode's style block ---

namespace {

// The whole block, read through its source.
std::vector<uint8_t> blockImage(ttf::SdBlock& block) {
  std::vector<uint8_t> image(block.bytes());
  const ttf::FileSource source = block.source();
  EXPECT_TRUE(source.read(source.ctx, 0, image.data(), block.bytes()));
  return image;
}

}  // namespace

TEST(TtfSdBlock, IsTheFontButForItsOutlines) {
  MemoryFile file{readFont("Bookerly/Bookerly-Regular.ttf")};
  ttf::SdBlock block;
  ASSERT_EQ(ttf::SdPlan::Ok, block.plan(file.source()));
  // Its eleven tables the engine reads, post's header among them, and GDEF, 4-byte aligned behind a 13-record
  // directory (no CPac: Bookerly's layout names no script the shaper handles); glyf is 208,484 bytes at 58,840 in the
  // file.
  EXPECT_EQ(177004u, block.bytes());
  EXPECT_EQ(0u, block.cpacBytes());
  const glyphstore::SdOutlines outlines = block.outlines();
  EXPECT_EQ(block.bytes(), outlines.fontOffset);
  EXPECT_EQ(58840u, outlines.fileOffset);
  EXPECT_EQ(208484u, outlines.length);

  const std::vector<uint8_t> image = blockImage(block);
  ttf::SdPageCache cache;
  const ttf::SdRange sd{file.source(), outlines.fontOffset, outlines.fileOffset, outlines.length, &cache};
  FontBytes card(image.data(), static_cast<uint32_t>(image.size()), &sd);
  FontBytes whole(file.bytes.data(), static_cast<uint32_t>(file.bytes.size()));
  ttf::Face face;
  ttf::Face wholeFace;
  ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(card, &face));
  ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(whole, &wholeFace));
  EXPECT_EQ(wholeFace.unitsPerEm, face.unitsPerEm);
  EXPECT_EQ(wholeFace.numGlyphs, face.numGlyphs);
  EXPECT_EQ(wholeFace.numHMetrics, face.numHMetrics);
  EXPECT_EQ(wholeFace.maxPoints, face.maxPoints);
  EXPECT_EQ(wholeFace.ascender, face.ascender);
  EXPECT_EQ(wholeFace.descender, face.descender);
  EXPECT_EQ(wholeFace.height, face.height);
  EXPECT_EQ(wholeFace.cmapFormat, face.cmapFormat);
  EXPECT_EQ(wholeFace.glyfLength, face.glyfLength);
  EXPECT_EQ(outlines.fontOffset, face.glyf);
  EXPECT_NE(0u, face.gpos);
  EXPECT_NE(0u, face.gsub);
  for (const uint32_t cp : {uint32_t{'A'}, uint32_t{'z'}, uint32_t{0xE9}, uint32_t{0x2014}, uint32_t{0x4E2D}}) {
    EXPECT_EQ(ttf::glyphIndex(whole, wholeFace, cp), ttf::glyphIndex(card, face, cp)) << cp;
  }
  // Every table but glyf is in place, byte for byte the file's; glyf never is.
  ASSERT_NE(nullptr, card.span(face.gpos, face.gposLength));
  EXPECT_EQ(0, std::memcmp(whole.span(wholeFace.gpos, wholeFace.gposLength), card.span(face.gpos, face.gposLength),
                           face.gposLength));
  EXPECT_EQ(0, std::memcmp(whole.span(wholeFace.loca, wholeFace.locaLength), card.span(face.loca, face.locaLength),
                           face.locaLength));
  EXPECT_EQ(nullptr, card.span(face.glyf, 1));
  EXPECT_FALSE(card.outOfRange());
  EXPECT_EQ(0u, cache.misses());  // nothing of the outlines read yet
}

TEST(TtfSdBlock, KeepsThePostHeaderSoTheFaceReadsItalicAsTheFileDoes) {
  // Spec 7.4: a style block keeps post's first 32 bytes, italicAngle among them. A TrueType file's glyph names after
  // them (NotoSans-Italic: 52,692 bytes of post) stay out.
  for (const char* name :
       {"NotoSans/NotoSans-Italic.ttf", "OpenDyslexic/OpenDyslexic-Italic.otf", "Bookerly/Bookerly-Regular.ttf"}) {
    MemoryFile file{readFont(name)};
    ttf::SdBlock block;
    ASSERT_EQ(ttf::SdPlan::Ok, block.plan(file.source())) << name;
    const std::vector<uint8_t> image = blockImage(block);
    ttf::SdPageCache cache;
    const glyphstore::SdOutlines outlines = block.outlines();
    const ttf::SdRange sd{file.source(), outlines.fontOffset, outlines.fileOffset, outlines.length, &cache};
    FontBytes card(image.data(), static_cast<uint32_t>(image.size()), &sd);
    FontBytes whole(file.bytes.data(), static_cast<uint32_t>(file.bytes.size()));
    ttf::Face face;
    ttf::Face wholeFace;
    ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(card, &face)) << name;
    ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(whole, &wholeFace)) << name;
    EXPECT_EQ(wholeFace.italic, face.italic) << name;
    EXPECT_EQ(std::string(name) != "Bookerly/Bookerly-Regular.ttf", face.italic) << name;
    const size_t record = findRecord(image, "post");
    ASSERT_NE(0u, record) << name;
    const uint32_t length = static_cast<uint32_t>(image[record + 12] << 24 | image[record + 13] << 16 |
                                                  image[record + 14] << 8 | image[record + 15]);
    EXPECT_EQ(32u, length) << name;
    EXPECT_EQ(block.bytes(), ttf::probeFile(file.source()).sdBytes) << name;
  }
}

TEST(TtfSdBlock, AnyRangeReadsAsTheWholeBlockDoes) {
  MemoryFile file{readFont("Inter/Inter-Regular.ttf")};
  ttf::SdBlock block;
  ASSERT_EQ(ttf::SdPlan::Ok, block.plan(file.source()));
  const std::vector<uint8_t> image = blockImage(block);
  const ttf::FileSource source = block.source();
  // Pieces of every size: the directory a byte at a time, across a table's end and its padding, a copy chunk.
  for (const uint32_t piece : {1u, 7u, 333u, 4096u}) {
    std::vector<uint8_t> pieces(image.size());
    for (uint32_t at = 0; at < image.size(); at += piece) {
      const uint32_t len = std::min<uint32_t>(piece, static_cast<uint32_t>(image.size()) - at);
      ASSERT_TRUE(source.read(source.ctx, at, pieces.data() + at, len)) << piece << " at " << at;
    }
    EXPECT_EQ(image, pieces) << piece;
  }
  uint8_t past = 0;
  EXPECT_FALSE(source.read(source.ctx, block.bytes(), &past, 1));
}

TEST(TtfSdBlock, AnOptionalTableOutsideTheFileIsLeftOutAsTheFaceLeavesItOut) {
  // A GPOS record past the end of the file: parseFace() takes the font as having no GPOS, so the block copies none,
  // and the probe's size says so too.
  Font font;
  font.glyphs.resize(1);
  font.glyphs[0].contours = {rect(0, 0, 64, 64)};
  font.extraTables = {{"GPOS", Bytes(64, 0)}};
  Bytes bytes = buildFont(font);
  set32(bytes, findRecord(bytes, "GPOS") + 12, static_cast<uint32_t>(bytes.size()));
  MemoryFile file{bytes};
  ttf::SdBlock block;
  ASSERT_EQ(ttf::SdPlan::Ok, block.plan(file.source()));
  EXPECT_EQ(block.bytes(), ttf::probeFile(file.source()).sdBytes);
  const std::vector<uint8_t> image = blockImage(block);
  const glyphstore::SdOutlines outlines = block.outlines();
  ttf::SdPageCache cache;
  const ttf::SdRange sd{file.source(), outlines.fontOffset, outlines.fileOffset, outlines.length, &cache};
  FontBytes card(image.data(), static_cast<uint32_t>(image.size()), &sd);
  ttf::Face face;
  ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(card, &face));
  EXPECT_EQ(0u, face.gpos);
  EXPECT_TRUE(block.sameAs(image.data(), static_cast<uint32_t>(image.size())));
}

// --- GDEF and the CPac room in a style block ---

namespace {

// One record of a font's table directory (a file's or a block's).
struct DirRecord {
  bool found = false;
  uint32_t checksum = 0;
  uint32_t offset = 0;
  uint32_t length = 0;
};

uint32_t read32(const Bytes& bytes, const size_t at) {
  return static_cast<uint32_t>(bytes[at]) << 24 | static_cast<uint32_t>(bytes[at + 1]) << 16 |
         static_cast<uint32_t>(bytes[at + 2]) << 8 | bytes[at + 3];
}

DirRecord recordOf(const Bytes& font, const char* name) {
  const size_t at = findRecord(font, name);
  if (at == 0) return {};
  return {true, read32(font, at + 4), read32(font, at + 8), read32(font, at + 12)};
}

// What ot::FilterTableWriter would write for the font's own GSUB and GPOS.
uint32_t writerBytes(const Bytes& font) {
  ot::FaceTables tables;
  if (!ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables)) return UINT32_MAX;
  tables[ot::FaceTables::CPAC] = ot::Table();
  tables[ot::FaceTables::CPPL] = ot::Table();
  ot::Face face;
  if (!face.init(tables)) return UINT32_MAX;
  return ot::FilterTableWriter(face).bytes();
}

// A GSUB (version 1.0, no features) whose ScriptList names `script` alone (a record with a null offset), and whose
// LookupList declares `declared` lookups and lists `offsets` (from the list's start), `body` right behind them. With
// dev2, a script the shaper handles, its lookups are counted for a CPac.
Bytes gsubWith(const uint32_t declared, const std::vector<uint16_t>& offsets, const Bytes& body,
               const char* script = "dev2") {
  Bytes gsub;
  for (const uint32_t word : {1u, 0u, 10u, 0u, 18u}) put16(gsub, word);  // scripts at 10, lookups at 18
  put16(gsub, 1);
  for (int i = 0; i < 4; ++i) gsub.push_back(static_cast<uint8_t>(script[i]));
  put16(gsub, 0);
  put16(gsub, declared);
  for (const uint16_t offset : offsets) put16(gsub, offset);
  gsub.insert(gsub.end(), body.begin(), body.end());
  return gsub;
}

// A built font with `gsub` as its GSUB.
Bytes fontWithGsub(const Bytes& gsub) {
  Font font;
  font.glyphs.resize(1);
  font.glyphs[0].contours = {rect(0, 0, 64, 64)};
  font.extraTables = {{"GSUB", gsub}};
  return buildFont(font);
}

// `count` lookups of `subtables` subtables each (null offsets, never read), packed in order behind the list's offsets.
Bytes gsubOfSimpleLookups(const uint32_t count, const uint32_t subtables = 1) {
  std::vector<uint16_t> offsets;
  Bytes body;
  const uint32_t lookupBytes = 6 + 2 * subtables;
  for (uint32_t i = 0; i < count; ++i) {
    offsets.push_back(static_cast<uint16_t>(2 + 2 * count + lookupBytes * i));
    for (const uint32_t word : {1u, 0u, subtables}) put16(body, word);  // type 1, flags 0
    for (uint32_t s = 0; s < subtables; ++s) put16(body, 0);
  }
  return gsubWith(count, offsets, body);
}

// A lookup declaring `subtables` subtables, with the room for their offsets.
Bytes lookupDeclaring(const uint32_t subtables) {
  Bytes lookup;
  for (const uint32_t word : {1u, 0u, subtables}) put16(lookup, word);
  lookup.resize(lookup.size() + 2 * subtables, 0);
  return lookup;
}

// Renames the first record tagged `from` in the ScriptList of `font`'s `table` to `to`, in place: the same font, its
// layout now naming another script. False when the list has no such record.
bool renameScript(Bytes& font, const char* table, const char* from, const char* to) {
  const DirRecord layout = recordOf(font, table);
  if (!layout.found) return false;
  const size_t list = layout.offset + (font[layout.offset + 4] << 8 | font[layout.offset + 5]);
  const uint32_t scripts = static_cast<uint32_t>(font[list] << 8 | font[list + 1]);
  for (uint32_t i = 0; i < scripts; ++i) {
    uint8_t* const tag = font.data() + list + 2 + 6 * i;
    if (std::memcmp(tag, from, 4) != 0) continue;
    std::memcpy(tag, to, 4);
    return true;
  }
  return false;
}

// The CPac room the probe reserves for a built font whose GSUB names `script` alone and has one lookup of one subtable.
uint32_t roomForScript(const char* script) {
  MemoryFile file{fontWithGsub(gsubWith(1, {4}, lookupDeclaring(1), script))};
  return ttf::probeFile(file.source()).cpacBytes;
}

}  // namespace

TEST(TtfFamily, AnSdModeBlockHoldsGdefAndRoomForCpac) {
  const Bytes bytes = readFixture("NotoSansDevanagari-Shaping-Regular.ttf");
  ASSERT_FALSE(bytes.empty());
  MemoryFile file{bytes};
  const ttf::FileProbe probed = ttf::probeFile(file.source());
  ASSERT_EQ(0, probed.style);
  ttf::SdBlock block;
  ASSERT_EQ(ttf::SdPlan::Ok, block.plan(file.source()));
  const std::vector<uint8_t> image = blockImage(block);

  const DirRecord gdefInFile = recordOf(bytes, "GDEF");
  const DirRecord gdef = recordOf(image, "GDEF");
  ASSERT_TRUE(gdefInFile.found);
  ASSERT_TRUE(gdef.found);
  EXPECT_EQ(gdefInFile.length, gdef.length);
  EXPECT_EQ(0, std::memcmp(bytes.data() + gdefInFile.offset, image.data() + gdef.offset, gdef.length));

  const DirRecord cpac = recordOf(image, "CPac");
  ASSERT_TRUE(cpac.found);
  EXPECT_GT(probed.cpacBytes, 0u);
  EXPECT_EQ(probed.cpacBytes, cpac.length);
  EXPECT_EQ(0u, cpac.checksum);
  EXPECT_EQ(block.cpacAt(), cpac.offset);
  EXPECT_EQ(block.cpacBytes(), cpac.length);
  const glyphstore::SdOutlines outlines = block.outlines();
  EXPECT_LE(cpac.offset + cpac.length, outlines.fontOffset);
  EXPECT_EQ(block.bytes(), outlines.fontOffset);
  // After every other copied table: glyf's record points past the block, and no table starts behind the CPac.
  const uint32_t tables = static_cast<uint32_t>(image[4] << 8 | image[5]);
  for (uint32_t i = 0; i < tables; ++i) {
    const size_t at = 12 + 16 * i;
    const uint32_t offset = read32(image, at + 8);
    if (read32(image, at) == 0x43506163u /* CPac */ || offset >= block.bytes()) continue;
    EXPECT_LT(offset, cpac.offset) << "table " << i;
  }
  // The room reads erased: the install writes the table there.
  EXPECT_TRUE(std::all_of(image.begin() + cpac.offset, image.begin() + cpac.offset + cpac.length,
                          [](const uint8_t b) { return b == 0xFF; }));
  EXPECT_EQ(block.bytes(), probed.sdBytes);
  EXPECT_EQ(ttf::flashBlockBytes(static_cast<uint32_t>(bytes.size()), probed.cpacBytes), probed.flashBytes);
  EXPECT_GT(probed.flashBytes, bytes.size());
}

TEST(TtfFamily, TheCpacRoomIsWhatTheWriterWrites) {
  // Fonts that shape: the subset, and Noto Sans, whose layout names dev2 and deva besides Latin.
  for (const Bytes& bytes :
       {readFixture("NotoSansDevanagari-Shaping-Regular.ttf"), readFont("NotoSans/NotoSans-Regular.ttf")}) {
    ASSERT_FALSE(bytes.empty());
    const uint32_t written = writerBytes(bytes);
    EXPECT_GT(written, 0u);
    MemoryFile file{bytes};
    uint32_t room = 0;
    ASSERT_TRUE(ttf::filterTableBytes(file.source(), &room));
    EXPECT_EQ(written, room);
    // The counts alone give the same room, whatever table they came from.
    ttf::LayoutCounts gsub;
    ttf::LayoutCounts gpos;
    const DirRecord gsubRecord = recordOf(bytes, "GSUB");
    const DirRecord gposRecord = recordOf(bytes, "GPOS");
    ASSERT_TRUE(ttf::countLookups(file.source(), gsubRecord.offset, gsubRecord.length, &gsub));
    ASSERT_TRUE(ttf::countLookups(file.source(), gposRecord.offset, gposRecord.length, &gpos));
    EXPECT_EQ(written, ttf::cpacBytesFor(gsub, gpos));
    EXPECT_EQ(written, 16 + 28 * (gsub.lookups + gpos.lookups) + 24 * (gsub.subtables + gpos.subtables));
  }
}

TEST(TtfFamily, TheProbeCapsAreTheWritersCaps) {
  EXPECT_EQ(ot::FilterTableWriter::kMaxLookups, ttf::kCpacMaxLookups);
  EXPECT_EQ(ot::FilterTableWriter::kMaxSubtables, ttf::kCpacMaxSubtables);
}

TEST(TtfFamily, AFontWithoutLookupsReservesNoCpac) {
  MemoryFile file{builtFont()};
  const ttf::FileProbe probed = ttf::probeFile(file.source());
  EXPECT_EQ(0, probed.style);
  EXPECT_EQ(0u, probed.cpacBytes);
  EXPECT_EQ(file.bytes.size(), probed.flashBytes);
  ttf::SdBlock block;
  ASSERT_EQ(ttf::SdPlan::Ok, block.plan(file.source()));
  EXPECT_EQ(0u, block.cpacBytes());
  EXPECT_EQ(0u, block.cpacAt());
  EXPECT_EQ(block.bytes(), probed.sdBytes);
  EXPECT_FALSE(recordOf(blockImage(block), "CPac").found);
}

TEST(TtfFamily, AFontWhoseLayoutNamesNoShapedScriptReservesNoCpac) {
  // Latin and CJK faces with lookups (ligatures, kerning) the writer would write a table for: none shapes, so none
  // reserves a room. The flash-mode block is the file alone and the SD-mode block has no CPac record.
  const char* const latin[] = {"Bookerly/Bookerly-Regular.ttf", "Bookerly/Bookerly-Bold.ttf",
                               "Bookerly/Bookerly-Italic.ttf",  "Bookerly/Bookerly-BoldItalic.ttf",
                               "Inter/Inter-Regular.ttf",       "Ubuntu/Ubuntu-Regular.ttf"};
  std::vector<Bytes> fonts;
  for (const char* name : latin) fonts.push_back(readFont(name));
  fonts.push_back(readFixture("NotoSansJP-TrueType.ttf"));
  fonts.push_back(readFixture("NotoSansJP-Cff.otf"));
  for (size_t i = 0; i < fonts.size(); ++i) {
    ASSERT_FALSE(fonts[i].empty()) << i;
    EXPECT_GT(writerBytes(fonts[i]), 0u) << i;
    MemoryFile file{fonts[i]};
    const ttf::FileProbe probed = ttf::probeFile(file.source());
    ASSERT_GE(probed.style, 0) << i;
    EXPECT_EQ(0u, probed.cpacBytes) << i;
    EXPECT_EQ(fonts[i].size(), probed.flashBytes) << i;
    ttf::SdBlock block;
    ASSERT_EQ(ttf::SdPlan::Ok, block.plan(file.source())) << i;
    EXPECT_EQ(0u, block.cpacBytes()) << i;
    EXPECT_EQ(block.bytes(), probed.sdBytes) << i;
    EXPECT_FALSE(recordOf(blockImage(block), "CPac").found) << i;
    uint32_t room = 1;
    ASSERT_TRUE(ttf::filterTableBytes(file.source(), &room)) << i;
    EXPECT_EQ(0u, room) << i;
  }
}

TEST(TtfFamily, OnlyAScriptTheShaperHandlesOrArabicReservesARoom) {
  // The engine's own tags (old and new, and Sinhala's), Arabic's, which the joining shaper will read, and Myanmar's
  // two (deviation 14: a mymr font takes the default shaper, but the plan tries the tag).
  for (const char* script : {"deva", "dev2", "dev3", "beng", "bng2", "bng3", "guru", "gur2", "gur3", "gujr", "gjr2",
                             "gjr3", "orya", "ory2", "ory3", "taml", "tml2", "tml3", "telu", "tel2", "tel3", "knda",
                             "knd2", "knd3", "mlym", "mlm2", "mlm3", "sinh", "arab", "mym2", "mymr"}) {
    EXPECT_EQ(16u + 28u + 24u, roomForScript(script)) << script;
  }
  // Whatever else: Latin and the defaults, Hebrew, CJK, and scripts the shaper does not handle.
  for (const char* script : {"DFLT", "dflt", "latn", "cyrl", "grek", "hebr", "hani", "kana", "hang", "thai", "khmr",
                             "tibt", "syrc", "thaa", "nko "}) {
    EXPECT_EQ(0u, roomForScript(script)) << script;
  }
  // The Indic tags are the engine's own list, not a copy of it.
  EXPECT_TRUE(ot::isShapedScriptTag(ot::tag("deva")));
  EXPECT_TRUE(ot::isShapedScriptTag(ot::tag("dev2")));
  EXPECT_TRUE(ot::isShapedScriptTag(ot::tag("mlm3")));
  EXPECT_TRUE(ot::isShapedScriptTag(ot::tag("sinh")));
  EXPECT_FALSE(ot::isShapedScriptTag(0));  // the list's padding
  EXPECT_FALSE(ot::isShapedScriptTag(ot::tag("latn")));
}

TEST(TtfFamily, ALatinScriptListReservesNoCpacEvenWithLookups) {
  // 300 lookups under latn alone: not counted, no room, though the writer would write a table for them.
  MemoryFile latin{fontWithGsub(gsubWith(1, {4}, lookupDeclaring(300), "latn"))};
  EXPECT_EQ(0u, ttf::probeFile(latin.source()).cpacBytes);
  EXPECT_EQ(16u + 28u + 24u * 300, writerBytes(latin.bytes));
  // The same lookups under arab: the room is the writer's table.
  MemoryFile arabic{fontWithGsub(gsubWith(1, {4}, lookupDeclaring(300), "arab"))};
  EXPECT_EQ(writerBytes(arabic.bytes), ttf::probeFile(arabic.source()).cpacBytes);
  EXPECT_EQ(16u + 28u + 24u * 300, ttf::probeFile(arabic.source()).cpacBytes);
}

TEST(TtfFamily, AShapedScriptInGposAloneReservesTheRoomOfBoth) {
  // GSUB under latn, GPOS (one kern lookup of a pair subtable) under dev2: the font shapes, and both tables count.
  LayoutLookup kern;
  PairSubtable pairs;
  pairs.pairs = {{1, {{2, -100}}}};
  kern.pairSubtables = {pairs};
  Font font;
  font.glyphs.resize(3);
  for (auto& glyph : font.glyphs) glyph.contours = {rect(0, 0, 64, 64)};
  font.extraTables = {{"GSUB", gsubWith(1, {4}, lookupDeclaring(2), "latn")},
                      {"GPOS", layoutTable({{"dev2", {0}}}, {{"kern", {0}}}, {kern})}};
  MemoryFile file{buildFont(font)};
  const uint32_t room = ttf::probeFile(file.source()).cpacBytes;
  EXPECT_EQ(16u + 28u * 2 + 24u * 3, room);
  EXPECT_EQ(writerBytes(file.bytes), room);
}

TEST(TtfFamily, AScriptListIsReadWhereverItLiesWithinItsBudget) {
  // 200 records of a script the shaper does not handle, then dev2: the first read holds the header and 19 records, each
  // later one 21, and the probe reads 8 more at most (187 records).
  const auto fontNaming = [](const uint32_t devAt) {
    Bytes gsub;
    for (const uint32_t word : {1u, 0u, 10u, 0u, 10u + 2 + 6 * 201}) put16(gsub, word);
    put16(gsub, 201);
    for (uint32_t i = 0; i <= 200; ++i) {
      const char* const script = i == devAt ? "dev2" : "zzzz";
      for (int c = 0; c < 4; ++c) gsub.push_back(static_cast<uint8_t>(script[c]));
      put16(gsub, 0);
    }
    put16(gsub, 1);  // the LookupList: one lookup of one subtable
    put16(gsub, 4);
    const Bytes lookup = lookupDeclaring(1);
    gsub.insert(gsub.end(), lookup.begin(), lookup.end());
    return fontWithGsub(gsub);
  };
  for (const uint32_t devAt : {0u, 18u, 19u, 100u, 186u}) {
    MemoryFile file{fontNaming(devAt)};
    EXPECT_EQ(16u + 28u + 24u, ttf::probeFile(file.source()).cpacBytes) << devAt;
  }
  MemoryFile past{fontNaming(187)};
  EXPECT_EQ(0u, ttf::probeFile(past.source()).cpacBytes);
  // The count's reads (the probe's, filterTableBytes()): the directory and the table's first 128 bytes, as for a list
  // of one record, then 8 reads of the list. Counted alone, as the probe's parseFace() reads pages wherever the other
  // tables lie behind the list.
  MemoryFile one{fontWithGsub(gsubWith(1, {4}, lookupDeclaring(1), "zzzz"))};
  MemoryFile counted{fontNaming(187)};
  uint32_t room = 0;
  ASSERT_TRUE(ttf::filterTableBytes(one.source(), &room));
  ASSERT_TRUE(ttf::filterTableBytes(counted.source(), &room));
  EXPECT_EQ(0u, room);
  EXPECT_EQ(one.reads + 8, counted.reads);
}

TEST(TtfFamily, AScriptListPastTheFirstWindowIsReadThere) {
  // The ScriptList after a LookupList of 300 lookups, far past the table's first 128 bytes: read where it lies.
  for (const char* script : {"dev2", "latn"}) {
    const uint32_t lookups = 300;
    const uint32_t listBytes = 2 + 2 * lookups + 8 * lookups;
    Bytes gsub;
    for (const uint32_t word : {1u, 0u, 10u + listBytes, 0u, 10u}) put16(gsub, word);  // lookups at 10, scripts after
    put16(gsub, lookups);
    for (uint32_t i = 0; i < lookups; ++i) put16(gsub, 2 + 2 * lookups + 8 * i);
    for (uint32_t i = 0; i < lookups; ++i) {
      for (const uint32_t word : {1u, 0u, 1u, 0u}) put16(gsub, word);  // type 1, flags 0, one subtable
    }
    put16(gsub, 1);
    for (int c = 0; c < 4; ++c) gsub.push_back(static_cast<uint8_t>(script[c]));
    put16(gsub, 0);
    MemoryFile file{fontWithGsub(gsub)};
    const uint32_t room = ttf::probeFile(file.source()).cpacBytes;
    EXPECT_EQ(std::strcmp(script, "dev2") == 0 ? 16u + 28u * lookups + 24u * lookups : 0u, room) << script;
  }
}

TEST(TtfFamily, TheCffBlockKeepsItsCffTableLast) {
  // A CJK CFF font with its DFLT script renamed arab: a CFF font that shapes, so its SD-mode block has a CPac room.
  Bytes bytes = readFixture("NotoSansJP-Cff.otf");
  ASSERT_FALSE(bytes.empty());
  ASSERT_TRUE(renameScript(bytes, "GPOS", "DFLT", "arab"));
  MemoryFile file{bytes};
  const ttf::FileProbe probed = ttf::probeFile(file.source());
  ttf::SdBlock block;
  ASSERT_EQ(ttf::SdPlan::Ok, block.plan(file.source()));
  EXPECT_GT(probed.cpacBytes, 0u);  // its GPOS has kern
  EXPECT_EQ(block.bytes(), probed.sdBytes);
  const std::vector<uint8_t> image = blockImage(block);
  const DirRecord cpac = recordOf(image, "CPac");
  const DirRecord cff = recordOf(image, "CFF ");
  ASSERT_TRUE(cpac.found);
  ASSERT_TRUE(cff.found);
  EXPECT_EQ(probed.cpacBytes, cpac.length);
  EXPECT_LE(cpac.offset + cpac.length, cff.offset);
  // The rebuilt table ends the block and its charstring data runs past it (it stays on the card); no other starts
  // later.
  EXPECT_LT(cff.offset, block.bytes());
  EXPECT_GT(cff.length, block.outlines().length);
  EXPECT_EQ(block.bytes(), cff.offset + (cff.length - block.outlines().length));
  EXPECT_GT(cff.offset + cff.length, block.bytes());
  const uint32_t tables = static_cast<uint32_t>(image[4] << 8 | image[5]);
  for (uint32_t i = 0; i < tables; ++i) EXPECT_LE(read32(image, 12 + 16 * i + 8), cff.offset) << "table " << i;
}

TEST(TtfFamily, AHostileLookupListIsCountedBounded) {
  // 65,535 lookups declared, past the cap: no CPac, and the list is not walked after its count.
  {
    MemoryFile file{fontWithGsub(gsubWith(65535, std::vector<uint16_t>(65535, 0xFFFF), {}))};
    const ttf::FileProbe probed = ttf::probeFile(file.source());
    EXPECT_EQ(0, probed.style);
    EXPECT_EQ(0u, probed.cpacBytes);
    EXPECT_EQ(file.bytes.size(), probed.flashBytes);
    EXPECT_LT(file.reads, 100u);
    EXPECT_EQ(0u, writerBytes(file.bytes));
  }
  // The most it takes: 4,096 lookups of a subtable each.
  {
    MemoryFile file{fontWithGsub(gsubOfSimpleLookups(4096))};
    const ttf::FileProbe probed = ttf::probeFile(file.source());
    EXPECT_EQ(16u + 28u * 4096 + 24u * 4096, probed.cpacBytes);
    EXPECT_EQ(writerBytes(file.bytes), probed.cpacBytes);
    // One more lookup is past the cap.
    MemoryFile past{fontWithGsub(gsubOfSimpleLookups(4097))};
    EXPECT_EQ(0u, ttf::probeFile(past.source()).cpacBytes);
  }
  // 4,096 lookups at one lookup that declares 65,535 subtables: the declarations sum past the cap, whatever the
  // lookups' count.
  {
    const uint32_t listBytes = 2 + 2 * 4096;
    MemoryFile file{fontWithGsub(
        gsubWith(4096, std::vector<uint16_t>(4096, static_cast<uint16_t>(listBytes)), lookupDeclaring(65535)))};
    const ttf::FileProbe probed = ttf::probeFile(file.source());
    EXPECT_EQ(0, probed.style);
    EXPECT_EQ(0u, probed.cpacBytes);
    EXPECT_EQ(0u, writerBytes(file.bytes));
  }
  // At the subtable cap exactly: still a table; one past: none.
  for (const uint32_t subtables : {ttf::kCpacMaxSubtables, ttf::kCpacMaxSubtables + 1}) {
    MemoryFile file{fontWithGsub(gsubWith(1, {4}, lookupDeclaring(subtables)))};
    const uint32_t room = ttf::probeFile(file.source()).cpacBytes;
    EXPECT_EQ(writerBytes(file.bytes), room) << subtables;
    EXPECT_EQ(subtables == ttf::kCpacMaxSubtables ? 16u + 28u + 24u * subtables : 0u, room) << subtables;
  }
}

TEST(TtfFamily, ALookupListWithBadOffsetsCountsAsTheFaceReadsIt) {
  // 200 plain lookups, listed out of order among four that the Face reads as lookups of no subtables or of fewer than
  // they declare: a null offset, one past the list, one declaring 50 subtables with room for 6, and one with under 8
  // bytes left to the table's end.
  constexpr uint32_t kPlain = 200;
  constexpr uint32_t kLookups = kPlain + 4;
  const uint32_t bodyAt = 2 + 2 * kLookups;
  Bytes body;
  for (uint32_t i = 0; i < kPlain; ++i) {
    for (const uint32_t word : {1u, 0u, 1u, 0u}) put16(body, word);  // type 1, flags 0, one subtable (never read)
  }
  const uint32_t tooMany = bodyAt + static_cast<uint32_t>(body.size());
  for (const uint32_t word : {1u, 0u, 50u, 0u, 0u, 0u}) put16(body, word);  // 6 bytes of header, room for 3 offsets
  const uint32_t nearEnd = bodyAt + static_cast<uint32_t>(body.size());
  for (const uint32_t word : {1u, 0u, 1u}) put16(body, word);
  body.push_back(0);  // 7 bytes to the end
  std::vector<uint16_t> offsets;
  for (uint32_t i = 0; i < kPlain; ++i) {
    offsets.push_back(static_cast<uint16_t>(bodyAt + 8 * ((i * 7) % kPlain)));  // out of order, each once
    if (i == 20) offsets.push_back(0);
    if (i == 60) offsets.push_back(0xFFFF);
    if (i == 100) offsets.push_back(static_cast<uint16_t>(tooMany));
    if (i == 140) offsets.push_back(static_cast<uint16_t>(nearEnd));
  }
  ASSERT_EQ(kLookups, offsets.size());
  MemoryFile file{fontWithGsub(gsubWith(kLookups, offsets, body))};
  const ttf::FileProbe probed = ttf::probeFile(file.source());
  EXPECT_EQ(0, probed.style);
  EXPECT_GT(probed.cpacBytes, 0u);
  EXPECT_EQ(writerBytes(file.bytes), probed.cpacBytes);
  // Headers within a window of each other share a read: well under the 204 lookups' one each (49 measured; each piece
  // of 64 offsets, sorted, still spans the body).
  EXPECT_LT(file.reads, 80u);
}

TEST(TtfFamily, EachPieceOfOffsetsReadsItsHeadersAfresh) {
  // 200 lookups of 10 bytes, packed in order. The first piece of 64 offsets ends in a window that holds lookup 64's
  // header too; the count reads offsets and headers through one buffer, so the next piece, read over that window,
  // must not be taken for lookup 64's header.
  MemoryFile file{fontWithGsub(gsubOfSimpleLookups(200, 2))};
  const ttf::FileProbe probed = ttf::probeFile(file.source());
  EXPECT_EQ(0, probed.style);
  EXPECT_EQ(16u + 28u * 200 + 24u * 400, probed.cpacBytes);
  EXPECT_EQ(writerBytes(file.bytes), probed.cpacBytes);
}

TEST(TtfFamily, ACraftedScatteredLookupListHitsTheReadBudget) {
  // 4,096 lookups whose offsets, piece by piece, name the same 64 places 256 bytes apart: every header lies in a
  // window of its own. Counted whole they would give a CPac; past the budget the table counts as capped.
  constexpr uint32_t kLookups = 4096;
  const uint32_t bodyAt = 2 + 2 * kLookups;
  Bytes body;
  body.resize(64 * 256, 0);
  std::vector<uint16_t> offsets;
  for (uint32_t i = 0; i < kLookups; ++i) offsets.push_back(static_cast<uint16_t>(bodyAt + 256 * (i % 64)));
  for (uint32_t k = 0; k < 64; ++k) {
    Bytes lookup;
    for (const uint32_t word : {1u, 0u, 1u}) put16(lookup, word);
    std::copy(lookup.begin(), lookup.end(), body.begin() + 256 * k);
  }
  MemoryFile file{fontWithGsub(gsubWith(kLookups, offsets, body))};
  const ttf::FileProbe probed = ttf::probeFile(file.source());
  EXPECT_EQ(0, probed.style);
  EXPECT_EQ(0u, probed.cpacBytes);
  EXPECT_EQ(file.bytes.size(), probed.flashBytes);
  // The budget, and the probe's own reads: the directory, head and OS/2.
  EXPECT_LE(file.reads, ttf::kMaxCountingReads + 10u);
  EXPECT_GT(file.reads, ttf::kMaxCountingReads);
  // Counted one by one, the same table would be a CPac.
  EXPECT_GT(writerBytes(file.bytes), 0u);
}

TEST(TtfFamily, ACardReadFailingWhileCountingIsTransient) {
  MemoryFile file{fontWithGsub(gsubOfSimpleLookups(300))};
  ttf::SdBlock block;
  ASSERT_EQ(ttf::SdPlan::Ok, block.plan(file.source()));
  EXPECT_EQ(0, ttf::probeFile(file.source()).style);
  // Reads of GSUB past its first 128 bytes (its header and its script list, which names dev2, read first) fail, the
  // LookupList's offsets among them; head, which the probe reads before counting, is outside them, so it is the count
  // that fails.
  const DirRecord head = recordOf(file.bytes, "head");
  const DirRecord gsub = recordOf(file.bytes, "GSUB");
  ASSERT_GT(gsub.length, 128u);
  ASSERT_TRUE(head.offset >= gsub.offset + gsub.length || head.offset + 54 <= gsub.offset + 128);
  struct Cut {
    MemoryFile* file;
    uint32_t from;
    uint32_t to;
  } cut{&file, gsub.offset + 128, gsub.offset + gsub.length};
  const ttf::FileSource cutSource{static_cast<uint32_t>(file.bytes.size()),
                                  [](void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
                                    const Cut& c = *static_cast<Cut*>(ctx);
                                    if (offset < c.to && uint64_t{offset} + len > c.from) return false;
                                    return MemoryFile::read(c.file, offset, dst, len);
                                  },
                                  &cut, file.stamp};
  EXPECT_EQ(ttf::SdPlan::Transient, block.plan(cutSource));
  EXPECT_EQ(-1, ttf::probeFile(cutSource).style);
  uint32_t room = 0;
  EXPECT_FALSE(ttf::filterTableBytes(cutSource, &room));
}

TEST(TtfSdBlock, AFileWithoutTheOutlinesOfItsKindHasNone) {
  ttf::SdBlock block;
  Bytes noGlyf = builtFont();
  noGlyf[findRecord(noGlyf, "glyf") + 3] = 'X';  // 'glyX'
  MemoryFile withoutOutlines{noGlyf};
  EXPECT_EQ(ttf::SdPlan::NoBlock, block.plan(withoutOutlines.source()));
  Bytes cff = builtFont();
  std::memcpy(cff.data(), "OTTO", 4);
  MemoryFile noCffTable{cff};
  EXPECT_EQ(ttf::SdPlan::NoBlock, block.plan(noCffTable.source()));  // OTTO: a glyf it carries is not its outlines
  Bytes broken = builtFont();
  set32(broken, findRecord(broken, "hmtx") + 12, static_cast<uint32_t>(broken.size()));  // runs past the end
  MemoryFile outside{broken};
  EXPECT_EQ(ttf::SdPlan::NoBlock, block.plan(outside.source()));
  MemoryFile fine{builtFont()};
  EXPECT_EQ(ttf::SdPlan::Ok, block.plan(fine.source()));
  // A read that fails says nothing of the file: for now only.
  fine.failReads = true;
  EXPECT_EQ(ttf::SdPlan::Transient, block.plan(fine.source()));
  MemoryFile openType{readFont("OpenDyslexic/OpenDyslexic-Regular.otf")};
  EXPECT_EQ(ttf::SdPlan::Ok, block.plan(openType.source()));
}

// --- Native size font IDs ---

TEST(TtfNativeIds, AWindowHoldsItsSizesAndNothingElse) {
  const int32_t base = ttf::nativeBaseFrom(0xDEADBEEFu);
  EXPECT_EQ(0x5EADBE00, base);  // bits 8-30: positive, a multiple of 256
  EXPECT_TRUE(ttf::isNativeId(base, base | 47));
  EXPECT_EQ(47, ttf::nativeIdPixels(base | 47));
  EXPECT_FALSE(ttf::isNativeId(base, (base + 0x100) | 47));
  EXPECT_FALSE(ttf::isNativeId(base, static_cast<int>(0xDEADBE2Fu)));  // the same bits with the sign set
  EXPECT_FALSE(ttf::isNativeId(0, 47));                                // no window
}

TEST(TtfNativeIds, AHashWithoutWindowBitsGivesNoWindow) { EXPECT_EQ(0, ttf::nativeBaseFrom(0x800000FFu)); }
