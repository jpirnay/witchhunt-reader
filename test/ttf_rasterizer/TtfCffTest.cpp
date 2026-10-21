// The CFF table of OpenType fonts (TtfCff): INDEXes, DICTs, the font DICTs of CID-keyed fonts and the font matrix,
// read as FreeType's CFF driver reads them. The driver's own parse of the table (its CFF_Font, reached through
// FreeType's internal headers) is the oracle wherever a test can reach it.
#include <ft2build.h>
#include FT_FREETYPE_H
#include <freetype/internal/cfftypes.h>
#include <freetype/internal/services/svcfftl.h>
#include <freetype/internal/tttypes.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "CffBuilder.h"
#include "TtfCff.h"
#include "TtfFace.h"
#include "TtfRasterizer.h"
#include "TtfTestUtil.h"

using namespace ttf;
using namespace ttf_test;

namespace {

FT_Library library() {
  static FT_Library lib = [] {
    FT_Library l = nullptr;
    FT_Init_FreeType(&l);
    return l;
  }();
  return lib;
}

std::vector<uint8_t> readCffTestFont(const std::string& name) {
  return name.rfind("OpenDyslexic", 0) == 0 ? readFont(name) : readFixture(name);
}

FaceStatus parse(const std::vector<uint8_t>& data, Face* face) {
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  return parseFace(bytes, face);
}

// FreeType's face of a font and the CFF driver's parse of its CFF table.
class FreeTypeCff {
 public:
  explicit FreeTypeCff(const std::vector<uint8_t>& font) {
    if (FT_New_Memory_Face(library(), font.data(), static_cast<FT_Long>(font.size()), 0, &face_) != 0) face_ = nullptr;
  }
  ~FreeTypeCff() {
    if (face_) FT_Done_Face(face_);
  }
  FreeTypeCff(const FreeTypeCff&) = delete;
  FreeTypeCff& operator=(const FreeTypeCff&) = delete;

  bool ok() const { return face_ != nullptr; }
  FT_Face face() const { return face_; }
  CFF_Font cff() const { return static_cast<CFF_Font>(reinterpret_cast<TT_Face>(face_)->extra.data); }

  // The FD cff_slot_load picks for `glyph`: cff_fd_select_get (without its cache) over FreeType's own copy of the
  // FDSelect. -1 for a name-keyed font.
  int fd(const uint32_t glyph) const {
    const CFF_Font font = cff();
    if (font->num_subfonts == 0) return -1;
    const CFF_FDSelectRec& select = font->fd_select;
    if (!select.data) return 0;
    if (select.format == 0) return select.data[glyph];
    const FT_Byte* p = select.data;
    const FT_Byte* limit = p + select.data_size;
    uint32_t first = static_cast<uint32_t>(p[0] << 8 | p[1]);
    p += 2;
    do {
      if (glyph < first) break;
      const int fd = *p++;
      const uint32_t next = static_cast<uint32_t>(p[0] << 8 | p[1]);
      p += 2;
      if (glyph < next) return fd;
      first = next;
    } while (p < limit);
    return 0;
  }

  // The glyph's font DICT and Private DICT, as the decoder uses them (cff_decoder_prepare); null when the FD is past
  // the FDArray, a glyph FreeType refuses.
  CFF_SubFont subfont(const uint32_t glyph) const {
    const int index = fd(glyph);
    if (index < 0) return &cff()->top_font;
    return static_cast<FT_UInt>(index) < cff()->num_subfonts ? cff()->subfonts[index] : nullptr;
  }

 private:
  FT_Face face_ = nullptr;
};

void expectIndex(const CFF_IndexRec& theirs, const CffIndex& ours, const std::string& what) {
  EXPECT_EQ(theirs.count, uint32_t{ours.count}) << what;
  if (theirs.count == 0) return;
  EXPECT_EQ(theirs.start, ours.start) << what;
  EXPECT_EQ(theirs.off_size, ours.offSize) << what;
  EXPECT_EQ(theirs.data_offset, ours.dataStart) << what;
  EXPECT_EQ(theirs.data_offset + theirs.data_size, ours.end) << what;
}

void expectMatrix(const CFF_FontRecDictRec& theirs, const CffMatrix& ours, const std::string& what) {
  EXPECT_EQ(theirs.font_matrix.xx, ours.xx) << what;
  EXPECT_EQ(theirs.font_matrix.xy, ours.xy) << what;
  EXPECT_EQ(theirs.font_matrix.yx, ours.yx) << what;
  EXPECT_EQ(theirs.font_matrix.yy, ours.yy) << what;
  EXPECT_EQ(theirs.font_offset.x, ours.dx) << what;
  EXPECT_EQ(theirs.font_offset.y, ours.dy) << what;
  EXPECT_EQ(theirs.units_per_em, ours.upm) << what;
}

// Every glyph's source against FreeType: the charstring as the CharStrings INDEX's offsets give it, the local
// subroutines, the matrix and the scale of the font DICT FreeType draws it with. A glyph FreeType refuses for its FD
// is Malformed.
void expectGlyphSourcesAreFreeTypes(const std::vector<uint8_t>& data, const std::string& what) {
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face)) << what;
  const FreeTypeCff ft(data);
  ASSERT_TRUE(ft.ok()) << what;
  const CFF_Font cff = ft.cff();
  const CffIndex& charStrings = face.cff.charStrings;
  const auto offsetAt = [&](const uint32_t i) {
    uint32_t value = 0;
    for (uint32_t b = 0; b < charStrings.offSize; ++b)
      value = value << 8 | data[charStrings.start + 3 + i * charStrings.offSize + b];
    return value;
  };
  for (uint32_t g = 0; g < face.numGlyphs; ++g) {
    const std::string glyph = what + " glyph " + std::to_string(g);
    bytes.clearErrors();
    CffGlyphSource source;
    const Status status = cffGlyphSource(bytes, face, static_cast<uint16_t>(g), &source);
    const CFF_SubFont sub = ft.subfont(g);
    if (!sub) {
      EXPECT_EQ(Status::Malformed, status) << glyph;
      continue;
    }
    ASSERT_EQ(Status::Ok, status) << glyph;
    EXPECT_EQ(charStrings.dataStart + offsetAt(g) - 1, source.offset) << glyph;
    EXPECT_EQ(offsetAt(g + 1) - offsetAt(g), source.length) << glyph;
    expectIndex(sub->local_subrs_index, source.localSubrs, glyph + " local subroutines");
    expectMatrix(sub->font_dict, source.matrix, glyph);
    const FT_ULong top = cff->top_font.font_dict.units_per_em;
    const FT_ULong own = sub->font_dict.units_per_em;
    EXPECT_EQ(top == own ? 1 : top, source.scaleNum) << glyph;
    EXPECT_EQ(top == own ? 1 : own, source.scaleDen) << glyph;
  }
}

}  // namespace

TEST(TtfCff, EveryCffTestFontParses) {
  for (const std::string& name : cffTestFonts()) {
    const std::vector<uint8_t> data = readCffTestFont(name);
    ASSERT_FALSE(data.empty()) << name;
    Face face;
    ASSERT_EQ(FaceStatus::Ok, parse(data, &face)) << name;
    EXPECT_EQ(Face::Outlines::Cff, face.outlines) << name;
    const FreeTypeCff ft(data);
    ASSERT_TRUE(ft.ok()) << name;
    EXPECT_EQ(ft.face()->num_glyphs, face.numGlyphs) << name;
    EXPECT_EQ(ft.face()->units_per_EM, face.unitsPerEm) << name;
    EXPECT_EQ(name.rfind("Noto", 0) == 0, face.cff.cid) << name;
    EXPECT_EQ(0, face.maxPoints) << name;  // the rasterizer plans a CFF face at its caps
    EXPECT_EQ(0, face.maxContours) << name;
  }
}

TEST(TtfCff, OpenDyslexicRegularIsNameKeyedWithSubroutines) {
  const std::vector<uint8_t> data = readFont("OpenDyslexic/OpenDyslexic-Regular.otf");
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(data, &face));
  EXPECT_EQ(504, face.cff.globalSubrs.count);
  EXPECT_EQ(408, face.cff.localSubrs.count);
  EXPECT_EQ(0, face.cff.fdArray.count);
  EXPECT_FALSE(face.cff.cid);
}

TEST(TtfCff, TheIndexesAndTheTopFontAreFreeTypes) {
  for (const std::string& name : cffTestFonts()) {
    const std::vector<uint8_t> data = readCffTestFont(name);
    Face face;
    ASSERT_EQ(FaceStatus::Ok, parse(data, &face)) << name;
    const FreeTypeCff ft(data);
    ASSERT_TRUE(ft.ok()) << name;
    const CFF_Font cff = ft.cff();
    expectIndex(cff->charstrings_index, face.cff.charStrings, name + " CharStrings");
    expectIndex(cff->global_subrs_index, face.cff.globalSubrs, name + " global subroutines");
    EXPECT_EQ(cff->num_subfonts, uint32_t{face.cff.fdArray.count}) << name;
    expectMatrix(cff->top_font.font_dict, face.cff.matrix, name + " top");
    EXPECT_EQ(cff->top_font.font_dict.has_font_matrix != 0, face.cff.hasFontMatrix) << name;
    if (!face.cff.cid) expectIndex(cff->top_font.local_subrs_index, face.cff.localSubrs, name + " local subroutines");
  }
}

TEST(TtfCff, EveryGlyphHasACharStringAndAFontDict) {
  for (const std::string& name : cffTestFonts()) {
    const std::vector<uint8_t> data = readCffTestFont(name);
    FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
    Face face;
    ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face)) << name;
    const FreeTypeCff ft(data);
    ASSERT_TRUE(ft.ok()) << name;
    for (uint32_t g = 0; g < face.numGlyphs; ++g) {
      bytes.clearErrors();
      CffGlyphSource source;
      ASSERT_EQ(Status::Ok, cffGlyphSource(bytes, face, static_cast<uint16_t>(g), &source)) << name << " glyph " << g;
      EXPECT_GE(source.offset, face.cff.charStrings.dataStart) << name << " glyph " << g;
      EXPECT_LE(source.offset + source.length, face.cff.charStrings.end) << name << " glyph " << g;
      if (source.length == 0) {
        ASSERT_EQ(0, FT_Load_Glyph(ft.face(), g, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING)) << name << " glyph " << g;
        EXPECT_EQ(0, ft.face()->glyph->outline.n_contours) << name << " glyph " << g;
      }
    }
  }
}

TEST(TtfCff, TheGlyphSourcesAreFreeTypes) {
  for (const std::string& name : cffTestFonts()) expectGlyphSourcesAreFreeTypes(readCffTestFont(name), name);
}

TEST(TtfCff, SubroutinesAreTheItemsFreeTypeCalls) {
  // FreeType keeps a pointer per subroutine (cff_index_get_pointers): each item lies where FreeType's does.
  for (const std::string& name : cffTestFonts()) {
    const std::vector<uint8_t> data = readCffTestFont(name);
    FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
    Face face;
    ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face)) << name;
    const FreeTypeCff ft(data);
    ASSERT_TRUE(ft.ok()) << name;
    const auto compare = [&](const CFF_IndexRec& theirs, FT_Byte** pointers, const CffIndex& ours, const char* which) {
      ASSERT_EQ(theirs.count, uint32_t{ours.count}) << name << " " << which;
      for (uint32_t i = 0; i < ours.count; ++i) {
        uint32_t offset = 0;
        uint32_t length = 0;
        ASSERT_TRUE(cffIndexItem(bytes, ours, i, &offset, &length)) << name << " " << which << " " << i;
        EXPECT_EQ(theirs.data_offset + static_cast<FT_ULong>(pointers[i] - theirs.bytes), offset)
            << name << " " << which << " " << i;
        EXPECT_EQ(static_cast<FT_ULong>(pointers[i + 1] - pointers[i]), length) << name << " " << which << " " << i;
      }
      uint32_t offset = 0;
      uint32_t length = 0;
      EXPECT_FALSE(cffIndexItem(bytes, ours, ours.count, &offset, &length)) << name << " " << which;
    };
    compare(ft.cff()->global_subrs_index, ft.cff()->global_subrs, face.cff.globalSubrs, "global");
    if (!face.cff.cid)
      compare(ft.cff()->top_font.local_subrs_index, ft.cff()->top_font.local_subrs, face.cff.localSubrs, "local");
  }
}

TEST(TtfCff, ASlantedMatrixIsKept) {
  for (const std::string& name : cffTestFonts()) {
    const std::vector<uint8_t> data = readCffTestFont(name);
    FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
    Face face;
    ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face)) << name;
    CffGlyphSource source;
    ASSERT_EQ(Status::Ok, cffGlyphSource(bytes, face, 1, &source)) << name;
    if (name == "NotoSansJP-CffSlanted.otf") {
      EXPECT_EQ(0x10000, source.matrix.xx);
      EXPECT_EQ(0x10000, source.matrix.yy);
      EXPECT_NE(0, source.matrix.xy);
      EXPECT_EQ(0, source.matrix.yx);
      EXPECT_FALSE(source.matrix.identity());
    } else {
      EXPECT_TRUE(source.matrix.identity()) << name;
    }
    EXPECT_EQ(1u, source.scaleNum) << name;
    EXPECT_EQ(1u, source.scaleDen) << name;
  }
}

TEST(TtfCff, FontMatricesAreReadAndConcatenatedAsFreeTypeDoes) {
  // FreeType reads each FontMatrix to the most precise 16.16 values and a units-per-em (cff_parse_font_matrix),
  // concatenates a font DICT's with the Top DICT's and normalizes both so yy is one (cff_face_init). Integers and
  // reals of every form, offsets, degenerate matrices (the default), and a scale a font DICT changes.
  const char* const matrices[] = {
      "",
      "0.001 0 0 0.001 0 0",
      "0.0005 0 0 0.0005 0 0",
      "1 0 0 1 0 0",
      "0.001 0 0.0002 0.001 0 0",
      "1E-3 0 0 .001 0 0",
      "0.0010 0 0 1.0E-3 12 -7",
      "-0.001 0 0 -0.001 0 0",
      "0 0.001 0.001 0 0 0",
      "2.5E-4 0 0 2.5E-4 0.5 0.25",
      "0.00048828125 0 0.00016 0.00048828125 0 0",
      "0.001 0.0003 -0.00025 0.0012 3.75 -1.5",
      "0.001 0 0 0.000001 0 0",
      "0 0 0 0 0 0",
      "1 0 0 1 0",
      "100000 0 0 100000 0 0",
      "0.001 0 0 0.001 5E-2 -2E-2",
      "10E-4 0 0 0.001 0 0",
  };
  // An offset of 1,000 em takes FreeType's arithmetic past 32 bits. Its FT_Long is 32 bits on Windows and 64 on
  // Linux, and the two builds differ; the engine computes as the 64-bit one (TtfFixed.h).
  const char* const past32Bits[] = {"0.001 0 0 0.001 1E3 0"};
  const auto compare = [](const char* top, const char* fd) {
    CffSpec spec;
    spec.cid = true;
    spec.topMatrix = top;
    spec.fds = {{fd, {}, ""}};
    const std::string what = std::string("top [") + top + "] fd [" + fd + "]";
    const Bytes font = buildOtf(spec);
    const FreeTypeCff ft(font);
    Face face;
    const FaceStatus status = parse(font, &face);
    ASSERT_EQ(ft.ok(), status == FaceStatus::Ok) << what;
    if (!ft.ok()) return;
    expectMatrix(ft.cff()->top_font.font_dict, face.cff.matrix, what + " top");
    expectGlyphSourcesAreFreeTypes(font, what);
  };
  for (const char* fd : {"", "0.001 0 0 0.001 0 0", "0.0005 0 0 0.00025 0 0", "1 0 0 1 0 0", "0.001 0 0.0003 0.001 5 5",
                         "0.002 0 0 0.002 -10 20"}) {
    for (const char* top : matrices) compare(top, fd);
    if (sizeof(FT_Long) == 8) {
      for (const char* top : past32Bits) compare(top, fd);
    }
  }
  // A name-keyed font has only the Top DICT's.
  for (const char* top : matrices) {
    CffSpec spec;
    spec.topMatrix = top;
    spec.fds = {{"", {}, ""}};
    const Bytes font = buildOtf(spec);
    const FreeTypeCff ft(font);
    Face face;
    ASSERT_EQ(ft.ok(), parse(font, &face) == FaceStatus::Ok) << top;
    if (ft.ok()) expectGlyphSourcesAreFreeTypes(font, std::string("name-keyed [") + top + "]");
  }
}

TEST(TtfCff, EachGlyphHasItsFontDictsSubroutines) {
  // Three font DICTs with 1, 2 and 3 local subroutines; FDSelect format 0 and format 3 assign them.
  CffSpec spec;
  spec.cid = true;
  spec.charStrings.assign(6, Bytes{14});
  spec.fds = {{"", {{11}}, ""}, {"", {{11}, {11}}, ""}, {"", {{11}, {11}, {11}}, ""}};
  spec.fdSelect = {0, 2, 0, 1, 1, 2, 0};
  expectGlyphSourcesAreFreeTypes(buildOtf(spec), "format 0");
  spec.fdSelect = {3, 0, 3, 0, 0, 1, 0, 2, 2, 0, 4, 1, 0, 6};  // 0-1: FD 1, 2-3: FD 2, 4-5: FD 1
  expectGlyphSourcesAreFreeTypes(buildOtf(spec), "format 3");
  spec.fdSelect = {3, 0, 3, 0, 0, 1, 0, 3, 2, 0, 3, 0, 0, 6};  // 0-2: FD 1, then an empty range, 3-5: FD 0
  expectGlyphSourcesAreFreeTypes(buildOtf(spec), "format 3, an empty range");
  spec.fdSelect = {3, 0, 1, 0, 2, 2, 0, 4};  // the first range starts late, the sentinel ends early: FD 0
  expectGlyphSourcesAreFreeTypes(buildOtf(spec), "format 3, glyphs outside the ranges");
}

TEST(TtfCff, AnFdPastTheFdArrayIsMalformed) {
  // cff_decoder_prepare refuses the glyph; the others draw.
  CffSpec spec;
  spec.cid = true;
  spec.fds = {{"", {{11}}, ""}, {"", {}, ""}};
  spec.fdSelect = {0, 0, 2, 1};
  const Bytes font = buildOtf(spec);
  FontBytes bytes(font.data(), static_cast<uint32_t>(font.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  CffGlyphSource source;
  EXPECT_EQ(Status::Ok, cffGlyphSource(bytes, face, 0, &source));
  EXPECT_EQ(Status::Malformed, cffGlyphSource(bytes, face, 1, &source));
  EXPECT_EQ(Status::Ok, cffGlyphSource(bytes, face, 2, &source));
  const FreeTypeCff ft(font);
  ASSERT_TRUE(ft.ok());
  EXPECT_NE(0, FT_Load_Glyph(ft.face(), 1, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING));
  expectGlyphSourcesAreFreeTypes(font, "FD 2 of 2");
}

TEST(TtfCff, AGlyphPastTheCharStringsIsMalformed) {
  const std::vector<uint8_t> data = readFixture("CffOperators.otf");
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  CffGlyphSource source;
  EXPECT_EQ(Status::Ok, cffGlyphSource(bytes, face, static_cast<uint16_t>(face.numGlyphs - 1), &source));
  EXPECT_EQ(Status::Malformed, cffGlyphSource(bytes, face, face.numGlyphs, &source));
  EXPECT_EQ(0u, source.length);
}

TEST(TtfCff, ASubrsOffsetGivenAsARealIsTruncated) {
  // cff_parse_num truncates a real DICT number to an integer: 11.75 is the Private DICT's 11 bytes (defaultWidthX in
  // 6, then the real in 4 and its operator).
  CffSpec spec;
  spec.fds = {{"", {{11}, {11}}, "11.75"}};
  const Bytes font = buildOtf(spec);
  ASSERT_TRUE(FreeTypeCff(font).ok());
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(font, &face));
  EXPECT_EQ(2, face.cff.localSubrs.count);
  expectGlyphSourcesAreFreeTypes(font, "Subrs 12.75");
}

TEST(TtfCff, TablesAreRefusedWhereFreeTypeRefusesThem) {
  // cff_font_load's checks: the header, the names, the FDSelect, the charset and the Encoding; a table FreeType
  // reads (charset ranges running into the next structure) is read alike.
  struct Case {
    const char* what;
    bool refused;  // by FreeType
    CffSpec spec;
  };
  std::vector<Case> cases;
  const auto add = [&](const char* what, const bool refused, const auto& change) {
    CffSpec spec;
    spec.cid = true;
    spec.fds = {{"", {{11}}, ""}};
    change(spec);
    cases.push_back({what, refused, spec});
  };
  add("a well-formed CID-keyed font", false, [](CffSpec&) {});
  add("a well-formed name-keyed font", false, [](CffSpec& s) { s.cid = false; });
  add("a CFF2 header", true, [](CffSpec& s) { s.header = {2, 0, 5, 0, 0}; });
  add("a header of 3 bytes", true, [](CffSpec& s) { s.header = {1, 0, 3}; });
  add("an offset size of 5 in the header", true, [](CffSpec& s) { s.header = {1, 0, 4, 5}; });
  add("a longer header", false, [](CffSpec& s) { s.header = {1, 0, 6, 4, 0, 0}; });
  add("two names", true, [](CffSpec& s) { s.names = {"One", "Two"}; });
  add("FDSelect format 1", true, [](CffSpec& s) { s.fdSelect = {1, 0, 0, 0}; });
  add("FDSelect format 3 without ranges", true, [](CffSpec& s) { s.fdSelect = {3, 0, 0, 0, 3}; });
  add("a charset format 3", true, [](CffSpec& s) { s.charset = {3, 0, 1, 0, 2}; });
  add("a charset format 1 running into the FDSelect", false, [](CffSpec& s) { s.charset = {1, 0, 1, 0}; });
  add("a predefined charset larger than the font may use", true, [](CffSpec& s) {
    s.cid = false;
    s.charsetOffset = 2;
    s.charStrings.assign(88, Bytes{14});
  });
  add("ExpertSubset, as many glyphs as it has", false, [](CffSpec& s) {
    s.cid = false;
    s.charsetOffset = 2;
    s.charStrings.assign(87, Bytes{14});
  });
  add("an Encoding of format 2", true, [](CffSpec& s) {
    s.cid = false;
    s.encoding = {2, 1, 65};
  });
  add("an Encoding of format 0 with supplements", false, [](CffSpec& s) {
    s.cid = false;
    s.encoding = {0x80, 2, 65, 66, 1, 67, 0, 36};
  });
  add("an Encoding of format 1", false, [](CffSpec& s) {
    s.cid = false;
    s.encoding = {1, 1, 65, 1};
  });
  for (Case& c : cases) {
    const Bytes font = buildOtf(c.spec);
    const FreeTypeCff ft(font);
    EXPECT_EQ(c.refused, !ft.ok()) << c.what;
    Face face;
    EXPECT_EQ(ft.ok() ? FaceStatus::Ok : FaceStatus::Malformed, parse(font, &face)) << c.what;
  }
}

TEST(TtfCff, ACidKeyedFontHasOneTo256FontDicts) {
  // FreeType reads a CID-keyed font without font DICTs, or with more than it can hold, with the Top DICT's (which
  // it never gives a Private DICT); the engine refuses both.
  for (const size_t fds : {size_t{0}, size_t{1}, size_t{256}, size_t{257}}) {
    CffSpec spec;
    spec.cid = true;
    spec.fds.assign(fds, CffFontDict{});
    Face face;
    EXPECT_EQ(fds >= 1 && fds <= 256 ? FaceStatus::Ok : FaceStatus::Malformed, parse(buildOtf(spec), &face)) << fds;
  }
}

TEST(TtfCff, StandardCharactersAreFoundThroughTheCharset) {
  // seac names its components by StandardEncoding code; FreeType maps the code to a SID and takes the first glyph
  // whose charset entry is that SID (cff_lookup_glyph_by_stdcharcode). FreeType's own charset and table are the
  // oracle: predefined charsets, formats 0, 1 and 2, and the charset of a CID-keyed font, whose entries are CIDs.
  std::vector<std::pair<std::string, Bytes>> fonts;
  const auto nameKeyed = [&](const std::string& what, const int32_t predefined, const Bytes& charset,
                             const uint32_t glyphs) {
    CffSpec spec;
    spec.charStrings.assign(glyphs, Bytes{14});
    spec.charsetOffset = predefined;
    spec.charset = charset;
    fonts.emplace_back(what, buildOtf(spec));
  };
  nameKeyed("ISOAdobe", 0, {}, 229);
  nameKeyed("ISOAdobe, 40 glyphs", 0, {}, 40);
  nameKeyed("Expert", 1, {}, 166);
  nameKeyed("ExpertSubset", 2, {}, 87);
  nameKeyed("ExpertSubset, 12 glyphs", 2, {}, 12);
  Bytes format0 = {0};
  for (const uint16_t sid : {66, 34, 34, 124, 0, 95}) put16(format0, sid);
  nameKeyed("format 0", -1, format0, 7);
  nameKeyed("format 1", -1, {1, 0, 30, 20, 0, 34, 0, 0, 120, 200}, 224);
  nameKeyed("format 2", -1, {2, 0, 40, 0, 3, 0, 33, 1, 0}, 200);
  nameKeyed("format 1, a range past the last SID", -1, {1, 0xFF, 0xF0, 0xFF}, 30);
  fonts.emplace_back("NotoSansJP-Cff.otf", readFixture("NotoSansJP-Cff.otf"));
  fonts.emplace_back("CffOperators.otf", readFixture("CffOperators.otf"));
  fonts.emplace_back("OpenDyslexic-Regular.otf", readFont("OpenDyslexic/OpenDyslexic-Regular.otf"));
  for (const auto& [what, font] : fonts) {
    FontBytes bytes(font.data(), static_cast<uint32_t>(font.size()));
    Face face;
    ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face)) << what;
    const FreeTypeCff ft(font);
    ASSERT_TRUE(ft.ok()) << what;
    const CFF_Font cff = ft.cff();
    const auto* load = static_cast<FT_Service_CFFLoad>(cff->cffload);
    for (int32_t code = -2; code < 260; ++code) {
      int32_t expected = -1;
      if (code >= 0 && code <= 255 && cff->charset.sids) {
        const FT_UShort sid = load->get_standard_encoding(static_cast<FT_UInt>(code));
        for (FT_UInt n = 0; n < cff->num_glyphs; ++n) {
          if (cff->charset.sids[n] == sid) {
            expected = static_cast<int32_t>(n);
            break;
          }
        }
      }
      EXPECT_EQ(expected, cffStandardCharGlyph(bytes, face, code)) << what << " code " << code;
    }
  }
}

TEST(TtfCff, ASeacInACidKeyedFontFindsItsComponentsByCid) {
  // A CID-keyed font's charset holds CIDs, and FreeType searches it all the same: Adobe-Japan1 gives ASCII the CIDs
  // StandardEncoding gives it as SIDs, so a seac over 'A' and 'B' draws both. One glyph's charstring becomes
  // `0 0 'A' 'B' endchar`; FreeType loads it, with the contours of both.
  std::vector<uint8_t> data = readFixture("NotoSansJP-CffFlat.otf");
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  const uint16_t glyphA = static_cast<uint16_t>(cffStandardCharGlyph(bytes, face, 'A'));
  const uint16_t glyphB = static_cast<uint16_t>(cffStandardCharGlyph(bytes, face, 'B'));
  ASSERT_NE(0xFFFF, glyphA);
  ASSERT_NE(0xFFFF, glyphB);
  CffGlyphSource source;
  const uint16_t patched = 200;
  ASSERT_EQ(Status::Ok, cffGlyphSource(bytes, face, patched, &source));
  ASSERT_GE(source.length, 5u);
  const uint8_t seac[] = {139, 139, 139 + 'A', 139 + 'B', 14};
  std::copy(seac, seac + 5, data.begin() + source.offset);
  const FreeTypeCff ft(data);
  ASSERT_TRUE(ft.ok());
  const auto contours = [&](const uint16_t glyph) {
    EXPECT_EQ(0, FT_Load_Glyph(ft.face(), glyph, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING)) << glyph;
    return ft.face()->glyph->outline.n_contours;
  };
  EXPECT_EQ(contours(glyphA) + contours(glyphB), contours(patched));
}

namespace {

// Every byte of `intact`'s CFF table set to 0xFF, the file cut there, and the table directory's length for the CFF
// table ending there. parseFace reads only the copy (FontBytes over exactly it); a face it accepts gives every glyph a
// source inside its CFF table, or Malformed, and seac's lookups stay among its glyphs.
void sweepCorruptions(const std::vector<uint8_t>& intact, const std::string& name, uint32_t* accepted,
                      uint32_t* refused) {
  Face intactFace;
  ASSERT_EQ(FaceStatus::Ok, parse(intact, &intactFace)) << name;
  const uint32_t table = intactFace.cff.table;
  const uint32_t tableEnd = table + intactFace.cff.tableLength;
  const size_t lengthField = findRecord(intact, "CFF ") + 12;
  ASSERT_NE(12u, lengthField) << name;
  for (uint32_t at = table; at < tableEnd; ++at) {
    for (int variant = 0; variant < 3; ++variant) {
      std::vector<uint8_t> copy;
      uint32_t end = tableEnd;
      if (variant == 0) {
        copy = intact;
        copy[at] = 0xFF;
      } else if (variant == 1) {
        copy.assign(intact.begin(), intact.begin() + at);
      } else {
        copy = intact;
        set32(copy, lengthField, at - table);
        end = at;
      }
      const std::string where = name + " byte " + std::to_string(at) + " variant " + std::to_string(variant);
      FontBytes bytes(copy.data(), static_cast<uint32_t>(copy.size()));
      Face face;
      const FaceStatus status = parseFace(bytes, &face);
      if (status != FaceStatus::Ok) {
        ASSERT_EQ(FaceStatus::Malformed, status) << where;
        ++*refused;
        continue;
      }
      ++*accepted;
      ASSERT_LE(face.cff.charStrings.end, end) << where;
      ASSERT_LE(face.cff.globalSubrs.end, end) << where;
      for (uint32_t g = 0; g < face.numGlyphs; ++g) {
        bytes.clearErrors();
        CffGlyphSource source;
        const Status glyph = cffGlyphSource(bytes, face, static_cast<uint16_t>(g), &source);
        ASSERT_TRUE(glyph == Status::Ok || glyph == Status::Malformed) << where << " glyph " << g;
        if (glyph != Status::Ok) continue;
        if (source.length > 0) {
          ASSERT_GE(source.offset, table) << where << " glyph " << g;
          ASSERT_LE(source.offset + source.length, end) << where << " glyph " << g;
        }
        if (source.localSubrs.count > 0) {
          ASSERT_GE(source.localSubrs.start, table) << where << " glyph " << g;
          ASSERT_LE(source.localSubrs.end, end) << where << " glyph " << g;
        }
      }
      for (int32_t code = 0; code < 256; ++code) {
        bytes.clearErrors();
        const int32_t glyph = cffStandardCharGlyph(bytes, face, code);
        ASSERT_LT(glyph, static_cast<int32_t>(face.numGlyphs)) << where << " code " << code;
        ASSERT_FALSE(bytes.outOfRange()) << where << " code " << code;
      }
    }
  }
}

}  // namespace

TEST(TtfCff, ACorruptedCffIsRefusedOrDrawsNothing) {
  // Review focus 1, over a CID-keyed font with subroutines (NotoSansJP-Cff.otf) and a name-keyed one with an Encoding
  // and local subroutines (CffOperators.otf); every byte of their CFF tables, about 4 s.
  const auto started = std::chrono::steady_clock::now();
  uint32_t accepted = 0;
  uint32_t refused = 0;
  sweepCorruptions(readFixture("NotoSansJP-Cff.otf"), "NotoSansJP-Cff.otf", &accepted, &refused);
  sweepCorruptions(readFixture("CffOperators.otf"), "CffOperators.otf", &accepted, &refused);
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  std::printf("corruption sweep: %u copies accepted, %u refused, %lld ms\n", accepted, refused,
              static_cast<long long>(ms));
  EXPECT_GT(accepted, 0u);
  EXPECT_GT(refused, 0u);
}
