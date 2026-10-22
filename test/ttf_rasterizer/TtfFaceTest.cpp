// parseFace, glyphIndex and the metrics: what the engine reads from a font file, and the converter's sizes.
#include <gtest/gtest.h>

#include <cstdint>

#include "CffBuilder.h"
#include "TtfBuilder.h"
#include "TtfFace.h"
#include "TtfTestUtil.h"

using namespace ttf;
using namespace ttf_test;

namespace {

// .notdef, 'A' and 'B': three glyphs of different widths.
Font threeGlyphs() {
  Font font;
  font.glyphs.resize(3);
  font.glyphs[0].contours = {rect(50, 0, 450, 700)};
  font.glyphs[1].contours = {rect(20, 0, 620, 700)};
  font.glyphs[1].advance = 640;
  font.glyphs[2].contours = {rect(60, 0, 560, 700), rect(160, 100, 460, 600)};
  font.glyphs[2].advance = 600;
  font.cmap = {{'A', 1}, {'B', 2}};
  return font;
}

FaceStatus parse(const Bytes& data, Face* face) {
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  return parseFace(bytes, face);
}

uint16_t lookup(const Bytes& data, const uint32_t codepoint) {
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  if (parseFace(bytes, &face) != FaceStatus::Ok) return 0xFFFF;
  return glyphIndex(bytes, face, codepoint);
}

}  // namespace

TEST(TtfFace, ParsesTheTablesAGlyphNeeds) {
  Font font = threeGlyphs();
  font.longLoca = true;
  const Bytes data = buildFont(font);
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(data, &face));
  EXPECT_EQ(1000, face.unitsPerEm);
  EXPECT_EQ(3, face.numGlyphs);
  EXPECT_EQ(3, face.numHMetrics);
  EXPECT_TRUE(face.longLoca);
  EXPECT_EQ(8, face.maxPoints);
  EXPECT_EQ(2, face.maxContours);
  EXPECT_EQ(20, face.xMin);
  EXPECT_EQ(0, face.yMin);
  EXPECT_EQ(620, face.xMax);
  EXPECT_EQ(700, face.yMax);
  EXPECT_EQ(800, face.ascender);
  EXPECT_EQ(-200, face.descender);
  EXPECT_EQ(1000, face.height);
  EXPECT_EQ(4, face.cmapFormat);
}

TEST(TtfFace, CffFontsAreReadAndCollectionsAreUnsupported) {
  Font font = threeGlyphs();
  font.sfntVersion = tagOf("OTTO");
  CffSpec cff;  // three glyphs, as the TrueType tables have
  font.extraTables = {{"CFF ", buildCff(cff)}};
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  EXPECT_EQ(Face::Outlines::Cff, face.outlines);
  EXPECT_EQ(3, face.numGlyphs);
  EXPECT_EQ(0u, face.glyf);
  EXPECT_EQ(0u, face.loca);
  EXPECT_EQ(1, lookup(buildFont(font), 'A'));
  font.extraTables.clear();  // 'OTTO' without a CFF table
  EXPECT_EQ(FaceStatus::Malformed, parse(buildFont(font), &face));
  font.sfntVersion = tagOf("ttcf");
  EXPECT_EQ(FaceStatus::Unsupported, parse(buildFont(font), &face));
  font.sfntVersion = tagOf("true");  // Apple's TrueType tag is fine
  ASSERT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  EXPECT_EQ(Face::Outlines::TrueType, face.outlines);
}

TEST(TtfFace, ACff2OnlyFontIsUnsupported) {
  // Variable OpenType (CFF2) is not read; FreeType reads a CFF2 table before a CFF one, so neither is a font with both.
  Font font = threeGlyphs();
  font.sfntVersion = tagOf("OTTO");
  Bytes cff2 = {2, 0, 5, 0, 0};
  font.extraTables = {{"CFF2", cff2}};
  Face face;
  EXPECT_EQ(FaceStatus::Unsupported, parse(buildFont(font), &face));
  font.extraTables.push_back({"CFF ", buildCff(CffSpec{})});
  EXPECT_EQ(FaceStatus::Unsupported, parse(buildFont(font), &face));
}

TEST(TtfFace, ACffFontTakesAShortMaxp) {
  // A CFF font's maxp is version 0.5, six bytes: no outline limits.
  Font font = threeGlyphs();
  font.sfntVersion = tagOf("OTTO");
  Bytes maxp(6, 0);
  set32(maxp, 0, 0x00005000);
  set16(maxp, 4, 3);
  font.extraTables = {{"CFF ", buildCff(CffSpec{})}, {"maxp", maxp}};
  Face face;
  EXPECT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  set32(maxp, 0, 0x00006000);
  font.extraTables[1].second = maxp;
  EXPECT_EQ(FaceStatus::Malformed, parse(buildFont(font), &face));
  font.sfntVersion = 0x00010000;  // a TrueType font needs version 1.0
  set32(maxp, 0, 0x00005000);
  font.extraTables = {{"maxp", maxp}};
  EXPECT_EQ(FaceStatus::Malformed, parse(buildFont(font), &face));
}

TEST(TtfFace, BrokenFilesAreMalformed) {
  Face face;
  EXPECT_EQ(FaceStatus::Malformed, parse(Bytes{}, &face));
  EXPECT_EQ(FaceStatus::Malformed, parse(Bytes{'w', 'O', 'F', 'F', 0, 0, 0, 0, 0, 0, 0, 0}, &face));
  const Bytes good = buildFont(threeGlyphs());
  EXPECT_EQ(FaceStatus::Malformed, parse(Bytes(good.begin(), good.begin() + 40), &face));  // directory cut

  Bytes data = buildFont(threeGlyphs());
  data[findRecord(data, "hmtx")] = 'x';  // the table is gone
  EXPECT_EQ(FaceStatus::Malformed, parse(data, &face));
}

TEST(TtfFace, ImpossibleHeaderValuesAreMalformed) {
  Face face;
  Font font = threeGlyphs();
  font.unitsPerEm = 8;
  EXPECT_EQ(FaceStatus::Malformed, parse(buildFont(font), &face));
  Bytes data = buildFont(threeGlyphs());
  set32(data, findRecord(data, "glyf") + 12, static_cast<uint32_t>(data.size()));  // glyf runs past the end
  EXPECT_EQ(FaceStatus::Malformed, parse(data, &face));
}

TEST(TtfFace, Format4MapsByDeltasAndByTheGlyphArray) {
  Font font = threeGlyphs();
  font.cmap = {{'A', 1}, {'B', 2}, {'C', 1}, {0x20AC, 2}};
  EXPECT_EQ(1, lookup(buildFont(font), 'A'));
  EXPECT_EQ(2, lookup(buildFont(font), 'B'));
  EXPECT_EQ(1, lookup(buildFont(font), 'C'));
  EXPECT_EQ(2, lookup(buildFont(font), 0x20AC));
  EXPECT_EQ(0, lookup(buildFont(font), 'D'));
  EXPECT_EQ(0, lookup(buildFont(font), 0x1F600));  // past the BMP: never in format 4
  font.cmapRangeOffsets = true;
  EXPECT_EQ(1, lookup(buildFont(font), 'A'));
  EXPECT_EQ(2, lookup(buildFont(font), 'B'));
  EXPECT_EQ(1, lookup(buildFont(font), 'C'));
  EXPECT_EQ(2, lookup(buildFont(font), 0x20AC));
  EXPECT_EQ(0, lookup(buildFont(font), 'D'));
}

TEST(TtfFace, Format4ReadsStayInsideTheCmapTable) {
  // The table directory says the cmap is shorter than its subtable: the bytes past it are still in the file (the
  // glyf table follows), but in SD mode they could be the card's, so nothing past the table is read.
  Font font = threeGlyphs();
  font.cmapRangeOffsets = true;  // one segment A-B through the glyph array, which ends the subtable
  Bytes data = buildFont(font);
  const size_t lengthField = findRecord(data, "cmap") + 12;
  const uint32_t length = static_cast<uint32_t>(data[lengthField] << 24 | data[lengthField + 1] << 16 |
                                                data[lengthField + 2] << 8 | data[lengthField + 3]);
  set32(data, lengthField, length - 2);  // B's glyph array entry is past the table
  EXPECT_EQ(1, lookup(data, 'A'));
  EXPECT_EQ(0, lookup(data, 'B'));
  set32(data, lengthField, length - 6);  // the segment arrays run past it too: passed over, and no other map
  Face face;
  EXPECT_EQ(FaceStatus::Unsupported, parse(data, &face));
}

TEST(TtfFace, AFormat4SubtableRunningPastTheTableGivesWayToTheNext) {
  // FreeType's validator drops the (3,1) subtable whose arrays the table cuts short, and takes the (0,3) one.
  Bytes cut = cmapFormat4({{'A', 2}}, false);
  cut.resize(cut.size() - 4);  // the cmap table ends inside the subtable's last arrays
  Font font = threeGlyphs();
  font.extraTables = {{"cmap", cmapTable({{0, 3, cmapFormat4({{'A', 1}}, false)}, {3, 1, cut}})}};
  EXPECT_EQ(1, lookup(buildFont(font), 'A'));
}

TEST(TtfFace, Format12ReachesPastTheBmp) {
  Font font = threeGlyphs();
  font.cmapFormat12 = true;
  font.cmap = {{'A', 1}, {'B', 2}, {0x1F600, 1}, {0x1F601, 2}};
  const Bytes data = buildFont(font);
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(data, &face));
  EXPECT_EQ(12, face.cmapFormat);
  EXPECT_EQ(1, lookup(data, 'A'));
  EXPECT_EQ(2, lookup(data, 0x1F601));
  EXPECT_EQ(0, lookup(data, 0x1F602));
}

TEST(TtfFace, AGlyphPastNumGlyphsReadsAsNone) {
  Font font = threeGlyphs();
  font.cmap = {{'A', 1}, {'Z', 7}};
  EXPECT_EQ(1, lookup(buildFont(font), 'A'));
  EXPECT_EQ(0, lookup(buildFont(font), 'Z'));
}

TEST(TtfFace, TheCmapIsChosenAsFreeTypeChoosesIt) {
  // From the last record backwards: a UCS-4 map first, else any Unicode map; formats it cannot read are
  // passed over, and Mac Roman is not Unicode.
  const Bytes mapsA1 = cmapFormat4({{'A', 1}}, false);
  const Bytes mapsA2 = cmapFormat4({{'A', 2}}, false);
  const Bytes format12A2 = cmapFormat12({{'A', 2}});
  Bytes format6(10, 0);
  set16(format6, 0, 6);
  struct Case {
    const char* what;
    std::vector<std::tuple<uint16_t, uint16_t, Bytes>> subtables;
    int expected;  // the glyph for 'A', or -1 for "no usable map"
  };
  const Case cases[] = {
      {"the last Unicode BMP map wins", {{0, 3, mapsA1}, {3, 1, mapsA2}}, 2},
      {"a UCS-4 map wins over a later BMP map", {{3, 10, format12A2}, {3, 1, mapsA1}}, 2},
      {"(0,4) is UCS-4 too", {{0, 4, format12A2}, {3, 1, mapsA1}}, 2},
      {"a format it cannot read is passed over", {{3, 1, mapsA2}, {3, 10, format6}}, 2},
      {"variation sequences are not a map", {{3, 1, mapsA1}, {0, 5, mapsA2}}, 1},
      {"Mac Roman alone is no Unicode map", {{1, 0, mapsA1}}, -1},
  };
  for (const Case& c : cases) {
    Font font = threeGlyphs();
    font.extraTables = {{"cmap", cmapTable(c.subtables)}};
    const Bytes data = buildFont(font);
    Face face;
    if (c.expected < 0) {
      EXPECT_EQ(FaceStatus::Unsupported, parse(data, &face)) << c.what;
    } else {
      EXPECT_EQ(c.expected, lookup(data, 'A')) << c.what;
    }
  }
}

TEST(TtfFace, HmtxIsReadAsFreeTypeReadsIt) {
  Font font = threeGlyphs();
  font.numHMetrics = 2;  // glyph 2 shares glyph 1's advance and keeps its own left side bearing
  font.glyphs[2].lsbIsXMin = false;
  font.glyphs[2].lsb = 33;
  const Bytes data = buildFont(font);
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  uint16_t advance = 0;
  int16_t lsb = 0;
  horizontalMetrics(bytes, face, 1, &advance, &lsb);
  EXPECT_EQ(640, advance);
  EXPECT_EQ(20, lsb);
  horizontalMetrics(bytes, face, 2, &advance, &lsb);
  EXPECT_EQ(640, advance);
  EXPECT_EQ(33, lsb);
  // Past the table, both read 0 rather than anything outside it.
  face.hmtxLength = 8;
  horizontalMetrics(bytes, face, 2, &advance, &lsb);
  EXPECT_EQ(640, advance);
  EXPECT_EQ(0, lsb);
  face.hmtxLength = 2;
  horizontalMetrics(bytes, face, 1, &advance, &lsb);
  EXPECT_EQ(0, advance);
  EXPECT_FALSE(bytes.outOfRange());
}

TEST(TtfFace, AdvancesAreTheConvertersTwelveFour) {
  const Bytes data = buildFont(threeGlyphs());
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  EXPECT_EQ(122356, unitsToPixels(face, 1867));
  EXPECT_EQ(233, advance12_4(bytes, face, 0, 1867));  // 500 units: 955906 in 16.16, 14.59 px
  EXPECT_EQ(299, advance12_4(bytes, face, 1, 1867));  // 640 units: 1223560 in 16.16
  EXPECT_EQ(80, advance12_4(bytes, face, 0, 640));    // 500 units at 10 px: 4.99998 px rounds to 5
}

TEST(TtfFace, LineMetricsAreTheConverters) {
  Font font = threeGlyphs();
  font.ascender = 1057;  // Bookerly's hhea
  font.descender = -261;
  const Bytes data = buildFont(font);
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(data, &face));
  const LineMetrics m = lineMetrics(face, 1867);  // FreeType: height 2432, ascender 1984, descender -512
  EXPECT_EQ(38, m.advanceY);
  EXPECT_EQ(31, m.ascender);
  EXPECT_EQ(-8, m.descender);
}

TEST(TtfFace, Os2MetricsAreUsedWhereFreeTypeUsesThem) {
  Font font = threeGlyphs();
  font.os2 = true;
  font.typoAscender = 900;
  font.typoDescender = -300;
  font.typoLineGap = 100;
  font.winAscent = 1100;
  font.winDescent = 400;
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  EXPECT_EQ(800, face.ascender);  // hhea, while OS/2 does not ask for its typo metrics

  font.os2Selection = 0x80;  // USE_TYPO_METRICS
  ASSERT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  EXPECT_EQ(900, face.ascender);
  EXPECT_EQ(-300, face.descender);
  EXPECT_EQ(1300, face.height);

  font.os2Selection = 0;
  font.ascender = 0;  // hhea empty: OS/2's typo metrics
  font.descender = 0;
  ASSERT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  EXPECT_EQ(900, face.ascender);

  font.typoAscender = 0;  // and those empty too: the win metrics
  font.typoDescender = 0;
  ASSERT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
  EXPECT_EQ(1100, face.ascender);
  EXPECT_EQ(-400, face.descender);
  EXPECT_EQ(1500, face.height);
}

TEST(TtfFace, BoldIsMacStyleOrAWeightOf600InEitherOutlineFormat) {
  // Spec 7.1: read alike from a TrueType face and a CFF one.
  for (const bool cff : {false, true}) {
    const auto bold = [cff](const uint16_t macStyle, const bool os2, const uint16_t weight) {
      Font font = threeGlyphs();
      if (cff) {
        font.sfntVersion = tagOf("OTTO");
        font.extraTables = {{"CFF ", buildCff(CffSpec{})}};
      }
      font.macStyle = macStyle;
      font.os2 = os2;
      font.weightClass = weight;
      Face face;
      EXPECT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
      return face.bold;
    };
    EXPECT_FALSE(bold(0, false, 400)) << cff;
    EXPECT_TRUE(bold(1, false, 400)) << cff;
    EXPECT_FALSE(bold(2, false, 400)) << cff;  // italic
    EXPECT_FALSE(bold(0, true, 599)) << cff;
    EXPECT_TRUE(bold(0, true, 600)) << cff;
  }
}

namespace {

// A post table: the format at offset 0, the 16.16 italicAngle at 4, zeros after.
Bytes postTable(const uint32_t format, const int32_t italicAngle, const size_t bytes = 32) {
  Bytes post(bytes, 0);
  for (int i = 0; i < 4; ++i) {
    post[i] = static_cast<uint8_t>(format >> (24 - 8 * i));
    post[4 + i] = static_cast<uint8_t>(static_cast<uint32_t>(italicAngle) >> (24 - 8 * i));
  }
  return post;
}

bool fileIsItalic(const char* name) {
  const Bytes data = readFont(name);
  Face face;
  EXPECT_EQ(FaceStatus::Ok, parse(data, &face)) << name;
  return face.italic;
}

}  // namespace

TEST(TtfFace, ItalicIsMacStyleFsSelectionOrItalicAngleInEitherOutlineFormat) {
  // Spec 7.4. OS/2 and post are read as FreeType accepts them: one too short (for its version), or a post table
  // of a format FreeType refuses, counts as missing.
  for (const bool cff : {false, true}) {
    const auto italic = [cff](const uint16_t macStyle, const bool os2, const uint16_t fsSelection,
                              const uint16_t os2Bytes = 78, const Bytes& post = {}) {
      Font font = threeGlyphs();
      if (cff) {
        font.sfntVersion = tagOf("OTTO");
        font.extraTables = {{"CFF ", buildCff(CffSpec{})}};
      }
      if (!post.empty()) font.extraTables.push_back({"post", post});
      font.macStyle = macStyle;
      font.os2 = os2;
      font.os2Selection = fsSelection;
      font.os2Bytes = os2Bytes;
      Face face;
      EXPECT_EQ(FaceStatus::Ok, parse(buildFont(font), &face));
      return face.italic;
    };
    EXPECT_FALSE(italic(0, false, 0)) << cff;
    EXPECT_TRUE(italic(2, false, 0)) << cff;
    EXPECT_FALSE(italic(1, false, 0)) << cff;                  // bold
    EXPECT_TRUE(italic(0, true, 1)) << cff;                    // ITALIC
    EXPECT_TRUE(italic(0, true, 1u << 9)) << cff;              // OBLIQUE
    EXPECT_FALSE(italic(0, true, 0x20 | 0x40 | 0x80)) << cff;  // BOLD, REGULAR, USE_TYPO_METRICS
    EXPECT_FALSE(italic(0, true, 1, 64)) << cff;               // a 64-byte OS/2: missing
    EXPECT_TRUE(italic(2, true, 0, 64)) << cff;                // macStyle still counts
    EXPECT_TRUE(italic(0, false, 0, 78, postTable(0x00030000, -15 * 0x10000))) << cff;
    EXPECT_TRUE(italic(0, false, 0, 78, postTable(0x00020000, 12 * 0x10000, 64))) << cff;  // glyph names follow
    EXPECT_FALSE(italic(0, false, 0, 78, postTable(0x00030000, 0))) << cff;
    EXPECT_FALSE(italic(0, false, 0, 78, postTable(0x00030000, -15 * 0x10000, 31))) << cff;  // too short
    EXPECT_FALSE(italic(0, false, 0, 78, postTable(0x00040000, -15 * 0x10000))) << cff;      // format refused
  }
  EXPECT_TRUE(fileIsItalic("OpenDyslexic/OpenDyslexic-Italic.otf"));  // by its angle alone
  EXPECT_FALSE(fileIsItalic("OpenDyslexic/OpenDyslexic-Regular.otf"));
  EXPECT_TRUE(fileIsItalic("Bookerly/Bookerly-Italic.ttf"));
  EXPECT_FALSE(fileIsItalic("Bookerly/Bookerly-Regular.ttf"));
}

TEST(TtfFace, HugeAdvancesNeitherOverflowNorWrap) {
  // A lying hmtx on a tiny em: the 16.16 linear advance passes 2^31, and the 12.4 result passes 16 bits.
  Font font = threeGlyphs();
  font.unitsPerEm = 16;
  font.glyphs[1].advance = 63550;
  Bytes data = buildFont(font);
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  EXPECT_EQ(524288u, advance12_4(bytes, face, 1, 528));  // 2147481600 in 16.16: the + 2048 used to overflow

  font = threeGlyphs();
  font.unitsPerEm = 2048;
  font.glyphs[1].advance = 65535;
  data = buildFont(font);
  bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  EXPECT_EQ(130558u, advance12_4(bytes, face, 1, 255 << 6));  // 8,160 px: wrapped to 4,064 px in 16 bits
}
