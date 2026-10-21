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
#include <cstring>
#include <string>
#include <vector>

#include "CffBuilder.h"
#include "TtfCff.h"
#include "TtfFace.h"
#include "TtfFamily.h"
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
    const FT_ULong top = cff->top_font.font_dict.units_per_em;
    const FT_ULong own = sub->font_dict.units_per_em;
    if (top != own && (top == 0 || own == 0 || top > INT32_MAX || own > INT32_MAX)) {
      EXPECT_EQ(Status::Malformed, status) << glyph << ": a scale of " << top << " / " << own;  // the engine's own rule
      continue;
    }
    ASSERT_EQ(Status::Ok, status) << glyph;
    EXPECT_EQ(charStrings.dataStart + offsetAt(g) - 1, source.offset) << glyph;
    EXPECT_EQ(offsetAt(g + 1) - offsetAt(g), source.length) << glyph;
    expectIndex(sub->local_subrs_index, source.localSubrs, glyph + " local subroutines");
    expectMatrix(sub->font_dict, source.matrix, glyph);
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
        // The interpreter's own read of a subroutine, from its two offsets.
        uint32_t subrOffset = 0;
        uint32_t subrLength = 0;
        ASSERT_TRUE(cffSubroutine(bytes, ours, i, &subrOffset, &subrLength)) << name << " " << which << " " << i;
        EXPECT_EQ(offset, subrOffset) << name << " " << which << " " << i;
        EXPECT_EQ(length, subrLength) << name << " " << which << " " << i;
      }
      uint32_t offset = 0;
      uint32_t length = 0;
      EXPECT_FALSE(cffIndexItem(bytes, ours, ours.count, &offset, &length)) << name << " " << which;
      EXPECT_FALSE(cffSubroutine(bytes, ours, ours.count, &offset, &length)) << name << " " << which;
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
  // A negative five-byte integer (operator 29) is a 32-bit two's complement number, and the engine reads it so.
  // FreeType's cff_parse_integer assembles it in an FT_ULong: with a 64-bit FT_Long (Linux) -10 becomes 4,294,967,286
  // and the matrix is dropped as strangely scaled; with a 32-bit one (Windows, the C3) it reads -10.
  const char* const fiveByteNegatives[] = {"0.001 0 0 0.001 #-10 #20", "0.002 0 0 0.002 #12 #-7"};
  const auto compare = [](const char* top, const char* fd) {
    CffSpec spec;
    spec.cid = true;
    spec.topMatrix = top;
    spec.fds = {cffFontDict(fd)};
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
    } else {
      for (const char* top : fiveByteNegatives) compare(top, fd);
    }
  }
  // A name-keyed font has only the Top DICT's.
  for (const char* top : matrices) {
    CffSpec spec;
    spec.topMatrix = top;
    spec.fds = {CffFontDict{}};
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
  spec.fds = {cffFontDict("", {{11}}), cffFontDict("", {{11}, {11}}), cffFontDict("", {{11}, {11}, {11}})};
  spec.fdSelect = {0, 2, 0, 1, 1, 2, 0};
  expectGlyphSourcesAreFreeTypes(buildOtf(spec), "format 0");
  spec.fdSelect = {3, 0, 3, 0, 0, 1, 0, 2, 2, 0, 4, 1, 0, 6};  // 0-1: FD 1, 2-3: FD 2, 4-5: FD 1
  expectGlyphSourcesAreFreeTypes(buildOtf(spec), "format 3");
  spec.fdSelect = {3, 0, 3, 0, 0, 1, 0, 3, 2, 0, 3, 0, 0, 6};  // 0-2: FD 1, then an empty range, 3-5: FD 0
  expectGlyphSourcesAreFreeTypes(buildOtf(spec), "format 3, an empty range");
  spec.fdSelect = {3, 0, 1, 0, 2, 2, 0, 4};  // the first range starts late, the sentinel ends early: FD 0
  expectGlyphSourcesAreFreeTypes(buildOtf(spec), "format 3, glyphs outside the ranges");
  // Ranges out of order: FreeType scans them in order, stopping at the first that starts past the glyph.
  spec.fdSelect = {3, 0, 2, 0, 2, 1, 0, 0, 2, 0, 6};  // 0-1: FD 0 (the first range starts at 2), 2-5: FD 2
  expectGlyphSourcesAreFreeTypes(buildOtf(spec), "format 3, a range starting before the one ahead of it");
  spec.fdSelect = {3, 0, 3, 0, 0, 1, 0, 4, 2, 0, 2, 0, 0, 6};  // 0-3: FD 1, 4-5: FD 0
  expectGlyphSourcesAreFreeTypes(buildOtf(spec), "format 3, a range ending before it starts");
}

namespace {

// Every glyph's source through one cache, in three orders, as without it.
void expectTheCacheChangesNothing(const Bytes& font, const std::string& what) {
  SCOPED_TRACE(what);
  FontBytes bytes(font.data(), static_cast<uint32_t>(font.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  std::vector<uint16_t> order;
  for (uint32_t g = 0; g < face.numGlyphs; ++g) order.push_back(static_cast<uint16_t>(g));
  std::vector<std::vector<uint16_t>> orders = {order, std::vector<uint16_t>(order.rbegin(), order.rend()), {}};
  for (uint32_t i = 0; i < order.size(); ++i) orders[2].push_back(order[(i * 7919u) % order.size()]);
  for (const std::vector<uint16_t>& glyphs : orders) {
    CffSourceCache cache;
    for (const uint16_t g : glyphs) {
      CffGlyphSource plain;
      CffGlyphSource cached;
      bytes.clearErrors();
      const Status expected = cffGlyphSource(bytes, face, g, &plain);
      bytes.clearErrors();
      ASSERT_EQ(expected, cffGlyphSource(bytes, face, g, &cached, &cache)) << g;
      ASSERT_EQ(0, std::memcmp(&plain.localSubrs, &cached.localSubrs, sizeof plain.localSubrs)) << g;
      ASSERT_EQ(0, std::memcmp(&plain.matrix, &cached.matrix, sizeof plain.matrix)) << g;
      ASSERT_EQ(plain.offset, cached.offset) << g;
      ASSERT_EQ(plain.length, cached.length) << g;
      ASSERT_EQ(plain.scaleNum, cached.scaleNum) << g;
      ASSERT_EQ(plain.scaleDen, cached.scaleDen) << g;
    }
  }
}

}  // namespace

TEST(TtfCff, ASourceCacheGivesWhatTheLookupGives) {
  CffSpec spec;
  spec.cid = true;
  spec.charStrings.assign(6, Bytes{14});
  spec.fds = {cffFontDict("", {{11}}), cffFontDict("0.0005 0 0 0.0005 0 0", {{11}, {11}}),
              cffFontDict("3 0 0 3 0 0", {{11}, {11}, {11}})};
  for (const Bytes& fdSelect : std::vector<Bytes>{{0, 2, 0, 1, 1, 2, 0},
                                                  {3, 0, 3, 0, 0, 1, 0, 2, 2, 0, 4, 1, 0, 6},
                                                  {3, 0, 3, 0, 0, 1, 0, 3, 2, 0, 3, 0, 0, 6},
                                                  {3, 0, 1, 0, 2, 2, 0, 4},
                                                  {3, 0, 2, 0, 2, 1, 0, 0, 2, 0, 6},
                                                  {3, 0, 3, 0, 0, 1, 0, 4, 2, 0, 2, 0, 0, 6},
                                                  {3, 0, 3, 0, 0, 0, 0, 3, 1, 0, 1, 2, 0, 4},
                                                  {3, 0, 2, 0, 0, 7, 0, 2, 1, 0, 6}}) {
    spec.fdSelect = fdSelect;
    expectTheCacheChangesNothing(buildOtf(spec), "FDSelect " + std::to_string(fdSelect.size()));
  }
  for (const std::string& name : cffTestFonts()) expectTheCacheChangesNothing(readCffTestFont(name), name);
}

TEST(TtfCff, AScaleThatWouldDivideByZeroOrPass32BitsIsMalformed) {
  // cff_slot_load scales a CID-keyed glyph by the Top DICT's units per em over its font DICT's. A FontMatrix of 3 em
  // leaves 0 units per em, one of [1E-9 0 0 4E-10] 2.5 billion: FreeType would divide by zero or scale wildly; the
  // engine refuses those glyphs, and gives the others a ratio of two non-zero 32-bit numbers.
  CffSpec spec;
  spec.cid = true;
  spec.fds = {cffFontDict("3 0 0 3 0 0"), cffFontDict("1E-9 0 0 4E-10 0 0"), cffFontDict("0.0005 0 0 0.0005 0 0")};
  spec.fdSelect = {0, 0, 1, 2};
  const Bytes font = buildOtf(spec);
  FontBytes bytes(font.data(), static_cast<uint32_t>(font.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  CffGlyphSource source;
  EXPECT_EQ(Status::Malformed, cffGlyphSource(bytes, face, 0, &source));
  EXPECT_EQ(Status::Malformed, cffGlyphSource(bytes, face, 1, &source));
  ASSERT_EQ(Status::Ok, cffGlyphSource(bytes, face, 2, &source));
  EXPECT_EQ(1000u, source.scaleNum);
  EXPECT_EQ(2000u, source.scaleDen);
  const FreeTypeCff ft(font);
  ASSERT_TRUE(ft.ok());
  EXPECT_EQ(0u, ft.cff()->subfonts[0]->font_dict.units_per_em);  // what the rule guards against
  EXPECT_GT(ft.cff()->subfonts[1]->font_dict.units_per_em, static_cast<FT_ULong>(INT32_MAX));
  expectGlyphSourcesAreFreeTypes(font, "scales");
}

TEST(TtfCff, ADictOverFourKilobytesIsRefused) {
  // The engine reads a CID-keyed glyph's font DICT and Private DICT again for every glyph, so their size bounds the
  // work a glyph costs. The test fonts' largest are 77 bytes (a Top DICT), 11 (a font DICT) and 36 (a Private DICT);
  // FreeType has no limit, the engine refuses a font with a DICT over 4,096 bytes.
  const auto parsed = [](const CffSpec& spec) {
    Face face;
    return parse(buildOtf(spec), &face);
  };
  for (const bool cid : {true, false}) {
    for (const size_t size : {size_t{4096}, size_t{4097}}) {
      const FaceStatus expected = size <= 4096 ? FaceStatus::Ok : FaceStatus::Malformed;
      const std::string what = std::string(cid ? "CID-keyed, " : "name-keyed, ") + std::to_string(size) + " bytes";
      CffSpec spec;
      spec.cid = cid;
      spec.fds = {CffFontDict{}};
      spec.topPad = cffDictPad(size - (cid ? 43 : 23));
      EXPECT_EQ(expected, parsed(spec)) << what << ", the Top DICT";
      spec.topPad.clear();
      spec.fds[0].privatePad = cffDictPad(size - 6);
      EXPECT_EQ(expected, parsed(spec)) << what << ", the Private DICT";
      if (!cid) continue;
      spec.fds[0].privatePad.clear();
      spec.fds[0].dictPad = cffDictPad(size - 11);
      EXPECT_EQ(expected, parsed(spec)) << what << ", the font DICT";
    }
  }
}

TEST(TtfCff, AnFdPastTheFdArrayIsMalformed) {
  // cff_decoder_prepare refuses the glyph; the others draw.
  CffSpec spec;
  spec.cid = true;
  spec.fds = {cffFontDict("", {{11}}), CffFontDict{}};
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
  spec.fds = {cffFontDict("", {{11}, {11}}, "11.75")};
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
    spec.fds = {cffFontDict("", {{11}})};
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

TEST(TtfCff, ACff2RecordCountsAsFreeTypeCountsIt) {
  // FreeType reads a CFF2 table before a CFF one, but it skips a table record that is empty or lies outside the file
  // (tt_face_lookup_table, tt_face_load_font_dir): such a font is read through its CFF table. A real CFF2 table, with
  // or without a CFF one, is not read here.
  Font font;
  font.sfntVersion = tagOf("OTTO");
  font.glyphs.resize(3);
  font.cmap = {{'A', 1}};
  const Bytes cff2 = {2, 0, 5, 0, 0};
  font.extraTables = {{"CFF2", Bytes{}}, {"CFF ", buildCff(CffSpec{})}};
  const Bytes empty = buildFont(font);
  font.extraTables[0].second = cff2;
  const Bytes both = buildFont(font);
  Bytes outside = both;
  set32(outside, findRecord(outside, "CFF2") + 8, static_cast<uint32_t>(outside.size()) + 16);
  for (const auto& [what, data] : {std::pair<const char*, const Bytes&>{"an empty CFF2 record", empty},
                                   std::pair<const char*, const Bytes&>{"a CFF2 record outside the file", outside}}) {
    EXPECT_TRUE(FreeTypeCff(data).ok()) << what;
    Face face;
    ASSERT_EQ(FaceStatus::Ok, parse(data, &face)) << what;
    EXPECT_EQ(Face::Outlines::Cff, face.outlines) << what;
  }
  Face face;
  EXPECT_EQ(FaceStatus::Unsupported, parse(both, &face)) << "a CFF2 table and a CFF table";
  font.extraTables.pop_back();
  EXPECT_EQ(FaceStatus::Unsupported, parse(buildFont(font), &face)) << "a CFF2 table alone";
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

namespace {

// Offset `i` of `index` (4-byte offsets, as CffBuilder writes them) in `font`.
void setIndexOffset(std::vector<uint8_t>& font, const CffIndex& index, const uint32_t i, const uint32_t value) {
  ASSERT_EQ(4, index.offSize);
  set32(font, index.start + 3 + i * 4, value);
}

}  // namespace

TEST(TtfCff, SubroutineIndexesMustBePlain) {
  // Ruling 8: the interpreter reads a subroutine from its two offsets, so every subroutine INDEX (global, the Private
  // DICT's, every font DICT's) must start at offset 1 and never decrease. FreeType makes any INDEX so
  // (cff_index_get_pointers: the first offset taken as 1, a running maximum); the engine refuses the font instead,
  // which only a broken or hostile font notices. The case that matters: 65,535 subroutines with zero offsets after the
  // first, each call of which cffIndexItem would answer by scanning them all.
  std::vector<Bytes> many(65535);
  CffSpec spec;
  spec.fds = {cffFontDict("", many)};
  spec.globalSubrs = {Bytes{11}, Bytes{11}, Bytes{11}};
  const std::vector<uint8_t> intact = buildOtf(spec);
  Face face;
  const auto microseconds = [](const std::chrono::steady_clock::time_point since) {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count());
  };
  auto started = std::chrono::steady_clock::now();
  ASSERT_EQ(FaceStatus::Ok, parse(intact, &face));  // every offset read once
  const long long plainUs = microseconds(started);
  ASSERT_EQ(65535, face.cff.localSubrs.count);

  std::vector<uint8_t> zeros = intact;
  for (uint32_t i = 1; i < 65535; ++i) setIndexOffset(zeros, face.cff.localSubrs, i, 0);
  started = std::chrono::steady_clock::now();
  EXPECT_EQ(FaceStatus::Malformed, parse(zeros, &face));
  std::printf("65,535 plain subroutine offsets read in %lld us; 65,535 with zeros refused in %lld us\n", plainUs,
              microseconds(started));

  ASSERT_EQ(FaceStatus::Ok, parse(intact, &face));
  const CffIndex global = face.cff.globalSubrs;  // offsets 1, 2, 3, 4
  std::vector<uint8_t> firstNotOne = intact;
  setIndexOffset(firstNotOne, global, 0, 2);
  EXPECT_EQ(FaceStatus::Malformed, parse(firstNotOne, &face));
  std::vector<uint8_t> descending = intact;
  setIndexOffset(descending, global, 2, 2);
  setIndexOffset(descending, global, 1, 3);
  EXPECT_EQ(FaceStatus::Malformed, parse(descending, &face));
  std::vector<uint8_t> empties = intact;  // offsets 1, 1, 1, 4: two empty subroutines, then one of three bytes
  setIndexOffset(empties, global, 1, 1);
  setIndexOffset(empties, global, 2, 1);
  EXPECT_EQ(FaceStatus::Ok, parse(empties, &face));

  // A font DICT's local subroutines, in a CID-keyed font whose glyphs all use FD 1 (FD 0 has none).
  CffSpec cid;
  cid.cid = true;
  cid.fds = {cffFontDict(""), cffFontDict("", {Bytes{11}, Bytes{11}})};
  cid.fdSelect = {0, 1, 1, 1};  // format 0
  const std::vector<uint8_t> cidIntact = buildOtf(cid);
  FontBytes bytes(cidIntact.data(), static_cast<uint32_t>(cidIntact.size()));
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  CffGlyphSource source;
  ASSERT_EQ(Status::Ok, cffGlyphSource(bytes, face, 0, &source));
  ASSERT_EQ(2, source.localSubrs.count);
  std::vector<uint8_t> cidBroken = cidIntact;
  setIndexOffset(cidBroken, source.localSubrs, 1, 0);
  EXPECT_EQ(FaceStatus::Malformed, parse(cidBroken, &face));
}

namespace {

// A font file on a card that counts the bytes it is asked for, read as discovery reads it: through an SdRange over the
// whole file with no tables in place (FileParse's way), so every read but those the page cache answers is counted.
struct CountingCard {
  explicit CountingCard(const std::vector<uint8_t>& font) : bytes(font) {}
  const std::vector<uint8_t>& bytes;
  uint64_t read = 0;

  FaceStatus parse(Face* face) {
    const FileSource file{static_cast<uint32_t>(bytes.size()),
                          [](void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
                            auto& card = *static_cast<CountingCard*>(ctx);
                            if (offset > card.bytes.size() || len > card.bytes.size() - offset) return false;
                            std::copy_n(card.bytes.data() + offset, len, static_cast<uint8_t*>(dst));
                            card.read += len;
                            return true;
                          },
                          this};
    SdPageCache cache;
    const SdRange range{file, 0, 0, file.size, &cache};
    FontBytes sd(nullptr, 0, &range);
    return parseFace(sd, face);
  }
};

constexpr uint32_t kSubrsIndexBytes = 65535 * 4;  // the offsets of a full local Subrs INDEX

}  // namespace

TEST(TtfCff, ManyFontDictsSharingOneSubrsIndexAreCheckedOnce) {
  // 256 font DICTs whose Private DICTs all point at one 65,535-entry Subrs INDEX: the check reads its offsets once, not
  // once per font DICT (256 x 262 KB from a 300 KB font, a minute of card reads at every discovery).
  CffSpec spec;
  spec.cid = true;
  spec.fds.assign(256, CffFontDict{});
  spec.fds[0] = cffFontDict("", std::vector<Bytes>(65535));
  for (size_t i = 1; i < 256; ++i) spec.fds[i].sharesSubrsOf = 0;
  const std::vector<uint8_t> font = buildOtf(spec);
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(font, &face));

  CountingCard card(font);
  ASSERT_EQ(FaceStatus::Ok, card.parse(&face));
  // The INDEX once, in 512-byte pages, and the small rest of the file (tables, the font DICTs and Private DICTs).
  EXPECT_LT(card.read, uint64_t{kSubrsIndexBytes} + 64 * 1024)
      << card.read << " bytes of a " << font.size() << " byte font";
}

TEST(TtfCff, FontDictsWithTooManySubroutinesInAllAreRefused) {
  // Distinct local Subrs INDEXes past 4 x 65,536 entries together: four full ones fit, a fifth is refused before its
  // offsets are read.
  const auto build = [](const size_t indexes) {
    CffSpec spec;
    spec.cid = true;
    for (size_t i = 0; i < indexes; ++i) spec.fds.push_back(cffFontDict("", std::vector<Bytes>(65535)));
    return buildOtf(spec);
  };
  Face face;
  const std::vector<uint8_t> four = build(4);
  EXPECT_EQ(FaceStatus::Ok, parse(four, &face)) << "four full INDEXes: 262,140 entries";
  const std::vector<uint8_t> five = build(5);
  CountingCard card(five);
  EXPECT_EQ(FaceStatus::Malformed, card.parse(&face));
  EXPECT_LT(card.read, 4 * uint64_t{kSubrsIndexBytes} + 64 * 1024)
      << card.read << " bytes read of a " << five.size() << " byte font";
}

TEST(TtfCff, AFontWithMoreDistinctIndexesThanRememberedStaysWithinTheBudget) {
  // 40 font DICTs with an INDEX of their own, then 200 sharing one of 1,000 entries: more distinct INDEXes than the
  // check remembers, so the shared one is checked again by every font DICT that has it, and the budget bounds that.
  CffSpec spec;
  spec.cid = true;
  for (size_t i = 0; i < 40; ++i) spec.fds.push_back(cffFontDict("", std::vector<Bytes>(3 + i)));
  spec.fds.push_back(cffFontDict("", std::vector<Bytes>(1000)));
  CffFontDict sharing;
  sharing.sharesSubrsOf = 40;
  spec.fds.insert(spec.fds.end(), 200, sharing);
  const std::vector<uint8_t> font = buildOtf(spec);
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parse(font, &face));

  CountingCard card(font);
  ASSERT_EQ(FaceStatus::Ok, card.parse(&face));
  EXPECT_LT(card.read, 4 * uint64_t{kSubrsIndexBytes} + 64 * 1024)
      << card.read << " bytes of a " << font.size() << " byte font";
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

// --- SD mode: the CFF table rebuilt in the style block, its charstring data read from the file ---

namespace {

// A font file served as the card serves one (ttf::FileSource), from memory.
FileSource memorySource(const std::vector<uint8_t>& file) {
  return {static_cast<uint32_t>(file.size()),
          [](void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
            const auto& bytes = *static_cast<const std::vector<uint8_t>*>(ctx);
            if (offset > bytes.size() || len > bytes.size() - offset) return false;
            std::copy_n(bytes.data() + offset, len, static_cast<uint8_t*>(dst));
            return true;
          },
          const_cast<std::vector<uint8_t>*>(&file)};
}

// `font`'s SD-mode block (its tables in place, the CFF table rebuilt last) with the charstring data read from the file
// draws every glyph at 14 pt as the whole file does, byte for byte. `drawn` counts the glyphs with ink.
void expectTheBlockDrawsAsTheFile(const std::vector<uint8_t>& font, const std::string& name, uint32_t* drawn) {
  *drawn = 0;
  const FileSource file = memorySource(font);
  SdBlock block;
  ASSERT_EQ(ttf::SdPlan::Ok, block.plan(file)) << name;
  // The block, read whole and in pieces of a prime size, which cut every part of it somewhere.
  FileSource blockSource = block.source();
  std::vector<uint8_t> tables(block.bytes());
  ASSERT_TRUE(blockSource.read(blockSource.ctx, 0, tables.data(), block.bytes())) << name;
  std::vector<uint8_t> pieces(block.bytes());
  for (uint32_t at = 0; at < block.bytes(); at += 97) {
    const uint32_t len = std::min<uint32_t>(97, block.bytes() - at);
    ASSERT_TRUE(blockSource.read(blockSource.ctx, at, pieces.data() + at, len)) << name << " at " << at;
  }
  EXPECT_EQ(tables, pieces) << name;
  EXPECT_FALSE(blockSource.read(blockSource.ctx, block.bytes() - 1, pieces.data(), 2)) << name;

  // The block's tables in place and the charstring data on the card, as an SD-mode style reads them.
  const glyphstore::SdOutlines outlines = block.outlines();
  EXPECT_EQ(block.bytes(), outlines.fontOffset) << name;
  SdPageCache cache;
  const SdRange range{file, outlines.fontOffset, outlines.fileOffset, outlines.length, &cache};
  FontBytes sd(tables.data(), block.bytes(), &range);
  FontBytes whole(font.data(), static_cast<uint32_t>(font.size()));
  Face sdFace;
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(whole, &face)) << name;
  ASSERT_EQ(FaceStatus::Ok, parseFace(sd, &sdFace)) << name;
  // The CFF table ends the block: only the CharStrings INDEX's data lies past it, and that is the file's.
  EXPECT_EQ(block.bytes(), sdFace.cff.charStrings.dataStart) << name;
  EXPECT_EQ(sd.size(), sdFace.cff.table + sdFace.cff.tableLength) << name;
  EXPECT_EQ(face.cff.charStrings.dataStart, outlines.fileOffset) << name;
  EXPECT_EQ(face.cff.charStrings.end - face.cff.charStrings.dataStart, outlines.length) << name;
  ASSERT_EQ(face.numGlyphs, sdFace.numGlyphs) << name;

  const uint32_t ppem = ppemFromPoints(14);
  Rasterizer fromFile;
  Rasterizer fromBlock;
  ASSERT_TRUE(fromFile.allocate(planScratch(face, ppem, kPoolBytesFlash))) << name;
  ASSERT_TRUE(fromBlock.allocate(planScratch(sdFace, ppem, kPoolBytesSd))) << name;
  for (uint32_t g = 0; g < face.numGlyphs; ++g) {
    const std::string glyph = name + " glyph " + std::to_string(g);
    whole.clearErrors();
    sd.clearErrors();
    EpdGlyph want{};
    EpdGlyph got{};
    const Status wanted = fromFile.render(whole, face, static_cast<uint16_t>(g), ppem, &want);
    ASSERT_EQ(wanted, fromBlock.render(sd, sdFace, static_cast<uint16_t>(g), ppem, &got)) << glyph;
    if (wanted != Status::Ok) continue;
    ASSERT_EQ(want.width, got.width) << glyph;
    ASSERT_EQ(want.height, got.height) << glyph;
    ASSERT_EQ(want.left, got.left) << glyph;
    ASSERT_EQ(want.top, got.top) << glyph;
    ASSERT_EQ(want.advanceX, got.advanceX) << glyph;
    ASSERT_EQ(want.dataLength, got.dataLength) << glyph;
    ASSERT_EQ(0, std::memcmp(fromFile.output(), fromBlock.output(), want.dataLength)) << glyph;
    *drawn += want.dataLength > 0 ? 1 : 0;
  }
  EXPECT_GT(cache.misses(), 0u) << name;  // the charstrings came from the file
}

// A charstring or subroutine drawing a `width` by 100 unit box from (50, 50), then ending with `op`.
Bytes box(const int32_t width, const uint8_t op) {
  Bytes out;
  putType2Op(out, {50, 50}, 21);             // rmoveto
  putType2Op(out, {width, 100, -width}, 6);  // hlineto, then alternating vlineto, hlineto
  out.push_back(op);
  return out;
}

// A charstring calling subroutine 0 (bias 107) with `op`, callsubr or callgsubr.
Bytes callingFirst(const uint8_t op) {
  Bytes out;
  putType2Op(out, {-107}, op);
  out.push_back(14);  // endchar
  return out;
}

}  // namespace

TEST(TtfCff, TheRebuiltCffDrawsAsTheOriginal) {
  for (const std::string& name : cffTestFonts()) {
    const std::vector<uint8_t> font = readCffTestFont(name);
    ASSERT_FALSE(font.empty()) << name;
    uint32_t drawn = 0;
    expectTheBlockDrawsAsTheFile(font, name, &drawn);
    EXPECT_GT(drawn, 40u) << name;
  }
}

TEST(TtfCff, SubroutinesLaidOutAfterEveryPrivateDictKeepTheirPlaces) {
  // As makeotf lays out a CID-keyed font (Noto CJK): every Private DICT, then every Subrs INDEX, each offset from its
  // own Private DICT's start across the others. Each glyph draws its FD's own subroutine.
  CffSpec spec;
  spec.cid = true;
  spec.subrsAfterPrivates = true;
  spec.charStrings = {callingFirst(10), callingFirst(10), callingFirst(10)};
  spec.fds = {cffFontDict("", {box(100, 11)}), cffFontDict("", {box(250, 11), box(400, 11)}),
              cffFontDict("", {box(600, 11)})};
  spec.fdSelect = {3, 0, 3, 0, 0, 0, 0, 1, 1, 0, 2, 2, 0, 3};  // glyph g in FD g
  const Bytes font = buildOtf(spec);
  uint32_t drawn = 0;
  expectTheBlockDrawsAsTheFile(font, "subroutines after the Private DICTs", &drawn);
  EXPECT_EQ(3u, drawn);
}

TEST(TtfCff, ANameKeyedFontKeepsItsEncodingAndTheLastOfARepeatedOffset) {
  // A name-keyed font with an Encoding of its own, the predefined ISOAdobe charset (not moved), local and global
  // subroutines, and a CharStrings offset set twice: the first is left as it is, the last moves.
  CffSpec spec;
  spec.charStrings = {callingFirst(10), callingFirst(29), box(300, 14)};
  spec.fds = {cffFontDict("", {box(200, 11)})};
  spec.globalSubrs = {box(500, 11)};
  spec.charsetOffset = 0;
  spec.encoding = {0, 2, 1, 2};  // format 0: codes 1 and 2
  putCffInt(spec.topPad, 3);
  putCffOp(spec.topPad, 17);  // CharStrings, overridden
  const Bytes font = buildOtf(spec);
  uint32_t drawn = 0;
  expectTheBlockDrawsAsTheFile(font, "name-keyed", &drawn);
  EXPECT_EQ(3u, drawn);
}
