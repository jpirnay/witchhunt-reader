// The integer loader against FreeType itself, unhinted (spec 9.3), over every glyph of every bundled TTF and of
// every CFF test font: every glyph must come out byte for byte. FreeType renders outlines flagged OVERLAP 4x4
// oversampled, which the engine does not (spec 11.6), so the reference clears that flag; the oversampled
// difference is measured on its own against the spec's 3% budget.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CffBuilder.h"
#include "FreeTypeReference.h"
#include "TtfBuilder.h"
#include "TtfCffCharString.h"
#include "TtfRasterizer.h"
#include "TtfTestUtil.h"
#include "ttf_raster_golden.h"

using namespace ttf;
using namespace ttf_test;

namespace {

constexpr uint32_t kPoints = 14;

int pixelAt(const uint8_t* bits, const int p) { return (bits[p >> 2] >> (6 - 2 * (p & 3))) & 3; }

std::string testName(const ::testing::TestParamInfo<std::string>& info) {
  std::string name = info.param.substr(info.param.find('/') + 1);
  for (char& c : name) {
    if (c == '-' || c == '.') c = '_';
  }
  return name;
}

// What the engine made of every glyph of a font at one size, against FreeType.
struct Comparison {
  uint32_t glyphs = 0;             ///< glyphs FreeType draws
  uint32_t tooLarge = 0;           ///< ... over 255 px a side or over the output region
  uint32_t overlapGlyphs = 0;      ///< ... flagged OVERLAP
  uint32_t loadFailures = 0;       ///< ... the engine does not load
  uint32_t advanceDiffs = 0;       ///< ... whose advance differs
  uint32_t boxDiffs = 0;           ///< ... whose bitmap box differs
  uint32_t inexact = 0;            ///< ... whose bitmap differs
  uint64_t pixels = 0;             ///< the pixels of the glyphs whose boxes match
  uint64_t differingPixels = 0;    ///< ... that differ
  uint32_t freeTypeFails = 0;      ///< glyphs FreeType cannot load
  uint32_t refusedAsFreeType = 0;  ///< ... the engine reports Malformed or Unsupported
  std::string inexactPictures;     ///< the first three inexact glyphs, ours then FreeType's
};

// Every glyph of `face` at `points`, drawn by the engine and by FreeType, into `out`. A load failure, and a glyph too
// large that is not Unsupported, fail the test here; the caller checks the counts.
void compareWithFreeType(const std::string& name, const std::vector<uint8_t>& data, FontBytes& bytes, const Face& face,
                         const uint32_t points, Comparison* out) {
  Comparison& c = *out;
  const FreeTypeReference reference(data, points);
  ASSERT_TRUE(reference.ok());
  ASSERT_EQ(reference.numGlyphs(), face.numGlyphs);
  const uint32_t ppem = ppemFromPoints(points);
  Rasterizer rasterizer;
  ASSERT_TRUE(rasterizer.allocate(planScratch(face, ppem, kPoolBytesFlash)));

  for (uint32_t g = 0; g < face.numGlyphs; ++g) {
    const ReferenceGlyph expected = reference.glyph(g);
    EpdGlyph metrics{};
    const Status status = rasterizer.render(bytes, face, static_cast<uint16_t>(g), ppem, &metrics);
    if (!expected.loaded) {
      ++c.freeTypeFails;
      c.refusedAsFreeType += status == Status::Malformed || status == Status::Unsupported;
      continue;
    }
    ++c.glyphs;
    // A bitmap over 255 px a side cannot be an EpdGlyph, and one over the output region is past its cap
    // (spec 5.3): both are Unsupported by design. Ubuntu's wordmark at 40 pt is 288 px wide.
    if (expected.width > 255 || expected.height > 255 ||
        glyphDataBytes(expected.width, expected.height, true) > rasterizer.plan().outputBytes) {
      EXPECT_EQ(Status::Unsupported, status) << "glyph " << g;
      ++c.tooLarge;
      continue;
    }
    if (status != Status::Ok) {
      ++c.loadFailures;
      ADD_FAILURE() << "glyph " << g << ": " << statusName(status);
      continue;
    }
    if (metrics.advanceX != static_cast<uint16_t>((expected.linearAdvance + 2048) >> 12)) ++c.advanceDiffs;
    if (metrics.width != expected.width || metrics.height != expected.height || metrics.left != expected.left ||
        metrics.top != expected.top) {
      ++c.boxDiffs;
      continue;
    }
    c.overlapGlyphs += expected.overlap;
    const uint8_t* ours = rasterizer.output();
    uint32_t differ = 0;
    for (int p = 0; p < expected.width * expected.height; ++p) {
      differ += pixelAt(ours, p) != pixelAt(expected.bitmap.data(), p);
    }
    c.pixels += static_cast<uint64_t>(expected.width) * static_cast<uint64_t>(expected.height);
    c.differingPixels += differ;
    if (differ > 0 && ++c.inexact <= 3) {
      c.inexactPictures += "glyph " + std::to_string(g) + ", " + std::to_string(differ) +
                           " pixels differ, ours then FreeType's:\n" + picture(metrics, ours) + "---\n" +
                           picture(metrics, expected.bitmap.data());
    }
  }
  std::printf(
      "%s @%u pt: %u glyphs (%u too large, %u flagged overlap), %u load failures, %u advance diffs, %u box "
      "diffs, %u inexact (%llu of %llu pixels differ); %u FreeType cannot load, %u of them refused\n",
      name.c_str(), points, c.glyphs, c.tooLarge, c.overlapGlyphs, c.loadFailures, c.advanceDiffs, c.boxDiffs,
      c.inexact, static_cast<unsigned long long>(c.differingPixels), static_cast<unsigned long long>(c.pixels),
      c.freeTypeFails, c.refusedAsFreeType);
}

class TtfVsFreeType : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    data = readFont(GetParam());
    ASSERT_FALSE(data.empty());
    bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
    ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  }

  std::vector<uint8_t> data;
  FontBytes bytes;
  Face face;
};

}  // namespace

TEST_P(TtfVsFreeType, EveryGlyphMatchesFreeType) {
  // Small print, the body size, and a large heading.
  for (const uint32_t points : {8u, kPoints, 40u}) {
    SCOPED_TRACE(points);
    Comparison c;
    compareWithFreeType(GetParam(), data, bytes, face, points, &c);
    if (HasFatalFailure()) return;
    EXPECT_GT(c.glyphs, 1000u);
    EXPECT_EQ(0u, c.loadFailures);
    EXPECT_EQ(0u, c.advanceDiffs);
    EXPECT_EQ(0u, c.boxDiffs);  // spec 9.3 allows 5; scaling exactly as FreeType does leaves none
    EXPECT_EQ(0u, c.inexact) << c.inexactPictures;
  }
}

TEST_P(TtfVsFreeType, OversampledOverlapsStayWithinThreePercent) {
  // Spec 9.3's pixel budget, against FreeType as it really renders outlines flagged OVERLAP: 4x4 oversampled.
  const FreeTypeReference reference(data, kPoints);
  ASSERT_TRUE(reference.ok());
  const uint32_t ppem = ppemFromPoints(kPoints);
  Rasterizer rasterizer;
  ASSERT_TRUE(rasterizer.allocate(planScratch(face, ppem, kPoolBytesFlash)));
  uint32_t overlapGlyphs = 0;
  uint64_t pixels = 0, differ = 0;
  for (uint32_t g = 0; g < face.numGlyphs; ++g) {
    const ReferenceGlyph expected = reference.glyph(g, true);
    if (!expected.loaded || !expected.overlap) continue;
    EpdGlyph metrics{};
    ASSERT_EQ(Status::Ok, rasterizer.render(bytes, face, static_cast<uint16_t>(g), ppem, &metrics)) << g;
    ASSERT_EQ(expected.width, metrics.width) << g;
    ASSERT_EQ(expected.height, metrics.height) << g;
    ++overlapGlyphs;
    for (int p = 0; p < expected.width * expected.height; ++p) {
      ++pixels;
      differ += pixelAt(rasterizer.output(), p) != pixelAt(expected.bitmap.data(), p);
    }
  }
  std::printf("%s @%u pt: %u overlap glyphs, %.2f%% of their pixels differ from FreeType's oversampled ones\n",
              GetParam().c_str(), kPoints, overlapGlyphs,
              pixels ? 100.0 * static_cast<double>(differ) / static_cast<double>(pixels) : 0.0);
  EXPECT_LE(differ * 100, pixels * 3);
}

TEST_P(TtfVsFreeType, TheCmapMatchesFreeType) {
  const FreeTypeReference reference(data, kPoints);
  ASSERT_TRUE(reference.ok());
  uint32_t mapped = 0, diffs = 0;
  for (uint32_t cp = 0; cp < 0x30000; ++cp) {
    const uint32_t expected = reference.glyphIndex(cp);
    mapped += expected != 0;
    if (glyphIndex(bytes, face, cp) != expected && ++diffs <= 5) ADD_FAILURE() << "U+" << std::hex << cp;
  }
  EXPECT_GT(mapped, 200u);
  EXPECT_EQ(0u, diffs);
  EXPECT_FALSE(bytes.outOfRange());
}

TEST_P(TtfVsFreeType, LineMetricsMatchTheConverterAtEverySize) {
  for (const uint32_t points : {8u, 12u, 14u, 18u, 24u, 40u}) {
    const FreeTypeReference reference(data, points);
    ASSERT_TRUE(reference.ok());
    const LineMetrics m = lineMetrics(face, ppemFromPoints(points));
    // fontconvert_sdcard.py: advanceY = ceil(height / 64), ascender = ceil, descender = floor.
    EXPECT_EQ(reference.height() / 64, m.advanceY) << points;
    EXPECT_EQ(reference.ascender() / 64, m.ascender) << points;
    EXPECT_EQ(reference.descender() / 64, m.descender) << points;
  }
}

INSTANTIATE_TEST_SUITE_P(BundledFonts, TtfVsFreeType, ::testing::ValuesIn(bundledTtfs()), testName);

TEST(TtfPoolSize, ThePoolNeverChangesTheOutput) {
  // Spec 2: the pool size is a speed dial only. Every glyph of a text face, with three pools.
  const std::vector<uint8_t> data = readFont("Bookerly/Bookerly-Regular.ttf");
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  const uint32_t ppem = ppemFromPoints(kPoints);
  Rasterizer small, flash, sd;
  ASSERT_TRUE(small.allocate(planScratch(face, ppem, 2048)));
  ASSERT_TRUE(flash.allocate(planScratch(face, ppem, kPoolBytesFlash)));
  ASSERT_TRUE(sd.allocate(planScratch(face, ppem, kPoolBytesSd)));
  for (uint16_t g = 0; g < face.numGlyphs; ++g) {
    EpdGlyph a{}, b{}, c{};
    ASSERT_EQ(Status::Ok, small.render(bytes, face, g, ppem, &a));
    ASSERT_EQ(Status::Ok, flash.render(bytes, face, g, ppem, &b));
    ASSERT_EQ(Status::Ok, sd.render(bytes, face, g, ppem, &c));
    ASSERT_EQ(0, std::memcmp(&a, &b, sizeof a)) << g;
    ASSERT_EQ(0, std::memcmp(&a, &c, sizeof a)) << g;
    ASSERT_EQ(0, std::memcmp(small.output(), flash.output(), a.dataLength)) << g;
    ASSERT_EQ(0, std::memcmp(small.output(), sd.output(), a.dataLength)) << g;
  }
}

namespace {

// The CFF test fonts: the fixtures and the bundled OpenDyslexic sources.
class TtfVsFreeTypeCff : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    data = readCffTestFont(GetParam());
    ASSERT_FALSE(data.empty());
    bytes = FontBytes(data.data(), static_cast<uint32_t>(data.size()));
    ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
    ASSERT_EQ(Face::Outlines::Cff, face.outlines);
  }

  std::vector<uint8_t> data;
  FontBytes bytes;
  Face face;
};

// A font and size whose bitmaps cannot match FreeType's exactly, with the measured share of its pixels that differ
// (spec 9.3 allows at most 3%).
struct PixelTolerance {
  const char* font;
  uint32_t points;
  double percent;
};

// Empty: every glyph of every CFF test font comes out as FreeType draws it.
const std::vector<PixelTolerance> kCffPixelTolerance = {};

const PixelTolerance* cffPixelTolerance(const std::string& font, const uint32_t points) {
  for (const PixelTolerance& t : kCffPixelTolerance) {
    if (font == t.font && points == t.points) return &t;
  }
  return nullptr;
}

}  // namespace

TEST_P(TtfVsFreeTypeCff, EveryGlyphMatchesFreeType) {
  // As the TrueType fonts: advances, boxes and bitmaps exactly FreeType's, and a glyph FreeType cannot load is one
  // the engine refuses as Malformed or Unsupported. No glyph is exempt: CffOperators has none using `random`, the one
  // operator FreeType runs (seeded from memory addresses) and the engine refuses.
  for (const uint32_t points : {8u, kPoints, 40u}) {
    SCOPED_TRACE(points);
    Comparison c;
    compareWithFreeType(GetParam(), data, bytes, face, points, &c);
    if (HasFatalFailure()) return;
    EXPECT_GT(c.glyphs, 0u);
    EXPECT_EQ(0u, c.loadFailures);
    EXPECT_EQ(c.freeTypeFails, c.refusedAsFreeType);
    EXPECT_EQ(0u, c.advanceDiffs);
    EXPECT_EQ(0u, c.boxDiffs);
    const PixelTolerance* tolerance = cffPixelTolerance(GetParam(), points);
    if (tolerance == nullptr) {
      EXPECT_EQ(0u, c.inexact) << c.inexactPictures;
    } else {
      ASSERT_LE(tolerance->percent, 3.0);
      EXPECT_LE(static_cast<double>(c.differingPixels), static_cast<double>(c.pixels) * tolerance->percent / 100)
          << c.inexactPictures;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(CffTestFonts, TtfVsFreeTypeCff, ::testing::ValuesIn(cffTestFonts()), testName);

TEST(TtfVsFreeTypeCffSynthetic, FontMatricesAndFontDictScalesMatchFreeType) {
  // The font matrix paths no test font reaches (NotoSansJP-CffSlanted's Top DICT matrix is the only one, with no
  // offset): a Top DICT matrix with an offset, and CID-keyed font DICTs with matrices of their own, concatenated with
  // the Top DICT's, with offsets, and with units per em the glyph is scaled by (cffgload.c:446-452). Lines, a curve and
  // a counter in each glyph.
  Bytes shape;
  const auto op = [&shape](const std::initializer_list<int> operands, const uint8_t code) {
    for (const int v : operands) {  // Type 2 integers from -1131 to 1131
      if (v >= -107 && v <= 107) {
        shape.push_back(static_cast<uint8_t>(v + 139));
      } else {
        const int magnitude = (v < 0 ? -v : v) - 108;
        shape.push_back(static_cast<uint8_t>((v < 0 ? 251 : 247) + magnitude / 256));
        shape.push_back(static_cast<uint8_t>(magnitude % 256));
      }
    }
    shape.push_back(code);
  };
  op({100, 100}, 21);                     // rmoveto
  op({350, 0, -50, 300}, 5);              // rlineto
  op({-80, 100, -150, 0, -70, -100}, 8);  // rrcurveto
  op({100, -200}, 21);                    // the counter
  op({0, 100, 150, 0, 0, -100}, 5);       // rlineto
  shape.push_back(14);                    // endchar
  const char* const fontDicts[] = {"", "0.0005 0 0 0.0005 0 0", "0.002 0 0 0.002 -10 20", "0.001 0 0.0003 0.001 5 5"};
  std::vector<std::pair<std::string, Bytes>> fonts;
  CffSpec named;
  named.charStrings = {{14}, shape};
  named.topMatrix = "0.001 0 0.0002 0.001 0.05 -0.03";
  named.fds = {CffFontDict{}};
  fonts.emplace_back("name-keyed [" + named.topMatrix + "]", buildOtf(named));
  for (const char* top : {"", "0.001 0 0 0.001 0 0", "0.001 0.0003 -0.00025 0.0012 3.75 -1.5"}) {
    CffSpec spec;
    spec.cid = true;
    spec.topMatrix = top;
    spec.charStrings = {{14}};
    spec.fdSelect = {0, 0};  // format 0: .notdef in FD 0, then glyph g in FD g - 1
    for (const char* fd : fontDicts) {
      spec.fds.push_back(cffFontDict(fd));
      spec.charStrings.push_back(shape);
      spec.fdSelect.push_back(static_cast<uint8_t>(spec.fds.size() - 1));
    }
    fonts.emplace_back(std::string("CID-keyed, top [") + top + "]", buildOtf(spec));
  }

  uint32_t offsets = 0;
  uint32_t scales = 0;
  for (const auto& [what, data] : fonts) {
    SCOPED_TRACE(what);
    FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
    Face face;
    ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
    for (uint16_t g = 1; g < face.numGlyphs; ++g) {
      CffGlyphSource source;
      ASSERT_EQ(Status::Ok, cffGlyphSource(bytes, face, g, &source));
      offsets += source.matrix.dx != 0 || source.matrix.dy != 0;
      scales += source.scaleNum != source.scaleDen;
    }
    for (const uint32_t points : {8u, kPoints, 40u}) {
      SCOPED_TRACE(points);
      Comparison c;
      compareWithFreeType(what, data, bytes, face, points, &c);
      if (HasFatalFailure()) return;
      EXPECT_EQ(face.numGlyphs, c.glyphs);
      EXPECT_EQ(0u, c.tooLarge);
      EXPECT_EQ(0u, c.loadFailures);
      EXPECT_EQ(0u, c.advanceDiffs);
      EXPECT_EQ(0u, c.boxDiffs);
      EXPECT_EQ(0u, c.inexact) << c.inexactPictures;
    }
  }
  // The paths are reached: offsets in the name-keyed font and in font DICTs, and font DICTs' own units per em.
  std::printf("%u glyphs with a font offset, %u scaled by a font DICT's units per em\n", offsets, scales);
  EXPECT_GE(offsets, 4u);
  EXPECT_GE(scales, 6u);
}

namespace {

// The golden hash of Bookerly Regular at 14 pt as `rasterizer` draws it, calling `between` before each glyph.
template <typename Between>
uint32_t bookerlyHash(Rasterizer& rasterizer, FontBytes& bytes, const Face& face, Between between) {
  uint32_t hash = ttf_golden::kSeed;
  for (uint16_t g = 0; g < face.numGlyphs; ++g) {
    between();
    EpdGlyph metrics{};
    if (rasterizer.render(bytes, face, g, ppemFromPoints(kPoints), &metrics) != Status::Ok) continue;
    hash = ttf_golden::hashGlyph(hash, g, metrics, rasterizer.output());
  }
  return hash;
}

}  // namespace

TEST(TtfGolden, TheDeviceBenchExpectsThisOutput) {
  // bench/ttf_raster_main.cpp computes the same hash on the device, over the same glyphs.
  const std::vector<uint8_t> data = readFont("Bookerly/Bookerly-Regular.ttf");
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  Rasterizer rasterizer;
  ASSERT_TRUE(rasterizer.allocate(planScratch(face, ppemFromPoints(40), kPoolBytesFlash)));
  const uint32_t hash = bookerlyHash(rasterizer, bytes, face, [] {});
  EXPECT_EQ(ttf_golden::kBookerlyRegular14pt, hash) << std::hex << hash;
}

TEST(TtfCffGolden, TrueTypeOutputIsUnchanged) {
  // One block for a CFF face and Bookerly, as a family of both plans it (the interpreter's region moves the output
  // region), and a CFF glyph drawn before each TrueType one: Bookerly comes out as the device bench expects.
  const std::vector<uint8_t> otf = readCffTestFont("NotoSansJP-Cff.otf");
  FontBytes otfBytes(otf.data(), static_cast<uint32_t>(otf.size()));
  Face cff;
  ASSERT_EQ(FaceStatus::Ok, parseFace(otfBytes, &cff));
  const std::vector<uint8_t> data = readFont("Bookerly/Bookerly-Regular.ttf");
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  const uint32_t ppem = ppemFromPoints(40);
  Rasterizer rasterizer;
  ASSERT_TRUE(rasterizer.allocate(
      mergePlans(planScratch(cff, ppem, kPoolBytesFlash), planScratch(face, ppem, kPoolBytesFlash))));
  EXPECT_EQ(kCffDecoderBytes, rasterizer.plan().decoderBytes);
  uint16_t cffGlyph = 0;
  uint32_t cffDrawn = 0;
  const uint32_t hash = bookerlyHash(rasterizer, bytes, face, [&] {
    EpdGlyph metrics{};
    cffDrawn += rasterizer.render(otfBytes, cff, cffGlyph, ppemFromPoints(kPoints), &metrics) == Status::Ok;
    cffGlyph = static_cast<uint16_t>((cffGlyph + 1) % cff.numGlyphs);
  });
  EXPECT_EQ(ttf_golden::kBookerlyRegular14pt, hash) << std::hex << hash;
  EXPECT_EQ(face.numGlyphs, cffDrawn);
}

TEST(TtfVsFreeTypeSynthetic, CompoundsMatchFreeTypeBitForBit) {
  // The loader paths no bundled font reaches (Review Focus 2): rotated, scaled and mirrored components,
  // USE_MY_METRICS taking a component's left side bearing, and that two levels deep; quadratic curves throughout.
  Font font;
  font.glyphs.resize(8);
  font.glyphs[0].contours = {rect(50, 0, 450, 700)};
  font.glyphs[1].contours = {// a bowl and its counter
                             {{250, 0},
                              {450, 0, false},
                              {450, 250},
                              {450, 500, false},
                              {250, 500},
                              {50, 500, false},
                              {50, 250},
                              {50, 0, false}},
                             {{250, 100},
                              {150, 100, false},
                              {150, 250},
                              {150, 400, false},
                              {250, 400},
                              {350, 400, false},
                              {350, 250},
                              {350, 100, false}}};
  font.glyphs[2].contours = {rect(80, 0, 160, 700)};  // a stem whose lsb is not its xMin
  font.glyphs[2].lsbIsXMin = false;
  font.glyphs[2].lsb = 20;
  font.glyphs[2].advance = 300;
  font.glyphs[3].components = {{1, 300, 0, {0x3800, 0x0C00, -0x0C00, 0x3800}}, {2, -40, 80, {0x4000, 0x5000}}};
  font.glyphs[4].components = {{2, 10, 0, {}, 0x0200}, {1, 200, 100}};  // USE_MY_METRICS on the stem
  font.glyphs[5].components = {{3, 0, 0}, {4, 600, -50, {}, 0x0200}};   // ...and through a compound
  font.glyphs[6].components = {{1, 500, 0, {-0x4000, 0x4000}}};         // mirrored
  font.glyphs[7].components = {{2, 0, 0, {0x2666}}};                    // scaled to 0.6
  font.cmap = {{'A', 1}};
  const Bytes data = buildFont(font);
  FontBytes bytes(data.data(), static_cast<uint32_t>(data.size()));
  Face face;
  ASSERT_EQ(FaceStatus::Ok, parseFace(bytes, &face));
  Rasterizer rasterizer;
  ASSERT_TRUE(rasterizer.allocate({64, 16, kMaxOutputBytes, kPoolBytesFlash}));
  for (const uint32_t points : {8u, kPoints, 40u}) {
    const FreeTypeReference reference(data, points);
    ASSERT_TRUE(reference.ok());
    for (uint16_t g = 1; g < 8; ++g) {
      const ReferenceGlyph expected = reference.glyph(g);
      ASSERT_TRUE(expected.loaded) << g;
      EpdGlyph metrics{};
      ASSERT_EQ(Status::Ok, rasterizer.render(bytes, face, g, ppemFromPoints(points), &metrics)) << g;
      EXPECT_EQ(static_cast<uint16_t>((expected.linearAdvance + 2048) >> 12), metrics.advanceX) << g << " @" << points;
      ASSERT_EQ(expected.width, metrics.width) << g << " @" << points;
      ASSERT_EQ(expected.height, metrics.height) << g << " @" << points;
      EXPECT_EQ(expected.left, metrics.left) << g << " @" << points;
      EXPECT_EQ(expected.top, metrics.top) << g << " @" << points;
      EXPECT_EQ(0, std::memcmp(rasterizer.output(), expected.bitmap.data(), expected.bitmap.size()))
          << g << " @" << points << ", ours then FreeType's:\n"
          << picture(metrics, rasterizer.output()) << "---\n"
          << picture(metrics, expected.bitmap.data());
    }
  }
}
