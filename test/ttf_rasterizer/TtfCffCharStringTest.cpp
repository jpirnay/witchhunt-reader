// The Type 2 charstring interpreter (TtfCffCharString): FreeType's cf2 engine as it runs unhinted, leaving the outline
// in font units. FreeType itself (FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING, which applies no font matrix to the fonts
// compared here) is the oracle: every synthetic charstring below is drawn by both, and every glyph of the CFF test
// fonts whose matrix is identity.
#include <ft2build.h>
#include FT_FREETYPE_H
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "CffBuilder.h"
#include "TtfCff.h"
#include "TtfCffCharString.h"
#include "TtfFace.h"
#include "TtfFixed.h"
#include "TtfRasterizer.h"
#include "TtfTestUtil.h"

// FreeType's square root (ftcalc.c), internal to FreeType but linked into the oracle.
extern "C" FT_UInt32 FT_SqrtFixed(FT_UInt32 x);

using namespace ttf;
using namespace ttf_test;

namespace {

// --- Charstrings ---

struct Op {
  uint16_t code;  ///< one byte, or 0x0C00 | x for the escaped operator 12 x
};
struct Fx {
  int32_t value;  ///< a 16.16 number, written with the 255 prefix
};
struct Raw {
  std::vector<uint8_t> bytes;  ///< bytes as they are (hint mask bytes)
};

namespace op {
constexpr Op hstem{1}, vstem{3}, vmoveto{4}, rlineto{5}, hlineto{6}, vlineto{7}, rrcurveto{8}, callsubr{10}, ret{11},
    endchar{14}, hstemhm{18}, hintmask{19}, cntrmask{20}, rmoveto{21}, hmoveto{22}, vstemhm{23}, rcurveline{24},
    rlinecurve{25}, vvcurveto{26}, hhcurveto{27}, callgsubr{29}, vhcurveto{30}, hvcurveto{31};
constexpr Op and_{0x0C03}, or_{0x0C04}, not_{0x0C05}, abs{0x0C09}, add{0x0C0A}, sub{0x0C0B}, div{0x0C0C}, neg{0x0C0E},
    eq{0x0C0F}, drop{0x0C12}, put{0x0C14}, get{0x0C15}, ifelse{0x0C16}, random{0x0C17}, mul{0x0C18}, sqrt{0x0C1A},
    dup{0x0C1B}, exch{0x0C1C}, index{0x0C1D}, roll{0x0C1E}, hflex{0x0C22}, flex{0x0C23}, hflex1{0x0C24}, flex1{0x0C25};
}  // namespace op

// A Type 2 integer in its shortest form.
void put(Bytes& out, const int32_t v) {
  if (v >= -107 && v <= 107) {
    out.push_back(static_cast<uint8_t>(v + 139));
  } else if (v >= 108 && v <= 1131) {
    out.push_back(static_cast<uint8_t>(247 + (v - 108) / 256));
    out.push_back(static_cast<uint8_t>((v - 108) % 256));
  } else if (v >= -1131 && v <= -108) {
    out.push_back(static_cast<uint8_t>(251 + (-v - 108) / 256));
    out.push_back(static_cast<uint8_t>((-v - 108) % 256));
  } else {
    out.push_back(28);
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
  }
}
void put(Bytes& out, const Fx f) {
  out.push_back(255);
  put32(out, static_cast<uint32_t>(f.value));
}
void put(Bytes& out, const Op o) {
  if (o.code >= 0x0C00) out.push_back(12);
  out.push_back(static_cast<uint8_t>(o.code));
}
void put(Bytes& out, const Raw& r) { out.insert(out.end(), r.bytes.begin(), r.bytes.end()); }
void put(Bytes& out, const Bytes& b) { out.insert(out.end(), b.begin(), b.end()); }

template <typename... T>
Bytes cs(const T&... items) {
  Bytes out;
  (put(out, items), ...);
  return out;
}

// `count` copies of `item`.
Bytes repeat(const Bytes& item, const size_t count) {
  Bytes out;
  for (size_t i = 0; i < count; ++i) out.insert(out.end(), item.begin(), item.end());
  return out;
}

// A name-keyed OpenType font whose glyph 1 runs `program` (glyphs 0 and 2 are bare endchars), with these local
// subroutines (the Top DICT's Private DICT's) and global ones. `topPad`: entries the Top DICT starts with.
std::vector<uint8_t> fontWith(const Bytes& program, const std::vector<Bytes>& locals = {},
                              const std::vector<Bytes>& globals = {}, const Bytes& topPad = {}) {
  CffSpec spec;
  spec.charStrings = {{14}, program, {14}};
  spec.fds = {cffFontDict("", locals)};
  spec.globalSubrs = globals;
  spec.topPad = topPad;
  return buildOtf(spec);
}

// --- Outlines ---

struct OutlinePoint {
  long x = 0;
  long y = 0;
  int tag = 0;  ///< 1 on the curve (FT_CURVE_TAG_ON), 2 a cubic control point (FT_CURVE_TAG_CUBIC)
  bool operator==(const OutlinePoint& o) const { return x == o.x && y == o.y && tag == o.tag; }
};

constexpr int kOn = 1;
constexpr int kCubic = 2;

// A glyph's outline, or that it could not be drawn: `drawn` false (FreeType's error, or the engine's status).
struct Shape {
  bool drawn = false;
  Status status = Status::Ok;  ///< the engine's
  std::vector<OutlinePoint> points;
  std::vector<int> ends;
  bool operator==(const Shape& o) const { return drawn == o.drawn && points == o.points && ends == o.ends; }
};

std::ostream& operator<<(std::ostream& out, const Shape& shape) {
  if (!shape.drawn) return out << "(not drawn: " << statusName(shape.status) << ")";
  out << shape.points.size() << " points, ends {";
  for (size_t i = 0; i < shape.ends.size(); ++i) out << (i ? " " : "") << shape.ends[i];
  out << "}:";
  for (const OutlinePoint& p : shape.points) out << " (" << p.x << "," << p.y << (p.tag == kOn ? "" : " c") << ")";
  return out;
}

Shape drawnShape(const std::vector<OutlinePoint>& points, const std::vector<int>& ends) {
  Shape shape;
  shape.drawn = true;
  shape.points = points;
  shape.ends = ends;
  return shape;
}

// The engine over a whole font: parseFace once, then any glyph, into arrays far larger than any test glyph needs.
class Engine {
 public:
  explicit Engine(const std::vector<uint8_t>& data, const uint16_t maxPoints = 4096, const uint16_t maxContours = 1024)
      : data_(data),
        bytes_(data_.data(), static_cast<uint32_t>(data_.size())),
        points_(maxPoints),
        tags_(maxPoints),
        contours_(maxContours),
        decoder_(new CffDecoder) {
    ok_ = parseFace(bytes_, &face_) == FaceStatus::Ok && face_.outlines == Face::Outlines::Cff;
  }

  bool ok() const { return ok_; }
  const Face& face() const { return face_; }
  FontBytes& bytes() { return bytes_; }

  // The glyph's source, whether its matrix and scale leave the outline as cf2 draws it.
  bool untransformed(const uint16_t glyph) {
    bytes_.clearErrors();
    CffGlyphSource source;
    if (cffGlyphSource(bytes_, face_, glyph, &source) != Status::Ok) return true;
    return source.matrix.identity() && source.matrix.dx == 0 && source.matrix.dy == 0 &&
           source.scaleNum == source.scaleDen;
  }

  Shape glyph(const uint16_t glyph) {
    Shape shape;
    bytes_.clearErrors();
    CffGlyphSource source;
    shape.status = cffGlyphSource(bytes_, face_, glyph, &source);
    if (shape.status != Status::Ok) return shape;
    CffOutline out{points_.data(), tags_.data(), contours_.data(), static_cast<uint16_t>(points_.size()),
                   static_cast<uint16_t>(contours_.size())};
    shape.status = runCharString(bytes_, face_, source, decoder_.get(), &out);
    if (shape.status != Status::Ok) return shape;
    shape.drawn = true;
    for (uint32_t i = 0; i < out.numPoints; ++i) shape.points.push_back({points_[i].x, points_[i].y, tags_[i]});
    for (uint32_t i = 0; i < out.numContours; ++i) shape.ends.push_back(contours_[i]);
    return shape;
  }

 private:
  std::vector<uint8_t> data_;
  FontBytes bytes_;
  Face face_;
  bool ok_ = false;
  std::vector<FT_Vector> points_;
  std::vector<unsigned char> tags_;
  std::vector<unsigned short> contours_;
  std::unique_ptr<CffDecoder> decoder_;
};

FT_Library library() {
  static FT_Library lib = [] {
    FT_Library l = nullptr;
    FT_Init_FreeType(&l);
    return l;
  }();
  return lib;
}

// FreeType's unscaled, unhinted outline of a glyph.
class Oracle {
 public:
  explicit Oracle(const std::vector<uint8_t>& data) : data_(data) {
    if (FT_New_Memory_Face(library(), data_.data(), static_cast<FT_Long>(data_.size()), 0, &face_) != 0) {
      face_ = nullptr;
    }
  }
  ~Oracle() {
    if (face_) FT_Done_Face(face_);
  }
  Oracle(const Oracle&) = delete;
  Oracle& operator=(const Oracle&) = delete;

  bool ok() const { return face_ != nullptr; }

  Shape glyph(const uint16_t glyph) const {
    Shape shape;
    if (FT_Load_Glyph(face_, glyph, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0) return shape;
    const FT_Outline& outline = face_->glyph->outline;
    shape.drawn = true;
    for (int i = 0; i < outline.n_points; ++i) {
      shape.points.push_back({outline.points[i].x, outline.points[i].y, outline.tags[i]});
    }
    for (int i = 0; i < outline.n_contours; ++i) shape.ends.push_back(outline.contours[i]);
    return shape;
  }

 private:
  std::vector<uint8_t> data_;
  FT_Face face_ = nullptr;
};

// The engine's outline of glyph 1 of `font`, after checking FreeType draws the same (or fails where it fails).
Shape drawAsFreeType(const std::vector<uint8_t>& font, const uint16_t glyph = 1) {
  Engine engine(font);
  EXPECT_TRUE(engine.ok());
  Oracle oracle(font);
  EXPECT_TRUE(oracle.ok());
  if (!engine.ok() || !oracle.ok()) return Shape{};
  const Shape ours = engine.glyph(glyph);
  const Shape theirs = oracle.glyph(glyph);
  EXPECT_EQ(theirs, ours) << "FreeType: " << theirs << "\nengine:   " << ours;
  return ours;
}

std::vector<uint8_t> readCffTestFont(const std::string& name) {
  return name.rfind("OpenDyslexic", 0) == 0 ? readFont(name) : readFixture(name);
}

uint16_t glyphFor(Engine& engine, const uint32_t codepoint) {
  return glyphIndex(engine.bytes(), engine.face(), codepoint);
}

}  // namespace

// --- Paths ---

TEST(TtfCffCharString, MovesAndLinesMakeAClosedContour) {
  // The closing line back to the start draws a point the builder removes again (ps_builder_close_contour).
  const Shape shape =
      drawAsFreeType(fontWith(cs(10, 20, op::rmoveto, 100, 0, 0, 100, -100, 0, op::rlineto, op::endchar)));
  EXPECT_EQ(drawnShape({{10, 20, kOn}, {110, 20, kOn}, {110, 120, kOn}, {10, 120, kOn}}, {3}), shape);
}

TEST(TtfCffCharString, ZeroLengthLinesAreDropped) {
  // A line of no length in character space (0 0), and one too short to move in device space, 16/65536 of a unit
  // (pshints.c: cf2_glyphpath_lineTo, cf2_glyphpath_pushPrevElem).
  const Shape shape = drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 0, 0, op::rlineto, 200, 0, op::rlineto, Fx{16},
                                                 0, op::rlineto, 0, 200, op::rlineto, op::endchar)));
  EXPECT_EQ(drawnShape({{100, 100, kOn}, {300, 100, kOn}, {300, 300, kOn}}, {2}), shape);
}

TEST(TtfCffCharString, ASinglePointContourIsDropped) {
  // A line of 1/1024 unit moves in device space but not in font units: its contour is two equal points. The next
  // path's move closes that contour a second time (FreeType closes it at every move it passes on, and the middle path
  // passes one on but draws nothing), which removes the second point; a contour of one point is dropped.
  const Shape shape =
      drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, Fx{64}, 0, op::rlineto,  // two points
                                 300, 0, op::rmoveto, Fx{16}, 0, op::rlineto,    // nothing drawn
                                 0, 300, op::rmoveto, 50, 0, op::rlineto, 0, 50, op::rlineto, op::endchar)));
  EXPECT_EQ(drawnShape({{400, 400, kOn}, {450, 400, kOn}, {450, 450, kOn}}, {2}), shape);
}

TEST(TtfCffCharString, AContourClosedTwiceLosesAnotherEqualPoint) {
  // As above with a triangle whose last corner is 1/1024 unit from its first: the first close removes the closing
  // point, the second the corner before it.
  const Shape shape =
      drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 200, 0, 0, 200, Fx{-200 * 65536 + 64}, -200,
                                 op::rlineto,                                  // S, T, U, S', S
                                 300, 0, op::rmoveto, Fx{16}, 0, op::rlineto,  // nothing drawn
                                 0, 300, op::rmoveto, 50, 0, op::rlineto, 0, 50, op::rlineto, op::endchar)));
  EXPECT_EQ(
      drawnShape({{100, 100, kOn}, {300, 100, kOn}, {300, 300, kOn}, {400, 400, kOn}, {450, 400, kOn}, {450, 450, kOn}},
                 {2, 5}),
      shape);
}

TEST(TtfCffCharString, AZeroLengthLineAfterAHintMaskIsPassedOn) {
  // After a hint mask, a line of no length still starts its path (cf2_glyphpath_lineTo's newHintMap): the move it
  // passes on closes the contour before a second time.
  const Shape shape =
      drawAsFreeType(fontWith(cs(0, 50, op::hstemhm, 100, 100, op::rmoveto, 200, 0, 0, 200, Fx{-200 * 65536 + 64}, -200,
                                 op::rlineto, 300, 0, op::rmoveto, op::hintmask, Raw{{0x80}}, 0, 0, op::rlineto, 0, 300,
                                 op::rmoveto, 50, 0, op::rlineto, 0, 50, op::rlineto, op::endchar)));
  EXPECT_EQ(3, shape.ends.empty() ? 0 : shape.ends[0] + 1);
}

TEST(TtfCffCharString, CurvesKeepBothControlPointsAsCubic) {
  const Shape shape =
      drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 100, 200, 150, 100, 100, -50, op::rrcurveto, op::endchar)));
  EXPECT_EQ(drawnShape({{100, 100, kOn}, {200, 300, kCubic}, {350, 400, kCubic}, {450, 350, kOn}}, {3}), shape);
}

TEST(TtfCffCharString, HhAndVvCurvesWithAnOddCountDropTwoBottomArguments) {
  // A count not of the form 4n or 4n+1 (8n, 8n+1, 8n+4 or 8n+5 for hv and vh) loses its two bottom arguments
  // (psintrp.c: count &= ~2 for vvcurveto, hhcurveto, vhcurveto and hvcurveto).
  EXPECT_EQ(drawnShape({{100, 100, kOn}, {150, 100, kCubic}, {250, 250, kCubic}, {350, 250, kOn}}, {3}),
            drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 7, 9, 50, 100, 150, 100, op::hhcurveto, op::endchar))));
  EXPECT_EQ(
      drawnShape({{100, 100, kOn}, {150, 130, kCubic}, {250, 280, kCubic}, {350, 280, kOn}}, {3}),
      drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 7, 9, 30, 50, 100, 150, 100, op::hhcurveto, op::endchar))));
  EXPECT_EQ(drawnShape({{100, 100, kOn}, {100, 150, kCubic}, {200, 300, kCubic}, {200, 400, kOn}}, {3}),
            drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 7, 9, 50, 100, 150, 100, op::vvcurveto, op::endchar))));
  EXPECT_EQ(
      drawnShape({{100, 100, kOn}, {130, 150, kCubic}, {230, 300, kCubic}, {230, 400, kOn}}, {3}),
      drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 7, 9, 30, 50, 100, 150, 100, op::vvcurveto, op::endchar))));
  EXPECT_EQ(drawnShape({{100, 100, kOn}, {150, 100, kCubic}, {250, 250, kCubic}, {250, 350, kOn}}, {3}),
            drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 7, 9, 50, 100, 150, 100, op::hvcurveto, op::endchar))));
  drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 7, 9, 50, 100, 150, 100, 40, op::vhcurveto, op::endchar)));
  // Three arguments leave none to draw with; one is too few.
  EXPECT_EQ(drawnShape({}, {}),
            drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 7, 9, 50, op::hhcurveto, op::endchar))));
  EXPECT_FALSE(drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 50, op::vvcurveto, op::endchar))).drawn);
}

TEST(TtfCffCharString, Flex1PutsTheLastDeltaOnTheLargerAxis) {
  // The five deltas travel further in x (500 > 80): the last is dx6, and the curve ends on the starting y.
  EXPECT_EQ(drawnShape({{100, 300, kOn},
                        {200, 330, kCubic},
                        {300, 370, kCubic},
                        {400, 400, kOn},
                        {500, 400, kCubic},
                        {600, 380, kCubic},
                        {700, 300, kOn}},
                       {6}),
            drawAsFreeType(fontWith(
                cs(100, 300, op::rmoveto, 100, 30, 100, 40, 100, 30, 100, 0, 100, -20, 100, op::flex1, op::endchar))));
  // Further in y: dy6, ending on the starting x.
  EXPECT_EQ(drawnShape({{100, 300, kOn},
                        {130, 400, kCubic},
                        {170, 500, kCubic},
                        {200, 600, kOn},
                        {200, 700, kCubic},
                        {180, 800, kCubic},
                        {100, 900, kOn}},
                       {6}),
            drawAsFreeType(fontWith(
                cs(100, 300, op::rmoveto, 30, 100, 40, 100, 30, 100, 0, 100, -20, 100, 100, op::flex1, op::endchar))));
  // Equally far: x must be strictly further, so the last is dy6.
  EXPECT_EQ(drawnShape({{100, 300, kOn},
                        {150, 350, kCubic},
                        {200, 400, kCubic},
                        {250, 450, kOn},
                        {300, 500, kCubic},
                        {350, 550, kCubic},
                        {100, 580, kOn}},
                       {6}),
            drawAsFreeType(fontWith(
                cs(100, 300, op::rmoveto, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 30, op::flex1, op::endchar))));
}

TEST(TtfCffCharString, HflexEndsOnTheStartingY) {
  EXPECT_EQ(
      drawnShape({{100, 300, kOn},
                  {200, 300, kCubic},
                  {300, 340, kCubic},
                  {400, 340, kOn},
                  {500, 340, kCubic},
                  {600, 300, kCubic},
                  {700, 300, kOn}},
                 {6}),
      drawAsFreeType(fontWith(cs(100, 300, op::rmoveto, 100, 100, 40, 100, 100, 100, 100, op::hflex, op::endchar))));
  drawAsFreeType(
      fontWith(cs(100, 300, op::rmoveto, 100, 30, 100, 40, 100, 100, 100, -30, 100, op::hflex1, op::endchar)));
  // flex reads twelve deltas and leaves the depth argument.
  drawAsFreeType(fontWith(
      cs(100, 300, op::rmoveto, 100, 30, 100, 40, 100, 30, 100, 0, 100, -30, 100, -40, 50, op::flex, op::endchar)));
}

TEST(TtfCffCharString, ThePathStartsAtTheOriginWithoutAMove) {
  // A charstring that draws before any move starts at (0, 0) (cf2_glyphpath_pushMove).
  EXPECT_EQ(drawnShape({{0, 0, kOn}, {200, 0, kOn}, {200, 200, kOn}}, {2}),
            drawAsFreeType(fontWith(cs(200, 0, op::rlineto, 0, 200, op::rlineto, op::endchar))));
}

TEST(TtfCffCharString, EndsAreFoundAsFreeTypeFindsThem) {
  // A charstring without endchar ends at its last byte, a subroutine without return at its own.
  EXPECT_EQ(drawnShape({{100, 100, kOn}, {300, 100, kOn}, {300, 300, kOn}}, {2}),
            drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, -107, op::callsubr),
                                    {cs(200, 0, op::rlineto, 0, 200, op::rlineto)})));
  // endchar in a subroutine ends the glyph.
  EXPECT_EQ(drawnShape({{100, 100, kOn}, {300, 100, kOn}, {300, 300, kOn}}, {2}),
            drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, -107, op::callsubr, 50, 50, op::rlineto),
                                    {cs(200, 0, op::rlineto, 0, 200, op::rlineto, op::endchar)})));
  // return in the glyph's own charstring is an error.
  EXPECT_FALSE(drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 200, 0, op::rlineto, op::ret))).drawn);
  // A number cut short at the end of a subroutine reads zeros (cf2_buf_readByte), not the next subroutine's bytes:
  // 28 1 is 256. So does a hint mask at the end of the glyph.
  EXPECT_EQ(drawnShape({{100, 100, kOn}, {300, 100, kOn}, {300, 356, kOn}}, {2}),
            drawAsFreeType(
                fontWith(cs(100, 100, op::rmoveto, 200, 0, op::rlineto, -107, op::callsubr, op::rlineto, op::endchar),
                         {cs(0, Raw{{28, 1}}), cs(100, 100, op::rlineto)})));
  drawAsFreeType(fontWith(cs(0, 50, 0, 50, op::hstemhm, 0, 50, 0, 50, 0, 50, 0, 50, 0, 50, op::vstemhm, 100, 100,
                             op::rmoveto, 200, 0, op::rlineto, op::hintmask)));
}

// --- Subroutines ---

TEST(TtfCffCharString, SubroutinesUseTheirBias) {
  // 107 below 1,240 subroutines, 1,131 below 33,900, 32,768 from there (cffdecode.c: cff_compute_bias); local and
  // global alike. Entry 0 draws a corner, every other entry nothing.
  const Bytes corner = cs(200, 0, op::rlineto, 0, 200, op::rlineto);
  const Shape expected = drawnShape({{100, 100, kOn}, {300, 100, kOn}, {300, 300, kOn}}, {2});
  struct Case {
    size_t count;
    int32_t call;
  };
  for (const Case c : {Case{1239, -107}, Case{1240, -1131}, Case{33899, -1131}, Case{33900, -32768}}) {
    std::vector<Bytes> subrs(c.count);
    subrs[0] = corner;
    EXPECT_EQ(expected, drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, c.call, op::callsubr, op::endchar), subrs)))
        << c.count << " local subroutines";
    EXPECT_EQ(expected,
              drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, c.call, op::callgsubr, op::endchar), {}, subrs)))
        << c.count << " global subroutines";
  }
  // Past either end of the INDEX.
  EXPECT_FALSE(drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, -108, op::callsubr, op::endchar), {corner})).drawn);
  EXPECT_FALSE(drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, -106, op::callsubr, op::endchar), {corner})).drawn);
  EXPECT_FALSE(drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, -107, op::callsubr, op::endchar))).drawn);
}

TEST(TtfCffCharString, CharstringType1HasNoSubroutineBias) {
  // A Top DICT CharstringType of 1: FreeType still runs the charstrings as Type 2 (cf2), with a bias of 0
  // (cff_compute_bias).
  Bytes topPad;
  putCffInt(topPad, 1);
  putCffOp(topPad, 0x106);
  const Bytes corner = cs(200, 0, op::rlineto, 0, 200, op::rlineto);
  const auto font = fontWith(cs(100, 100, op::rmoveto, 0, op::callsubr, 1, op::callgsubr, op::endchar), {corner},
                             {Bytes{}, cs(0, 100, op::rlineto)}, topPad);
  Engine engine(font);
  ASSERT_TRUE(engine.ok());
  EXPECT_EQ(1, engine.face().cff.charstringType);
  EXPECT_EQ(drawnShape({{100, 100, kOn}, {300, 100, kOn}, {300, 300, kOn}, {300, 400, kOn}}, {3}),
            drawAsFreeType(font));
  EXPECT_EQ(2, Engine(fontWith(cs(op::endchar))).face().cff.charstringType);
}

namespace {

// Local subroutines 0 to n-1, each calling the next `calls` times; the last draws `last`.
std::vector<Bytes> chain(const size_t n, const int calls, const Bytes& last) {
  std::vector<Bytes> subrs;
  for (size_t i = 0; i + 1 < n; ++i) {
    const Bytes call = cs(static_cast<int32_t>(i + 1) - 107, op::callsubr);
    subrs.push_back(repeat(call, calls));
  }
  subrs.push_back(last);
  return subrs;
}

}  // namespace

TEST(TtfCffCharString, SubroutineNestingStopsAtSixteen) {
  // The glyph and 16 levels of subroutines (CF2_MAX_SUBR); a 17th call fails.
  const Bytes glyph = cs(100, 100, op::rmoveto, -107, op::callsubr, 0, 200, op::rlineto, op::endchar);
  EXPECT_EQ(drawnShape({{100, 100, kOn}, {300, 100, kOn}, {300, 300, kOn}}, {2}),
            drawAsFreeType(fontWith(glyph, chain(16, 1, cs(200, 0, op::rlineto)))));
  const Shape deeper = drawAsFreeType(fontWith(glyph, chain(17, 1, cs(200, 0, op::rlineto))));
  EXPECT_FALSE(deeper.drawn);
  EXPECT_EQ(Status::Malformed, deeper.status);
}

TEST(TtfCffCharString, ARunawayCharStringStops) {
  // Review focus 1: 15 subroutines each calling the next twice, called twice by the glyph, 2^15 calls of the last and
  // about 197,000 operators and operands: FreeType runs them all (it stops at 20,000,000), the engine stops at
  // kCffMaxOperators. 13 levels run about 49,000, which the engine runs too.
  const Bytes glyph = cs(100, 100, op::rmoveto, -107, op::callsubr, -107, op::callsubr, op::endchar);
  EXPECT_EQ(drawnShape({}, {}), drawAsFreeType(fontWith(glyph, chain(13, 2, {}))));
  const auto font = fontWith(glyph, chain(15, 2, {}));
  Engine engine(font);
  ASSERT_TRUE(engine.ok());
  EXPECT_EQ(Status::Unsupported, engine.glyph(1).status);
  EXPECT_TRUE(Oracle(font).glyph(1).drawn);
}

// --- Hints ---

TEST(TtfCffCharString, HintMasksConsumeTheirBytes) {
  // A mask has a bit per stem, rounded up to whole bytes: 8 stems one, 9 stems two. Read one byte short or long, the
  // rlineto that follows would get three arguments and fail.
  const Bytes hstems4 = cs(0, 50, 100, 50, 100, 50, 100, 50);
  const Bytes vstems4 = cs(0, 50, 100, 50, 100, 50, 100, 50);
  const Shape expected = drawnShape({{100, 100, kOn}, {150, 100, kOn}, {150, 150, kOn}}, {2});
  EXPECT_EQ(expected,
            drawAsFreeType(fontWith(cs(hstems4, op::hstemhm, vstems4, op::vstemhm, 100, 100, op::rmoveto, op::hintmask,
                                       Raw{{0xFF}}, 50, 0, op::rlineto, 0, 50, op::rlineto, op::endchar))));
  EXPECT_EQ(expected, drawAsFreeType(fontWith(cs(hstems4, 100, 50, op::hstemhm, vstems4, op::vstemhm, 100, 100,
                                                 op::rmoveto, op::hintmask, Raw{{0xFF, 0x80}}, 50, 0, op::rlineto, 0,
                                                 50, op::rlineto, op::endchar))));
  // The vertical stems may be the arguments of the mask operator itself; cntrmask reads its bytes too.
  EXPECT_EQ(expected,
            drawAsFreeType(fontWith(cs(hstems4, 100, 50, op::hstemhm, vstems4, op::cntrmask, Raw{{0xA5, 0x80}}, 100,
                                       100, op::rmoveto, op::hintmask, Raw{{0x5A, 0x00}}, 50, 0, op::rlineto, 0, 50,
                                       op::rlineto, op::endchar))));
  // Once a mask is in force, a hintmask with arguments is ignored, its bytes unread: here they read as an endchar.
  EXPECT_EQ(drawnShape({{100, 100, kOn}, {150, 100, kOn}, {150, 150, kOn}}, {2}),
            drawAsFreeType(fontWith(cs(hstems4, op::hstemhm, 100, 100, op::rmoveto, 50, 0, op::rlineto, 0, 50,
                                       op::rlineto, 0, 50, op::hintmask, Raw{{14}}, 300, 300, op::rlineto))));
}

TEST(TtfCffCharString, MoreThanNinetySixStemsFail) {
  // Four hstemhm of 24 stems are 96 (CF2_MAX_HINTS); one more fails where FreeType counts them: a hint mask, or the
  // first move's hint map. A glyph that never draws or reads a mask builds no map and does not fail.
  const Bytes stems24 = repeat(cs(10, 20), 24);
  const Bytes stems96 = cs(stems24, op::hstemhm, stems24, op::hstemhm, stems24, op::hstemhm, stems24, op::hstemhm);
  const Bytes draw = cs(100, 100, op::rmoveto, 50, 0, op::rlineto, 0, 50, op::rlineto, op::endchar);
  EXPECT_TRUE(drawAsFreeType(fontWith(cs(stems96, draw))).drawn);
  const Shape tooMany = drawAsFreeType(fontWith(cs(stems96, 0, 50, op::vstemhm, draw)));
  EXPECT_FALSE(tooMany.drawn);
  EXPECT_EQ(Status::Malformed, tooMany.status);
  EXPECT_FALSE(
      drawAsFreeType(fontWith(cs(stems96, 0, 50, op::vstemhm, op::hintmask, Raw{repeat({0xFF}, 13)}, op::endchar)))
          .drawn);
  EXPECT_FALSE(
      drawAsFreeType(fontWith(cs(stems96, 0, 50, op::vstemhm, op::cntrmask, Raw{repeat({0xFF}, 13)}, op::endchar)))
          .drawn);
  EXPECT_EQ(drawnShape({}, {}), drawAsFreeType(fontWith(cs(stems96, 0, 50, op::vstemhm, op::endchar))));
  // Stems after the first map are ignored: they cannot fail it.
  EXPECT_TRUE(drawAsFreeType(fontWith(cs(stems96, 100, 100, op::rmoveto, 0, 50, op::vstemhm, 50, 0, op::rlineto, 0, 50,
                                         op::rlineto, op::endchar)))
                  .drawn);
}

// --- Operands ---

TEST(TtfCffCharString, TheWidthIsReadAndIgnored) {
  // A leading odd argument to the first stack-clearing operator is the width; it does not move the outline.
  const Shape expected = drawnShape({{10, 20, kOn}, {110, 20, kOn}, {110, 120, kOn}}, {2});
  const Bytes lines = cs(100, 0, op::rlineto, 0, 100, op::rlineto, op::endchar);
  EXPECT_EQ(expected, drawAsFreeType(fontWith(cs(10, 20, op::rmoveto, lines))));
  EXPECT_EQ(expected, drawAsFreeType(fontWith(cs(500, 10, 20, op::rmoveto, lines))));
  EXPECT_EQ(expected, drawAsFreeType(fontWith(cs(500, 0, 50, op::hstem, 10, 20, op::rmoveto, lines))));
  EXPECT_EQ(expected, drawAsFreeType(fontWith(cs(500, 0, 50, op::vstemhm, 10, 20, op::rmoveto, lines))));
  EXPECT_EQ(expected, drawAsFreeType(fontWith(cs(500, 10, op::hmoveto, 20, op::vmoveto, lines))));
  EXPECT_EQ(drawnShape({}, {}), drawAsFreeType(fontWith(cs(500, op::endchar))));
}

TEST(TtfCffCharString, RollRotatesTheBottomEntries) {
  // FreeType rolls the bottom `count` entries of the stack, not the top ones (psstack.c: cf2_stack_roll).
  EXPECT_EQ(
      drawnShape({{100, 0, kOn}, {400, 100, kOn}, {600, 500, kOn}}, {2}),
      drawAsFreeType(fontWith(cs(100, 0, op::rmoveto, 100, 200, 300, 400, 3, 1, op::roll, op::rlineto, op::endchar))));
  EXPECT_EQ(
      drawnShape({{100, 0, kOn}, {300, 300, kOn}, {400, 700, kOn}}, {2}),
      drawAsFreeType(fontWith(cs(100, 0, op::rmoveto, 100, 200, 300, 400, 3, -1, op::roll, op::rlineto, op::endchar))));
  // A count past the stack fails; one of 0 or 1 does nothing.
  EXPECT_FALSE(
      drawAsFreeType(fontWith(cs(100, 0, op::rmoveto, 100, 200, 3, 1, op::roll, op::rlineto, op::endchar))).drawn);
  drawAsFreeType(fontWith(cs(100, 0, op::rmoveto, 100, 200, 1, 1, op::roll, op::rlineto, op::endchar)));
}

TEST(TtfCffCharString, OperandsKeepTheirTypes) {
  // Numbers are integers, results of arithmetic are 16.16: an operator that takes an integer (callsubr, index, roll,
  // put, get) refuses a 16.16 one (cf2_stack_popInt), however whole.
  const Bytes corner = cs(200, 0, op::rlineto, 0, 200, op::rlineto);
  EXPECT_TRUE(drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, -107, op::callsubr, op::endchar), {corner})).drawn);
  EXPECT_FALSE(
      drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, -107, op::dup, op::drop, op::callsubr, op::endchar), {corner}))
          .drawn);
  EXPECT_FALSE(
      drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, Fx{-107 * 65536}, op::callsubr, op::endchar), {corner})).drawn);
  // and, or, not and eq give integers: index takes them. add gives a 16.16 value: index refuses it.
  EXPECT_EQ(drawnShape({{100, 100, kOn}, {150, 150, kOn}, {210, 210, kOn}, {280, 280, kOn}, {360, 360, kOn}}, {4}),
            drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 50, 1, 1, op::eq, op::index, op::rlineto, 60, 1, 1,
                                       op::and_, op::index, op::rlineto, 70, 0, 1, op::or_, op::index, op::rlineto, 80,
                                       0, op::not_, op::index, op::rlineto, op::endchar))));
  EXPECT_FALSE(
      drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 50, 0, 1, op::add, op::index, op::rlineto, op::endchar)))
          .drawn);
  // put and get store 32 values; an index past them is ignored, and get then pushes nothing.
  EXPECT_EQ(
      drawnShape({{100, 100, kOn}, {170, 190, kOn}, {200, 190, kOn}}, {2}),
      drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 70, 31, op::put, 90, 0, op::put, 5, 32, op::put, 31, op::get, 0,
                                 op::get, op::rlineto, 30, 32, op::get, 0, op::rlineto, op::endchar))));
  // ifelse picks the first pair's value when its first condition is not above the second.
  drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 50, 90, 2, 1, op::ifelse, 0, op::rlineto, 0, 50, 90, 1, 2,
                             op::ifelse, op::rlineto, op::endchar)));
  // index counts from the top; below 0 it repeats the top, past the bottom it takes the bottom.
  EXPECT_EQ(
      drawnShape({{100, 100, kOn}, {130, 130, kOn}, {170, 170, kOn}, {220, 220, kOn}}, {3}),
      drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 30, 0, op::index, op::rlineto, 40, -1, op::index, op::rlineto,
                                 50, 60, 9, op::index, op::exch, op::drop, op::rlineto, op::endchar))));
}

TEST(TtfCffCharString, ArithmeticMatchesFreeType) {
  // div is FT_DivFix, mul FT_MulFix, sqrt FT_SqrtFixed, each to the bit: FreeType's own functions are linked in.
  const int32_t values[] = {0,          1,         -1,          2,       3,          0x7FFF,     0x8000,     0xFFFF,
                            0x10000,    -0x10000,  0x18000,     3 << 16, -(7 << 16), 100 << 16,  12345678,   -98765,
                            0x7FFFFFFF, INT32_MIN, -0x7FFFFFFF, 1 << 30, -(1 << 30), 0x00010001, 46341 << 15};
  for (const int32_t a : values) {
    for (const int32_t b : values) {
      EXPECT_EQ(static_cast<int32_t>(FT_DivFix(a, b)), divFix(a, b)) << a << " div " << b;
      EXPECT_EQ(static_cast<int32_t>(FT_MulFix(a, b)), mulFix(a, b)) << a << " mul " << b;
    }
  }
  for (uint32_t v = 0; v < 0x30000; v += 7) EXPECT_EQ(FT_SqrtFixed(v), sqrtFixed(v)) << v;
  for (uint64_t v = 0x30000; v <= 0xFFFFFFFFu; v += 0x10003) {
    EXPECT_EQ(FT_SqrtFixed(static_cast<FT_UInt32>(v)), sqrtFixed(static_cast<uint32_t>(v))) << v;
  }
  for (const int32_t v : values) {
    EXPECT_EQ(FT_SqrtFixed(static_cast<FT_UInt32>(v)), sqrtFixed(static_cast<uint32_t>(v))) << v;
  }
  // The operators, through the outline: results multiplied up so their fractions show in font units.
  drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 1000, 3, op::div, 0, op::rlineto, 10, 3, op::div, 100, op::mul, 0,
                             op::rlineto, 2, op::sqrt, 1000, op::mul, 0, op::exch, op::rlineto, 0, op::sqrt, -5,
                             op::sqrt, op::add, 50, op::add, 0, op::rlineto, -300, op::abs, op::neg, 7, op::div, 0,
                             op::rlineto, 1, 0, op::div, 0, op::rlineto, op::endchar)));
}

TEST(TtfCffCharString, RandomIsUnsupported) {
  // Review focus 2: FreeType seeds random from memory addresses, so its outline cannot be reproduced.
  const auto font =
      fontWith(cs(100, 100, op::rmoveto, op::random, 100, op::mul, 0, op::rlineto, 0, 100, op::rlineto, op::endchar));
  Engine engine(font);
  ASSERT_TRUE(engine.ok());
  EXPECT_EQ(Status::Unsupported, engine.glyph(1).status);
  EXPECT_TRUE(Oracle(font).glyph(1).drawn);
}

TEST(TtfCffCharString, AStackOverflowFails) {
  // 48 operands fit (CF2_OPERAND_STACK_SIZE), 49 do not.
  const Bytes pairs24 = repeat(cs(10, 10), 24);
  EXPECT_TRUE(drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, pairs24, op::rlineto, op::endchar))).drawn);
  const Shape overflow = drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 1, pairs24, op::rlineto, op::endchar)));
  EXPECT_FALSE(overflow.drawn);
  EXPECT_EQ(Status::Malformed, overflow.status);
  // Too few operands fail as well.
  EXPECT_FALSE(drawAsFreeType(fontWith(cs(100, op::rmoveto, op::endchar))).drawn);
  EXPECT_FALSE(drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 10, 10, 10, op::rlineto, op::endchar))).drawn);
}

TEST(TtfCffCharString, OperatorsFreeTypeIgnoresClearTheStackOrNot) {
  // Reserved operators and Type 1 ones clear the stack; callothersubr and pop (12 16, 12 17) leave it.
  drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 50, 0, Raw{{0}}, 0, 50, op::rlineto, op::endchar)));
  drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 50, 0, Raw{{9}}, 0, 50, op::rlineto, op::endchar)));
  drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 50, 0, Raw{{12, 6}}, 0, 50, op::rlineto, op::endchar)));
  drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 50, 0, Raw{{12, 38}}, 0, 50, op::rlineto, op::endchar)));
  drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 50, 0, Raw{{12, 16}}, 0, 50, op::rlineto, op::endchar)));
  drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 50, 0, Raw{{12, 17}}, 0, 50, op::rlineto, op::endchar)));
  drawAsFreeType(fontWith(cs(100, 100, op::rmoveto, 50, 0, Raw{{16}}, 0, 50, op::rlineto, op::endchar)));
}

// --- seac ---

TEST(TtfCffCharString, SeacDrawsTheAccentThenTheBase) {
  // `20 50 65 193 endchar`: the accent (StandardEncoding 193, grave) moved by (20, 50), then the base (65, A).
  const std::vector<uint8_t> font = readFixture("CffOperators.otf");
  Engine engine(font);
  ASSERT_TRUE(engine.ok());
  const Shape base = engine.glyph(glyphFor(engine, 'A'));
  const Shape accent = engine.glyph(glyphFor(engine, 0x60));
  ASSERT_TRUE(base.drawn);
  ASSERT_TRUE(accent.drawn);
  Shape expected = accent;
  for (OutlinePoint& p : expected.points) {
    p.x += 20;
    p.y += 50;
  }
  for (const OutlinePoint& p : base.points) expected.points.push_back(p);
  for (const int end : base.ends) expected.ends.push_back(end + static_cast<int>(accent.points.size()));
  EXPECT_EQ(expected, drawAsFreeType(font, glyphFor(engine, 0xC0)));
}

TEST(TtfCffCharString, ASeacInACidKeyedFontDrawsItsComponents) {
  // Ruling 3: a CID-keyed font's charset is searched for seac's components (Task 2's patched glyph).
  std::vector<uint8_t> data = readFixture("NotoSansJP-CffFlat.otf");
  Engine before(data);
  ASSERT_TRUE(before.ok());
  CffGlyphSource source;
  const uint16_t patched = 200;
  ASSERT_EQ(Status::Ok, cffGlyphSource(before.bytes(), before.face(), patched, &source));
  ASSERT_GE(source.length, 5u);
  const uint8_t seac[] = {139 + 30, 139 - 20, 139 + 'A', 139 + 'B', 14};
  std::copy(seac, seac + 5, data.begin() + source.offset);
  const Shape shape = drawAsFreeType(data, patched);
  EXPECT_TRUE(shape.drawn);
  EXPECT_GT(shape.ends.size(), 1u);
}

TEST(TtfCffCharString, ASeacFailsWhereFreeTypeFails) {
  // A component the charset does not name ('A' is SID 34; the font has three glyphs), and a component that is itself
  // a seac (glyph 0's SID, 0, is StandardEncoding's code 0).
  EXPECT_FALSE(drawAsFreeType(fontWith(cs(0, 0, 'A', 0, op::endchar))).drawn);
  EXPECT_FALSE(drawAsFreeType(fontWith(cs(0, 0, 0, 'A', op::endchar))).drawn);
  CffSpec spec;
  spec.charStrings = {cs(0, 0, 0, 0, op::endchar), cs(10, 10, 0, 0, op::endchar), {14}};
  const Shape nested = drawAsFreeType(buildOtf(spec));
  EXPECT_FALSE(nested.drawn);
  EXPECT_EQ(Status::Malformed, nested.status);
  // Components of nothing draw nothing; a width before the four arguments is read as such.
  spec.charStrings = {{14}, cs(10, 10, 0, 0, op::endchar), cs(300, 10, 10, 0, 0, op::endchar)};
  EXPECT_EQ(drawnShape({}, {}), drawAsFreeType(buildOtf(spec)));
  EXPECT_EQ(drawnShape({}, {}), drawAsFreeType(buildOtf(spec), 2));
}

// --- Whole fonts ---

TEST(TtfCffCharString, EveryGlyphOfTheOperatorFontMatchesFreeTypeUnscaled) {
  const std::vector<uint8_t> font = readFixture("CffOperators.otf");
  Engine engine(font);
  ASSERT_TRUE(engine.ok());
  const Oracle oracle(font);
  ASSERT_TRUE(oracle.ok());
  for (uint32_t g = 0; g < engine.face().numGlyphs; ++g) {
    ASSERT_TRUE(engine.untransformed(static_cast<uint16_t>(g)));
    const Shape theirs = oracle.glyph(static_cast<uint16_t>(g));
    ASSERT_TRUE(theirs.drawn) << "glyph " << g;
    EXPECT_EQ(theirs, engine.glyph(static_cast<uint16_t>(g))) << "glyph " << g;
  }
}

TEST(TtfCffCharString, EveryGlyphOfEveryTestFontMatchesFreeTypeUnscaled) {
  // Every glyph whose font matrix is identity (all but NotoSansJP-CffSlanted's): the same points, tags and contour
  // ends as FreeType's, or Malformed / Unsupported where FreeType fails.
  for (const std::string& name : cffTestFonts()) {
    const std::vector<uint8_t> font = readCffTestFont(name);
    Engine engine(font);
    ASSERT_TRUE(engine.ok()) << name;
    const Oracle oracle(font);
    ASSERT_TRUE(oracle.ok()) << name;
    uint32_t compared = 0;
    uint32_t skipped = 0;
    uint32_t failed = 0;
    uint32_t points = 0;
    for (uint32_t g = 0; g < engine.face().numGlyphs; ++g) {
      if (!engine.untransformed(static_cast<uint16_t>(g))) {
        ++skipped;
        continue;
      }
      ++compared;
      const Shape theirs = oracle.glyph(static_cast<uint16_t>(g));
      const Shape ours = engine.glyph(static_cast<uint16_t>(g));
      if (!theirs.drawn) {
        ++failed;
        EXPECT_TRUE(ours.status == Status::Malformed || ours.status == Status::Unsupported)
            << name << " glyph " << g << ": " << statusName(ours.status);
        continue;
      }
      points += static_cast<uint32_t>(ours.points.size());
      EXPECT_EQ(theirs, ours) << name << " glyph " << g;
    }
    std::printf("%s: %u glyphs compared (%u points), %u FreeType fails, %u with a matrix skipped\n", name.c_str(),
                compared, points, failed, skipped);
    if (name != "NotoSansJP-CffSlanted.otf") {
      EXPECT_EQ(0u, skipped) << name;
    }
  }
}

namespace {

struct SweepCounts {
  uint32_t copies = 0;
  uint32_t drawn = 0;        ///< glyphs both drew, identically
  uint32_t refused = 0;      ///< glyphs FreeType failed, the engine Malformed or Unsupported
  uint32_t unsupported = 0;  ///< glyphs FreeType drew, the engine Unsupported (random, operator or point limits)
};

// Bytes of `intact`'s charstrings (every `step`th) and, when `subroutines`, of its subroutines, each set to values that
// read as operators or numbers, and the glyphs that may run them drawn by both: the glyph owning the byte and
// `seacGlyph` (which runs others as components), or every glyph for a subroutine's byte. An outline the engine draws is
// FreeType's; a glyph FreeType fails, the engine refuses.
void sweepCharStrings(const std::vector<uint8_t>& intact, const std::string& name, const uint32_t step,
                      const bool subroutines, const int32_t seacGlyph, SweepCounts* counts) {
  static const uint8_t kValues[] = {0, 10, 11, 12, 14, 19, 28, 29, 255};
  Engine original(intact);
  ASSERT_TRUE(original.ok()) << name;
  const Face& face = original.face();
  struct Range {
    uint32_t from;
    uint32_t to;
    int32_t glyph;  ///< -1: a subroutine
  };
  std::vector<Range> ranges;
  for (uint32_t g = 0; g < face.numGlyphs; ++g) {
    uint32_t offset = 0;
    uint32_t length = 0;
    ASSERT_TRUE(cffIndexItem(original.bytes(), face.cff.charStrings, g, &offset, &length));
    ranges.push_back({offset, offset + length, static_cast<int32_t>(g)});
  }
  if (subroutines) {
    ranges.push_back({face.cff.globalSubrs.dataStart, face.cff.globalSubrs.end, -1});
    ranges.push_back({face.cff.localSubrs.dataStart, face.cff.localSubrs.end, -1});
  }
  for (const Range& range : ranges) {
    for (uint32_t at = range.from; at < range.to; at += step) {
      for (const uint8_t value : kValues) {
        if (intact[at] == value) continue;
        std::vector<uint8_t> copy = intact;
        copy[at] = value;
        ++counts->copies;
        Engine engine(copy);
        ASSERT_TRUE(engine.ok());
        const Oracle oracle(copy);
        ASSERT_TRUE(oracle.ok());
        for (uint32_t g = 0; g < face.numGlyphs; ++g) {
          if (range.glyph >= 0 && static_cast<int32_t>(g) != range.glyph && static_cast<int32_t>(g) != seacGlyph) {
            continue;
          }
          const std::string where =
              name + " byte " + std::to_string(at) + " = " + std::to_string(value) + ", glyph " + std::to_string(g);
          const Shape theirs = oracle.glyph(static_cast<uint16_t>(g));
          const Shape ours = engine.glyph(static_cast<uint16_t>(g));
          if (!theirs.drawn) {
            ASSERT_TRUE(ours.status == Status::Malformed || ours.status == Status::Unsupported)
                << where << ": " << statusName(ours.status) << " " << ours;
            ++counts->refused;
          } else if (ours.status == Status::Unsupported) {
            ++counts->unsupported;
          } else {
            ASSERT_EQ(theirs, ours) << where;
            ++counts->drawn;
          }
        }
      }
    }
  }
}

}  // namespace

TEST(TtfCffCharString, ACorruptedCharStringDrawsAsFreeTypeOrNotAtAll) {
  // Review focus 1, against the oracle: every byte of CffOperators' charstrings and subroutines, and every 7th of
  // NotoSansJP-Cff's charstrings, set to operator bytes. No read outside the font (AddressSanitizer on CI, UBSan
  // here), no hang, and no outline FreeType would not draw.
  const auto started = std::chrono::steady_clock::now();
  SweepCounts counts;
  const std::vector<uint8_t> operators = readFixture("CffOperators.otf");
  Engine engine(operators);
  ASSERT_TRUE(engine.ok());
  sweepCharStrings(operators, "CffOperators.otf", 1, true, glyphFor(engine, 0xC0), &counts);
  sweepCharStrings(readFixture("NotoSansJP-Cff.otf"), "NotoSansJP-Cff.otf", 7, false, -1, &counts);
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  std::printf(
      "charstring sweep: %u copies; %u glyphs drawn as FreeType, %u refused as FreeType, %u Unsupported "
      "(FreeType drew), %lld ms\n",
      counts.copies, counts.drawn, counts.refused, counts.unsupported, static_cast<long long>(ms));
  EXPECT_GT(counts.drawn, 0u);
  EXPECT_GT(counts.refused, 0u);
}

// --- Limits ---

TEST(TtfCffCharString, MorePointsOrContoursThanTheOutlineHoldsAreUnsupported) {
  // Two triangles of three points, but the second's closing point is added before the builder removes it: 7 points.
  const auto font = fontWith(cs(100, 100, op::rmoveto, 200, 0, 0, 200, op::rlineto, 300, 300, op::rmoveto, 200, 0, 0,
                                200, op::rlineto, op::endchar));
  EXPECT_TRUE(Engine(font, 7, 2).glyph(1).drawn);
  EXPECT_EQ(Status::Unsupported, Engine(font, 6, 2).glyph(1).status);
  EXPECT_EQ(Status::Unsupported, Engine(font, 7, 1).glyph(1).status);
  const auto square = fontWith(cs(10, 20, op::rmoveto, 100, 0, 0, 100, -100, 0, op::rlineto, op::endchar));
  EXPECT_EQ(Status::Unsupported, Engine(square, 4, 1).glyph(1).status);
  EXPECT_TRUE(Engine(square, 5, 1).glyph(1).drawn);
}

TEST(TtfCffCharString, TheDecoderIsSmall) {
  static_assert(kCffDecoderBytes % 8 == 0, "the scratch block is planned in 8-byte steps");
  static_assert(kCffDecoderBytes >= sizeof(CffDecoder), "the planned size holds the decoder");
  std::printf("sizeof(CffDecoder) %u, planned %u\n", static_cast<unsigned>(sizeof(CffDecoder)),
              static_cast<unsigned>(kCffDecoderBytes));
  EXPECT_LE(kCffDecoderBytes, 1024u);
}
