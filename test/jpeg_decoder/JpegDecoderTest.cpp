// Regression tests for the TJpgDec-based JPEG decode path.
//
// These guard three bugs found on real EPUB covers (see lib/TJpgDec and
// lib/JpegToBmpConverter / lib/Epub/.../JpegToFramebufferConverter):
//
//   1. Descale extent: TJpgDec emits floor(dim / 2^scale), not ceil. The
//      JpegToBmpConverter buffers a full MCU row and only flushes it once a
//      block reaches `srcWidth`; an over-estimate (ceil) on an odd dimension
//      meant the last column never arrived and zero rows were written.
//   2. Grayscale clamp: with JD_FASTDECODE>=1 the IDCT writes unclamped int16_t.
//      The grayscale output path must BYTECLIP it, or IDCT overshoot near sharp
//      edges wraps mod 256 -> black speckle in white areas.
//
// Fixtures live in fixtures/ and are reproducible via fixtures/generate_fixtures.py.

#include <gtest/gtest.h>
#include <tjpgd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "BitmapHelpers.h"
#include "BuildArena.h"
#include "HalStorage.h"  // FsFile (stdio-backed shim)
#include "JpegToBmpConverter.h"
#include "Print.h"
#include "ProgressiveJpeg.h"
#include "ProgressiveJpegDc.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "."
#endif

namespace {

std::string fixture(const char* name) { return std::string(FIXTURE_DIR) + "/" + name; }

// --- Minimal TJpgDec driver (file in, grayscale out) -----------------------

struct DecodeResult {
  JRESULT jr = JDR_OK;
  int width = 0;              // TJpgDec source width (jdec.width)
  int height = 0;             // TJpgDec source height (jdec.height)
  int outRight = 0;           // max (rect.right + 1) seen — actual emitted width
  int outBottom = 0;          // max (rect.bottom + 1) seen — actual emitted height
  std::vector<uint8_t> gray;  // outRight x outBottom, row-major (scale 0 only here)
};

struct DriverCtx {
  FILE* fp = nullptr;
  DecodeResult* res = nullptr;
  int bufW = 0;  // width of the gray buffer (== source width >> scale)
};

size_t driverInput(JDEC* jd, uint8_t* buff, size_t ndata) {
  FILE* fp = static_cast<DriverCtx*>(jd->device)->fp;
  if (buff) return fread(buff, 1, ndata, fp);
  return fseek(fp, static_cast<long>(ndata), SEEK_CUR) == 0 ? ndata : 0;
}

int driverOutput(JDEC* jd, void* bitmap, JRECT* rect) {
  auto* ctx = static_cast<DriverCtx*>(jd->device);
  DecodeResult* r = ctx->res;
  const auto* px = static_cast<const uint8_t*>(bitmap);
  const int validW = rect->right - rect->left + 1;
  const int blockH = rect->bottom - rect->top + 1;
  if (rect->right + 1 > r->outRight) r->outRight = rect->right + 1;
  if (rect->bottom + 1 > r->outBottom) r->outBottom = rect->bottom + 1;
  for (int y = 0; y < blockH; y++) {
    for (int x = 0; x < validW; x++) {
      const int gx = rect->left + x;
      const int gy = rect->top + y;
      if (gx < ctx->bufW && gy < static_cast<int>(r->gray.size()) / ctx->bufW) {
        r->gray[gy * ctx->bufW + gx] = px[y * validW + x];
      }
    }
  }
  return 1;
}

DecodeResult decodeGray(const std::string& path, uint8_t scale) {
  DecodeResult res;
  FILE* fp = fopen(path.c_str(), "rb");
  EXPECT_NE(fp, nullptr) << "could not open fixture: " << path;
  if (!fp) {
    res.jr = JDR_INP;
    return res;
  }

  std::vector<uint8_t> pool(32 * 1024);
  DriverCtx ctx;
  ctx.fp = fp;
  ctx.res = &res;

  JDEC jdec;
  res.jr = jd_prepare(&jdec, driverInput, pool.data(), pool.size(), &ctx);
  if (res.jr == JDR_OK) {
    res.width = jdec.width;
    res.height = jdec.height;
    ctx.bufW = jdec.width >> scale;
    res.gray.assign(static_cast<size_t>(ctx.bufW) * (jdec.height >> scale), 0);
    res.jr = jd_decomp(&jdec, driverOutput, scale);
  }
  fclose(fp);
  return res;
}

// --- PGM (P5) loader for golden images -------------------------------------

struct Pgm {
  int w = 0, h = 0;
  std::vector<uint8_t> px;
};

Pgm loadPgm(const std::string& path) {
  Pgm out;
  FILE* fp = fopen(path.c_str(), "rb");
  EXPECT_NE(fp, nullptr) << "could not open golden: " << path;
  if (!fp) return out;
  char magic[3] = {};
  int maxval = 0;
  if (fscanf(fp, "%2s %d %d %d", magic, &out.w, &out.h, &maxval) == 4 && std::string(magic) == "P5" && maxval == 255) {
    fgetc(fp);  // single whitespace after maxval
    out.px.resize(static_cast<size_t>(out.w) * out.h);
    const size_t got = fread(out.px.data(), 1, out.px.size(), fp);
    EXPECT_EQ(got, out.px.size()) << "short read on golden PGM: " << path;
  }
  fclose(fp);
  return out;
}

// --- In-memory Print sink for the BMP converter ----------------------------

struct MemoryPrint : public Print {
  std::vector<uint8_t> buf;
  size_t write(uint8_t b) override {
    buf.push_back(b);
    return 1;
  }
  size_t write(const uint8_t* p, size_t n) override {
    buf.insert(buf.end(), p, p + n);
    return n;
  }
};

int32_t le32(const std::vector<uint8_t>& b, size_t off) {
  return static_cast<int32_t>(b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) |
                              (static_cast<uint32_t>(b[off + 3]) << 24));
}

uint16_t le16(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

struct ProgressiveRows {
  uint16_t width = 0;
  uint16_t height = 0;
  std::vector<uint8_t> pixels;

  static bool accept(void* user, uint16_t y, const uint8_t* row, uint16_t rowWidth) {
    auto& capture = *static_cast<ProgressiveRows*>(user);
    if (y != capture.height || (capture.width != 0 && capture.width != rowWidth)) return false;
    capture.width = rowWidth;
    capture.height++;
    capture.pixels.insert(capture.pixels.end(), row, row + rowWidth);
    return true;
  }
};

}  // namespace

TEST(BitmapHelpers, WritesGrayscaleBmpHeaders) {
  for (const uint8_t bitsPerPixel : {1, 2, 8}) {
    MemoryPrint output;
    const int rowBytes = writeGrayscaleBmpHeader(output, 13, 7, bitsPerPixel);
    const uint32_t colorCount = 1U << bitsPerPixel;
    const uint32_t pixelOffset = 54 + colorCount * 4;

    ASSERT_EQ(rowBytes, (13 * bitsPerPixel + 31) / 32 * 4);
    ASSERT_EQ(output.buf.size(), pixelOffset);
    EXPECT_EQ(output.buf[0], 'B');
    EXPECT_EQ(output.buf[1], 'M');
    EXPECT_EQ(le32(output.buf, 10), pixelOffset);
    EXPECT_EQ(le32(output.buf, 18), 13);
    EXPECT_EQ(le32(output.buf, 22), -7);
    EXPECT_EQ(le16(output.buf, 28), bitsPerPixel);
    EXPECT_EQ(le32(output.buf, 46), colorCount);
    EXPECT_EQ(output.buf[54], 0);
    EXPECT_EQ(output.buf[pixelOffset - 4], 255);
    if (bitsPerPixel == 2) {
      EXPECT_EQ(output.buf[58], 85);
      EXPECT_EQ(output.buf[62], 170);
    }
  }
}

// ---------------------------------------------------------------------------
// Bug 1: TJpgDec descales by floor(dim/2^scale). The converter relies on this
// exact extent; ceil would over-estimate on odd dimensions.
// ---------------------------------------------------------------------------
TEST(JpegDecoder, DescaleExtentIsFloorNotCeil) {
  DecodeResult full = decodeGray(fixture("odd_420.jpg"), 0);
  ASSERT_EQ(full.jr, JDR_OK);
  EXPECT_EQ(full.width, 35);
  EXPECT_EQ(full.height, 53);
  EXPECT_EQ(full.outRight, 35);
  EXPECT_EQ(full.outBottom, 53);

  DecodeResult half = decodeGray(fixture("odd_420.jpg"), 1);
  ASSERT_EQ(half.jr, JDR_OK);
  // floor(35/2)=17, floor(53/2)=26 — NOT ceil (18/27).
  EXPECT_EQ(half.outRight, 35 >> 1);
  EXPECT_EQ(half.outBottom, 53 >> 1);
  EXPECT_EQ(half.outRight, 17);
  EXPECT_EQ(half.outBottom, 26);
}

// ---------------------------------------------------------------------------
// Bug 2: grayscale output must clamp. Compare against a reference (Pillow)
// decode; the wrap-around bug produced max diffs of 255.
// ---------------------------------------------------------------------------
TEST(JpegDecoder, GrayscaleOutputIsClamped) {
  DecodeResult dec = decodeGray(fixture("contrast_420.jpg"), 0);
  ASSERT_EQ(dec.jr, JDR_OK);

  Pgm golden = loadPgm(fixture("contrast_420.gray.pgm"));
  ASSERT_EQ(golden.w, dec.outRight);
  ASSERT_EQ(golden.h, dec.outBottom);
  ASSERT_EQ(dec.gray.size(), golden.px.size());

  int maxDiff = 0;
  long sumDiff = 0;
  for (size_t i = 0; i < golden.px.size(); i++) {
    const int d = std::abs(static_cast<int>(dec.gray[i]) - static_cast<int>(golden.px[i]));
    if (d > maxDiff) maxDiff = d;
    sumDiff += d;
  }
  const double meanDiff = static_cast<double>(sumDiff) / golden.px.size();
  // IDCT rounding differs from Pillow by <=1; the unclamped wrap bug gave 255.
  EXPECT_LE(maxDiff, 8) << "grayscale output not clamped (IDCT overshoot wrapped mod 256)";
  EXPECT_LE(meanDiff, 3.0);
}

TEST(ProgressiveJpegDc, DecodesFirstScanIntoResizedRows) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("progressive_420.jpg")));

  ProgressiveRows capture;
  ProgressiveJpegDc::DecodeOptions options;
  options.outputWidth = 48;
  options.outputHeight = 32;
  const auto result = ProgressiveJpegDc::decode(file, options, ProgressiveRows::accept, &capture);
  file.close();

  ASSERT_EQ(result, ProgressiveJpegDc::Result::Ok) << ProgressiveJpegDc::resultName(result);
  ASSERT_EQ(capture.width, 48);
  ASSERT_EQ(capture.height, 32);
  ASSERT_EQ(capture.pixels.size(), 48u * 32u);
  EXPECT_LT(capture.pixels[8 * 48 + 40], 80);    // black upper-right region
  EXPECT_GT(capture.pixels[24 * 48 + 40], 175);  // white lower-right region
  EXPECT_LT(capture.pixels[16 * 48 + 4], capture.pixels[16 * 48 + 20]);
}

// ---------------------------------------------------------------------------
// RowScaler::mapCoordinate computed `outputIndex * (sourceSize - 1) << 16` in 32 bits.
// `*` binds tighter than `<<`, so the product was formed first and the shift overflowed
// once it reached 65536; the source position then wrapped to near zero, submit()'s
// "have we reached this source row yet" test passed for every remaining output row at
// once, and they were all emitted from the last decoded row. On a real 1920x2708
// progressive cover scaled to a 540-row thumbnail that meant 194 correct rows followed
// by 346 copies of one row. Both axes were affected; one test each.
// ---------------------------------------------------------------------------
TEST(ProgressiveJpegDc, TallImageDoesNotRepeatRowsPastOverflow) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("progressive_tall.jpg")));

  ProgressiveRows capture;
  ProgressiveJpegDc::DecodeOptions options;
  options.outputWidth = 8;
  options.outputHeight = 540;  // 339 DC rows: the wrap began at output row 194
  const auto result = ProgressiveJpegDc::decode(file, options, ProgressiveRows::accept, &capture);
  file.close();

  ASSERT_EQ(result, ProgressiveJpegDc::Result::Ok) << ProgressiveJpegDc::resultName(result);
  ASSERT_EQ(capture.height, 540);

  // The source is a black-to-white gradient top to bottom, so the output has to keep
  // getting brighter all the way down. The bug froze it at the value of row ~193.
  const auto row = [&](int y) { return capture.pixels[static_cast<size_t>(y) * capture.width]; };
  EXPECT_LT(row(0), row(193));
  EXPECT_LT(row(193), row(300));
  EXPECT_LT(row(300), row(539));
  EXPECT_GT(row(539), 200);  // reaches white rather than freezing at ~36% grey
}

TEST(ProgressiveJpegDc, WideImageDoesNotRepeatColumnsPastOverflow) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("progressive_wide.jpg")));

  ProgressiveRows capture;
  ProgressiveJpegDc::DecodeOptions options;
  options.outputWidth = 382;  // 240 DC columns: the wrap began at output column 275
  options.outputHeight = 8;
  const auto result = ProgressiveJpegDc::decode(file, options, ProgressiveRows::accept, &capture);
  file.close();

  ASSERT_EQ(result, ProgressiveJpegDc::Result::Ok) << ProgressiveJpegDc::resultName(result);
  ASSERT_EQ(capture.width, 382);

  // Left-to-right gradient, read across the first output row.
  const auto col = [&](int x) { return capture.pixels[static_cast<size_t>(x)]; };
  EXPECT_LT(col(0), col(274));
  EXPECT_LT(col(274), col(330));
  EXPECT_LT(col(330), col(381));
  EXPECT_GT(col(381), 200);
}

TEST(ProgressiveJpegDc, RejectsBaselineWithoutEmittingRows) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("contrast_420.jpg")));

  ProgressiveRows capture;
  ProgressiveJpegDc::DecodeOptions options;
  options.outputWidth = 48;
  options.outputHeight = 32;
  const auto result = ProgressiveJpegDc::decode(file, options, ProgressiveRows::accept, &capture);
  file.close();

  EXPECT_EQ(result, ProgressiveJpegDc::Result::Unsupported);
  EXPECT_TRUE(capture.pixels.empty());
}

// ---------------------------------------------------------------------------
// ProgressiveJpeg::decodeResized: the 1/8 decode resampled to a box -- the DC-only preview's job,
// from every DC scan, and the same tests. The 1/8 levels are whole blocks' averages, so it
// interpolates between them in both axes.
// ---------------------------------------------------------------------------
namespace {

ProgressiveJpeg::Result decodeResized(const char* name, const uint16_t width, const uint16_t height,
                                      ProgressiveRows& capture) {
  FsFile file;
  EXPECT_TRUE(file.openForRead(fixture(name)));
  ProgressiveJpeg::DecodeOptions options;
  const auto result = ProgressiveJpeg::decodeResized(file, options, width, height, ProgressiveRows::accept, &capture);
  file.close();
  return result;
}

}  // namespace

TEST(ProgressiveJpegResized, DecodesTheEighthIntoResizedRows) {
  ProgressiveRows capture;
  const auto result = decodeResized("progressive_420.jpg", 48, 32, capture);
  ASSERT_EQ(result, ProgressiveJpeg::Result::Ok) << ProgressiveJpeg::resultName(result);
  ASSERT_EQ(capture.width, 48);
  ASSERT_EQ(capture.height, 32);
  ASSERT_EQ(capture.pixels.size(), 48u * 32u);
  EXPECT_LT(capture.pixels[8 * 48 + 40], 80);    // black upper-right region
  EXPECT_GT(capture.pixels[24 * 48 + 40], 175);  // white lower-right region
  EXPECT_LT(capture.pixels[16 * 48 + 4], capture.pixels[16 * 48 + 20]);
}

// ---------------------------------------------------------------------------
// RowScaler::mapCoordinate computed `outputIndex * (sourceSize - 1) << 16` in 32 bits.
// `*` binds tighter than `<<`, so the product was formed first and the shift overflowed
// once it reached 65536; the source position then wrapped to near zero, submit()'s
// "have we reached this source row yet" test passed for every remaining output row at
// once, and they were all emitted from the last decoded row. On a real 1920x2708
// progressive cover scaled to a 540-row thumbnail that meant 194 correct rows followed
// by 346 copies of one row. Both axes were affected; one test each.
// ---------------------------------------------------------------------------
TEST(ProgressiveJpegResized, TallImageDoesNotRepeatRowsPastOverflow) {
  ProgressiveRows capture;
  // 339 rows at 1/8: the wrap began at output row 194.
  const auto result = decodeResized("progressive_tall.jpg", 8, 540, capture);
  ASSERT_EQ(result, ProgressiveJpeg::Result::Ok) << ProgressiveJpeg::resultName(result);
  ASSERT_EQ(capture.height, 540);

  // The source is a black-to-white gradient top to bottom, so the output has to keep
  // getting brighter all the way down. The bug froze it at the value of row ~193.
  const auto row = [&](int y) { return capture.pixels[static_cast<size_t>(y) * capture.width]; };
  EXPECT_LT(row(0), row(193));
  EXPECT_LT(row(193), row(300));
  EXPECT_LT(row(300), row(539));
  EXPECT_GT(row(539), 200);  // reaches white rather than freezing at ~36% grey
}

TEST(ProgressiveJpegResized, WideImageDoesNotRepeatColumnsPastOverflow) {
  ProgressiveRows capture;
  // 240 columns at 1/8: the wrap began at output column 275.
  const auto result = decodeResized("progressive_wide.jpg", 382, 8, capture);
  ASSERT_EQ(result, ProgressiveJpeg::Result::Ok) << ProgressiveJpeg::resultName(result);
  ASSERT_EQ(capture.width, 382);

  // Left-to-right gradient, read across the first output row.
  const auto col = [&](int x) { return capture.pixels[static_cast<size_t>(x)]; };
  EXPECT_LT(col(0), col(274));
  EXPECT_LT(col(274), col(330));
  EXPECT_LT(col(330), col(381));
  EXPECT_GT(col(381), 200);
}

TEST(ProgressiveJpegResized, RejectsBaselineWithoutEmittingRows) {
  ProgressiveRows capture;
  EXPECT_EQ(decodeResized("contrast_420.jpg", 48, 32, capture), ProgressiveJpeg::Result::Unsupported);
  EXPECT_TRUE(capture.pixels.empty());
}

// The resize starts from every block, the partial ones at the right and bottom edges too: a 9x5
// image is two levels side by side, not nothing (a picture with no whole block row at 1/8).
TEST(ProgressiveJpegResized, ResizesFromEveryBlockEvenAPartialOne) {
  ProgressiveRows capture;
  const auto result = decodeResized("prog_tiny_9x5.jpg", 18, 10, capture);
  ASSERT_EQ(result, ProgressiveJpeg::Result::Ok) << ProgressiveJpeg::resultName(result);
  ASSERT_EQ(capture.height, 10);
  EXPECT_NE(capture.pixels[0], capture.pixels[17]) << "the second, partial block has a level of its own";
}

// ---------------------------------------------------------------------------
// Bug 1 end-to-end: the thumbnail converter on an odd-width baseline JPEG that
// forces a 1/2 DCT pre-scale. Before the floor fix this wrote 0 rows and failed.
// ---------------------------------------------------------------------------
TEST(JpegToBmpConverter, OddDimensionThumbnailDecodesAllRows) {
  struct Case {
    const char* name;
    int width;
    int height;
  };
  // 35x53 -> target 15x22 forces 1/2 DCT (effSrc 17x26) then fine-scales down,
  // so needsScaling=true and the MCU-row completion path is exercised. The even-dimension
  // control, 96x64 -> 40x26 (1/2 DCT, effSrc 48x32), must keep working after the floor fix.
  for (const Case c : {Case{"odd_420.jpg", 15, 22}, Case{"contrast_420.jpg", 40, 26}}) {
    SCOPED_TRACE(c.name);
    FsFile f;
    ASSERT_TRUE(f.openForRead(fixture(c.name)));

    MemoryPrint out;
    const bool ok = JpegToBmpConverter::jpegFileToBmpStreamWithSize(f, out, c.width, c.height);
    f.close();

    ASSERT_TRUE(ok) << "converter failed (descale-width regression)";
    ASSERT_GE(out.buf.size(), 70u);
    ASSERT_EQ(out.buf[0], 'B');
    ASSERT_EQ(out.buf[1], 'M');
    EXPECT_EQ(le32(out.buf, 18), c.width);    // BMP width
    EXPECT_EQ(le32(out.buf, 22), -c.height);  // BMP height (top-down => negative)
  }
}

// Unpack a 2-bit BMP the converter wrote into levels 0..3, top row first.
static std::vector<uint8_t> unpack2BitBmp(const std::vector<uint8_t>& bmp, int& w, int& h) {
  w = le32(bmp, 18);
  h = -le32(bmp, 22);  // top-down BMPs carry a negative height
  const uint32_t offset = static_cast<uint32_t>(le32(bmp, 10));
  const int bytesPerRow = (w * 2 + 31) / 32 * 4;
  std::vector<uint8_t> px(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = bmp.data() + offset + static_cast<size_t>(y) * bytesPerRow;
    for (int x = 0; x < w; ++x) px[static_cast<size_t>(y) * w + x] = (row[x / 4] >> (6 - 2 * (x % 4))) & 3;
  }
  return px;
}

// A progressive cover used to be shown from its DC scan: 1/8 resolution, upscaled -- a 221x324
// cover became a 27x40 smear. It is now decoded in full, so the same picture encoded progressive
// and baseline must come out of the converter alike (only IDCT rounding and the dither's
// response to it may differ).
TEST(JpegToBmpConverter, ProgressiveThumbnailMatchesItsBaselineTwin) {
  auto convert = [](const char* name) {
    FsFile file;
    EXPECT_TRUE(file.openForRead(fixture(name)));
    MemoryPrint out;
    EXPECT_TRUE(JpegToBmpConverter::jpegFileToBmpStreamWithSize(file, out, 203, 141));
    file.close();
    return out.buf;
  };
  int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
  const auto prog = unpack2BitBmp(convert("prog_full_420.jpg"), w1, h1);
  const auto base = unpack2BitBmp(convert("prog_full_420_base.jpg"), w2, h2);
  ASSERT_EQ(w1, 203);
  ASSERT_EQ(h1, 141);
  ASSERT_EQ(w2, w1);
  ASSERT_EQ(h2, h1);

  size_t same = 0;
  long diff = 0;
  for (size_t i = 0; i < prog.size(); ++i) {
    const int d = std::abs(static_cast<int>(prog[i]) - static_cast<int>(base[i]));
    same += d == 0;
    diff += d;
  }
  const double identical = static_cast<double>(same) / prog.size();
  const double meanDiff = static_cast<double>(diff) / prog.size();
  // Measured: full decode 0.877 identical / 0.125 mean level difference (the dither reacts to
  // +-1 gray differences on a noisy picture); the DC preview scored 0.586 / 0.521.
  EXPECT_GT(identical, 0.80);
  EXPECT_LT(meanDiff, 0.20);
}

// progressive_420.jpg is a left-half gradient beside a black upper-right and a white lower-right
// quadrant (fixtures/generate_fixtures.py), so every row down to the last one has to carry the
// picture, not just the header announce it.
TEST(JpegToBmpConverter, ProgressiveThumbnailDecodesAllRows) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("progressive_420.jpg")));

  MemoryPrint out;
  const bool ok = JpegToBmpConverter::jpegFileToBmpStreamWithSize(file, out, 40, 26);
  file.close();

  ASSERT_TRUE(ok);
  ASSERT_GE(out.buf.size(), 70u);
  EXPECT_EQ(le32(out.buf, 18), 40);
  EXPECT_EQ(le32(out.buf, 22), -26);
  const int bytesPerRow = (40 * 2 + 31) / 32 * 4;
  ASSERT_EQ(out.buf.size(), static_cast<size_t>(le32(out.buf, 10)) + 26u * bytesPerRow) << "every row written";

  int w = 0, h = 0;
  const auto px = unpack2BitBmp(out.buf, w, h);
  const auto at = [&](const int x, const int y) { return px[static_cast<size_t>(y) * w + x]; };
  // Right half, clear of the quadrant edge (row 13) and the gradient's end (column ~20).
  for (int x = 24; x < w; ++x) {
    for (int y = 0; y < 12; ++y) EXPECT_EQ(at(x, y), 0) << "black quadrant at " << x << "," << y;
    for (int y = 14; y < h; ++y) EXPECT_EQ(at(x, y), 3) << "white quadrant at " << x << "," << y;
  }
  // Left half: dark to light, on the first row and the last.
  for (const int y : {0, h - 1}) {
    EXPECT_LT(at(1, y) + at(2, y), at(17, y) + at(18, y)) << "gradient on row " << y;
  }
}

// ---------------------------------------------------------------------------
// Crop mode must emit EXACTLY the requested dimensions, whichever axis overfills.
// crop=true scales by the larger fit factor, so a source whose aspect differs from
// the target overfills one dimension; older builds wrote that overfill into the
// file (e.g. a 340x561 BMP in thumb_340x540.bmp). The home themes draw thumbs 1:1,
// so the mismatch forced GfxRenderer::drawBitmap to rescale the already-dithered
// 1-bit image — aliasing the dither into a visible grid on covers. The converter
// now center-crops to the exact target.
// ---------------------------------------------------------------------------
TEST(JpegToBmpConverter, CropModeEmitsExactTargetWidthOverfill) {
  FsFile f;
  ASSERT_TRUE(f.openForRead(fixture("contrast_420.jpg")));

  MemoryPrint out;
  // 96x64 landscape into a 20x40 portrait box: scale = max(20/96, 40/64) = 0.625
  // -> scaled 60x40, width overfills (60 > 20) and must be center-cropped away.
  const bool ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(f, out, 20, 40);
  f.close();

  ASSERT_TRUE(ok);
  ASSERT_GE(out.buf.size(), 62u);
  EXPECT_EQ(le32(out.buf, 18), 20);   // exact target width, no overfill
  EXPECT_EQ(le32(out.buf, 22), -40);  // exact target height (top-down => negative)
}

TEST(JpegToBmpConverter, CropModeEmitsExactTargetHeightOverfill) {
  FsFile f;
  ASSERT_TRUE(f.openForRead(fixture("contrast_420.jpg")));

  MemoryPrint out;
  // 96x64 into a 30x10 box: scale = max(30/96, 10/64) = 0.3125 -> scaled 30x20,
  // height overfills (20 > 10) — the vertical-overfill case observed on-device.
  const bool ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(f, out, 30, 10);
  f.close();

  ASSERT_TRUE(ok);
  ASSERT_GE(out.buf.size(), 62u);
  EXPECT_EQ(le32(out.buf, 18), 30);
  EXPECT_EQ(le32(out.buf, 22), -10);
}

// Fit mode (crop=false), the cover grids' thumbnail: the WHOLE image inside the box, scaled by the
// smaller factor, so one side is the box's and the other at most -- the opposite of the crop above.
// A box that cropped a third off every ordinary cover is what this was added for.
TEST(JpegToBmpConverter, FitModeKeepsTheWholeImageInsideTheBox) {
  FsFile f;
  ASSERT_TRUE(f.openForRead(fixture("contrast_420.jpg")));

  MemoryPrint out;
  // 96x64 landscape into a 20x40 portrait box: scale = min(20/96, 40/64) = 0.2083 -> 20x13.
  const bool ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(f, out, 20, 40, nullptr, /*crop=*/false);
  f.close();

  ASSERT_TRUE(ok);
  ASSERT_GE(out.buf.size(), 62u);
  EXPECT_EQ(le32(out.buf, 18), 20);           // the width is the box's
  const int32_t height = -le32(out.buf, 22);  // top-down => negative
  EXPECT_LE(height, 40);                      // nothing past the box
  EXPECT_NEAR(height, 13, 1);                 // and the aspect kept: nothing cropped
}

TEST(JpegToBmpConverter, FitModeIsHeightBoundInAWideBox) {
  FsFile f;
  ASSERT_TRUE(f.openForRead(fixture("contrast_420.jpg")));

  MemoryPrint out;
  // 96x64 into a 30x10 box: scale = min(30/96, 10/64) = 0.15625 -> 15x10. Height bound.
  const bool ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(f, out, 30, 10, nullptr, /*crop=*/false);
  f.close();

  ASSERT_TRUE(ok);
  ASSERT_GE(out.buf.size(), 62u);
  EXPECT_NEAR(le32(out.buf, 18), 15, 1);
  EXPECT_EQ(le32(out.buf, 22), -10);
}

// ---------------------------------------------------------------------------
// One decode, two thumbnails (memory audit 2026-09, R9 item 2): the Lyra carousel needs a 340x540
// and a 200x390 thumb of every cover, and each used to be a separate full decode of the JPEG. The
// multi-target entry decodes once, at the DCT scale the LARGEST target needs, and feeds every
// target its own resampler and ditherer from the same source rows.
//  - The largest target's BMP must be byte-identical to a single conversion of that size: same
//    decode scale, same resampling, same dither.
//  - A smaller target resamples from the larger one's finer source rows (a single conversion
//    would have picked a coarser DCT scale), so its bits differ; it must still be a complete BMP
//    of exactly its box, and the same picture: compared on 4x4 block darkness, not per pixel,
//    because error diffusion reacts to +-1 gray differences.
// ---------------------------------------------------------------------------
namespace {

// Unpacks a top-down 1-bit BMP into one byte per pixel (0/1).
std::vector<uint8_t> unpack1BitBmp(const std::vector<uint8_t>& bmp, int& width, int& height) {
  width = le32(bmp, 18);
  height = std::abs(le32(bmp, 22));
  const uint32_t dataOffset = static_cast<uint32_t>(le32(bmp, 10));
  const int stride = ((width + 31) / 32) * 4;
  std::vector<uint8_t> px(static_cast<size_t>(width) * height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t byte = dataOffset + static_cast<size_t>(y) * stride + x / 8;
      if (byte >= bmp.size()) return {};
      px[static_cast<size_t>(y) * width + x] = (bmp[byte] >> (7 - (x % 8))) & 1;
    }
  }
  return px;
}

std::vector<uint8_t> convertSingle1Bit(const char* name, const int w, const int h) {
  FsFile file;
  EXPECT_TRUE(file.openForRead(fixture(name)));
  MemoryPrint out;
  EXPECT_TRUE(JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(file, out, w, h));
  file.close();
  return out.buf;
}

// Mean absolute difference of 4x4 block means, in [0, 1].
double blockMeanDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const int w, const int h) {
  double total = 0;
  int blocks = 0;
  for (int by = 0; by + 4 <= h; by += 4) {
    for (int bx = 0; bx + 4 <= w; bx += 4) {
      int sa = 0, sb = 0;
      for (int y = by; y < by + 4; ++y) {
        for (int x = bx; x < bx + 4; ++x) {
          sa += a[static_cast<size_t>(y) * w + x];
          sb += b[static_cast<size_t>(y) * w + x];
        }
      }
      total += std::abs(sa - sb) / 16.0;
      ++blocks;
    }
  }
  return blocks > 0 ? total / blocks : 1.0;
}

void expectTwoTargetsFromOneDecode(const char* name) {
  SCOPED_TRACE(name);
  constexpr int kLargeW = 90, kLargeH = 60;  // needs the 1/2 DCT scale on a 203x141 source
  constexpr int kSmallW = 40, kSmallH = 28;  // alone it would take 1/4

  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture(name)));
  MemoryPrint large;
  MemoryPrint small;
  const JpegToBmpConverter::BmpTarget targets[2] = {{&large, kLargeW, kLargeH}, {&small, kSmallW, kSmallH}};
  ASSERT_TRUE(JpegToBmpConverter::jpegFileTo1BitBmpStreamsWithSizes(file, targets, 2));
  file.close();

  EXPECT_EQ(large.buf, convertSingle1Bit(name, kLargeW, kLargeH)) << "largest target must match a lone conversion";

  int w = 0, h = 0, sw = 0, sh = 0;
  const auto smallPx = unpack1BitBmp(small.buf, w, h);
  const auto soloPx = unpack1BitBmp(convertSingle1Bit(name, kSmallW, kSmallH), sw, sh);
  ASSERT_EQ(w, kSmallW);
  ASSERT_EQ(h, kSmallH);
  ASSERT_EQ(sw, w);
  ASSERT_EQ(sh, h);
  ASSERT_EQ(smallPx.size(), static_cast<size_t>(w) * h);  // every row present
  EXPECT_LT(blockMeanDifference(smallPx, soloPx, w, h), 0.15);
}

}  // namespace

TEST(JpegToBmpConverter, TwoTargetsFromOneBaselineDecode) { expectTwoTargetsFromOneDecode("prog_full_420_base.jpg"); }

TEST(JpegToBmpConverter, TwoTargetsFromOneProgressiveDecode) { expectTwoTargetsFromOneDecode("prog_full_420.jpg"); }

// The order of the targets does not matter: the decode scale follows the largest wherever it sits.
TEST(JpegToBmpConverter, TwoTargetsSmallFirstStillDecodesAtTheLargestScale) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("prog_full_420.jpg")));
  MemoryPrint small;
  MemoryPrint large;
  const JpegToBmpConverter::BmpTarget targets[2] = {{&small, 40, 28}, {&large, 90, 60}};
  ASSERT_TRUE(JpegToBmpConverter::jpegFileTo1BitBmpStreamsWithSizes(file, targets, 2));
  file.close();
  EXPECT_EQ(large.buf, convertSingle1Bit("prog_full_420.jpg", 90, 60));
}

TEST(JpegToBmpConverter, TwoTargetsRejectsBadArguments) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("prog_full_420.jpg")));
  MemoryPrint out;
  const JpegToBmpConverter::BmpTarget three[3] = {{&out, 10, 10}, {&out, 10, 10}, {&out, 10, 10}};
  EXPECT_FALSE(JpegToBmpConverter::jpegFileTo1BitBmpStreamsWithSizes(file, three, 3));
  EXPECT_FALSE(JpegToBmpConverter::jpegFileTo1BitBmpStreamsWithSizes(file, nullptr, 1));
  const JpegToBmpConverter::BmpTarget noSink[1] = {{nullptr, 10, 10}};
  EXPECT_FALSE(JpegToBmpConverter::jpegFileTo1BitBmpStreamsWithSizes(file, noSink, 1));
  file.close();
}

// ---------------------------------------------------------------------------
// JpegThumbSession (memory audit 2026-09, R9 item 3): Home's cover pass converts a cover a few
// units at a time and checks for input in between. However the work is sliced, and whatever the
// caller does with the file between slices, the BMPs must be the one-shot conversion's.
// ---------------------------------------------------------------------------
namespace {

struct SessionRun {
  std::vector<uint8_t> large;
  std::vector<uint8_t> small;
  int calls = 0;
  JpegThumbSession::Status status = JpegThumbSession::Status::Error;
  bool started = false;
  bool progressive = false;
};

SessionRun runSession(const char* name, const uint16_t units, BuildArena* scratch = nullptr) {
  SessionRun run;
  FsFile file;
  EXPECT_TRUE(file.openForRead(fixture(name)));
  MemoryPrint large;
  MemoryPrint small;
  const JpegToBmpConverter::BmpTarget targets[2] = {{&large, 90, 60}, {&small, 40, 28}};
  {
    auto session = JpegThumbSession::begin(file, targets, 2, scratch);
    run.started = session != nullptr;
    if (!session) return run;
    run.progressive = session->progressive();
    do {
      run.status = session->continueSteps(units);
      ++run.calls;
      uint8_t junk[5];  // the caller reads elsewhere in the file between slices
      file.seek(1);
      file.read(junk, sizeof(junk));
    } while (run.status == JpegThumbSession::Status::Running && run.calls < 100000);
  }
  file.close();
  run.large = large.buf;
  run.small = small.buf;
  return run;
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>> convertTwoOneShot(const char* name) {
  FsFile file;
  EXPECT_TRUE(file.openForRead(fixture(name)));
  MemoryPrint large;
  MemoryPrint small;
  const JpegToBmpConverter::BmpTarget targets[2] = {{&large, 90, 60}, {&small, 40, 28}};
  EXPECT_TRUE(JpegToBmpConverter::jpegFileTo1BitBmpStreamsWithSizes(file, targets, 2));
  file.close();
  return {large.buf, small.buf};
}

void expectSessionMatchesOneShot(const char* name, const bool progressive) {
  SCOPED_TRACE(name);
  const auto oneShot = convertTwoOneShot(name);
  ASSERT_FALSE(oneShot.first.empty());
  for (const uint16_t units : {uint16_t{1}, uint16_t{7}, uint16_t{UINT16_MAX}}) {
    const SessionRun run = runSession(name, units);
    ASSERT_TRUE(run.started) << "units=" << units;
    EXPECT_EQ(run.progressive, progressive);
    EXPECT_EQ(run.status, JpegThumbSession::Status::Done) << "units=" << units;
    EXPECT_EQ(run.large, oneShot.first) << "units=" << units;
    EXPECT_EQ(run.small, oneShot.second) << "units=" << units;
    if (units == 1) {
      EXPECT_GT(run.calls, 4) << "one unit must not finish the image";
    }
  }
}

}  // namespace

TEST(JpegThumbSession, BaselineSlicesMatchTheOneShotConversion) {
  expectSessionMatchesOneShot("prog_full_420_base.jpg", false);
}

TEST(JpegThumbSession, ProgressiveSlicesMatchTheOneShotConversion) {
  expectSessionMatchesOneShot("prog_full_420.jpg", true);
}

TEST(JpegThumbSession, OddDimensionBaselineCompletes) {
  const auto oneShot = convertTwoOneShot("odd_420.jpg");
  const SessionRun run = runSession("odd_420.jpg", 1);
  ASSERT_TRUE(run.started);
  EXPECT_EQ(run.status, JpegThumbSession::Status::Done);
  EXPECT_EQ(run.large, oneShot.first);
  EXPECT_EQ(run.small, oneShot.second);
}

// In a lent region the session holds its blocks across slices and gives every byte back when it
// goes -- Home returns the region to the display right after.
TEST(JpegThumbSession, HoldsItsRegionBlocksUntilItGoes) {
  for (const char* name : {"prog_full_420_base.jpg", "prog_full_420.jpg"}) {
    SCOPED_TRACE(name);
    const auto oneShot = convertTwoOneShot(name);
    BuildArena arena(64 * 1024);
    ASSERT_TRUE(arena.valid());
    FsFile file;
    ASSERT_TRUE(file.openForRead(fixture(name)));
    MemoryPrint large;
    MemoryPrint small;
    const JpegToBmpConverter::BmpTarget targets[2] = {{&large, 90, 60}, {&small, 40, 28}};
    {
      auto session = JpegThumbSession::begin(file, targets, 2, &arena);
      ASSERT_NE(session, nullptr);
      EXPECT_GT(arena.used(), 0u) << "decoder memory and row pipeline come from the region";
      const size_t held = arena.used();
      auto status = JpegThumbSession::Status::Running;
      while (status == JpegThumbSession::Status::Running) {
        status = session->continueSteps(2);
        EXPECT_EQ(arena.used(), held) << "nothing is taken or given back between slices";
      }
      EXPECT_EQ(status, JpegThumbSession::Status::Done);
      EXPECT_EQ(session->continueSteps(1), JpegThumbSession::Status::Done) << "a finished session stays finished";
    }
    EXPECT_EQ(arena.used(), 0u);
    file.close();
    EXPECT_EQ(large.buf, oneShot.first);
    EXPECT_EQ(small.buf, oneShot.second);
  }
}

// Home's progressive covers read through the decoder's windows (ProgressiveJpeg::ReadWindows)
// when the lent region has room left after the workspace and the row pipeline, as the reader's
// pictures do: through one shared 512-byte buffer every scan re-read its data in every band. The
// BMPs are the ones a decode without the windows writes, the one-shot and the sliced alike, and the
// region comes back empty. Without a region there are no windows: the heap is never asked for them.
namespace {

struct CoverReads {
  std::vector<uint8_t> large;
  std::vector<uint8_t> small;
  size_t calls = 0;
  size_t bytes = 0;
};

CoverReads convertCover(const char* name, BuildArena* scratch, const bool sliced) {
  CoverReads out;
  FsFile file;
  EXPECT_TRUE(file.openForRead(fixture(name)));
  MemoryPrint large;
  MemoryPrint small;
  const JpegToBmpConverter::BmpTarget targets[2] = {{&large, 90, 60}, {&small, 40, 28}};
  const size_t calls = FsFile::calls;
  const size_t bytes = FsFile::bytesRead;
  if (sliced) {
    auto session = JpegThumbSession::begin(file, targets, 2, scratch);
    EXPECT_NE(session, nullptr);
    if (session) {
      auto status = JpegThumbSession::Status::Running;
      while (status == JpegThumbSession::Status::Running) status = session->continueSteps(3);
      EXPECT_EQ(status, JpegThumbSession::Status::Done);
    }
  } else {
    EXPECT_TRUE(JpegToBmpConverter::jpegFileTo1BitBmpStreamsWithSizes(file, targets, 2, scratch));
  }
  out.calls = FsFile::calls - calls;
  out.bytes = FsFile::bytesRead - bytes;
  file.close();
  out.large = large.buf;
  out.small = small.buf;
  return out;
}

}  // namespace

TEST(JpegToBmpConverter, AProgressiveCoverReadsThroughWindowsFromItsRegion) {
  for (const bool sliced : {false, true}) {
    SCOPED_TRACE(sliced ? "sliced (JpegThumbSession)" : "one-shot");
    const CoverReads heap = convertCover("prog_full_420.jpg", nullptr, sliced);
    BuildArena arena(64 * 1024);
    ASSERT_TRUE(arena.valid());
    const CoverReads lent = convertCover("prog_full_420.jpg", &arena, sliced);
    EXPECT_EQ(arena.used(), 0u);
    EXPECT_GE(arena.highWater(), ProgressiveJpeg::scanWindowBytes()) << "the windows came out of the region";
    ASSERT_FALSE(heap.large.empty());
    EXPECT_EQ(lent.large, heap.large);
    EXPECT_EQ(lent.small, heap.small);
    std::printf("[ thumbs   ] %s: %zu card calls / %zu bytes without a region, %zu / %zu lent\n",
                sliced ? "sliced" : "one-shot", heap.calls, heap.bytes, lent.calls, lent.bytes);
    EXPECT_LE(lent.calls * 2, heap.calls);
    EXPECT_LT(lent.bytes, heap.bytes);
  }
}

// A region with room for the workspace and the row pipeline but not for the windows: the decode runs
// as before, in the region, and the heap is not asked for the windows.
TEST(JpegToBmpConverter, AProgressiveCoverWithoutRoomForTheWindowsDecodesAsBefore) {
  FsFile probeFile;
  ASSERT_TRUE(probeFile.openForRead(fixture("prog_full_420.jpg")));
  ProgressiveJpeg::ImageInfo info;
  ASSERT_EQ(ProgressiveJpeg::probe(probeFile, info), ProgressiveJpeg::Result::Ok);
  probeFile.close();
  const CoverReads heap = convertCover("prog_full_420.jpg", nullptr, false);
  // Find a region size the whole decode fits in, then take away less than the windows need.
  BuildArena roomy(64 * 1024);
  (void)convertCover("prog_full_420.jpg", &roomy, false);
  const size_t withWindows = roomy.highWater();
  BuildArena tight(withWindows - ProgressiveJpeg::scanWindowBytes() + 64);
  ASSERT_TRUE(tight.valid());
  const CoverReads squeezed = convertCover("prog_full_420.jpg", &tight, false);
  EXPECT_EQ(tight.used(), 0u);
  EXPECT_LT(tight.highWater(), withWindows) << "no windows";
  EXPECT_EQ(squeezed.large, heap.large);
  EXPECT_EQ(squeezed.small, heap.small);
  EXPECT_EQ(squeezed.bytes, heap.bytes) << "read as without a region";
}

// Home leaves mid-cover (a book is opened): the session goes unfinished and must leave the
// region empty so it can go back to the display.
TEST(JpegThumbSession, AbandonedMidDecodeReleasesEverything) {
  for (const char* name : {"prog_full_420_base.jpg", "prog_full_420.jpg"}) {
    SCOPED_TRACE(name);
    BuildArena arena(64 * 1024);
    FsFile file;
    ASSERT_TRUE(file.openForRead(fixture(name)));
    MemoryPrint large;
    MemoryPrint small;
    const JpegToBmpConverter::BmpTarget targets[2] = {{&large, 90, 60}, {&small, 40, 28}};
    {
      auto session = JpegThumbSession::begin(file, targets, 2, &arena);
      ASSERT_NE(session, nullptr);
      for (int i = 0; i < 3; ++i) EXPECT_EQ(session->continueSteps(1), JpegThumbSession::Status::Running);
    }
    EXPECT_EQ(arena.used(), 0u);
    file.close();
  }
}

// Restores the stubbed free heap even when an ASSERT_* returns early, so the pressure cannot leak
// into the tests that run after this one.
class FreeHeapScope {
 public:
  explicit FreeHeapScope(const uint32_t bytes) : saved_(ESP.getFreeHeap()) { ESP.setFreeHeap(bytes); }
  ~FreeHeapScope() { ESP.setFreeHeap(saved_); }
  FreeHeapScope(const FreeHeapScope&) = delete;
  FreeHeapScope& operator=(const FreeHeapScope&) = delete;

 private:
  const uint32_t saved_;
};

// A progressive image only the DC preview takes is not sliced; the caller converts it one-shot.
// The host heap stub fits every scale's workspace, so the refusal only happens with the heap one
// byte short of the smallest (1/8) workspace plus the 16 KB the row pipeline is owed.
TEST(JpegThumbSession, RefusesWhatOnlyTheDcPreviewTakes) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("progressive_420.jpg")));
  ProgressiveJpeg::ImageInfo info;
  ASSERT_EQ(ProgressiveJpeg::probe(file, info), ProgressiveJpeg::Result::Ok) << "the full decoder takes it";
  MemoryPrint out;
  const JpegToBmpConverter::BmpTarget target[1] = {{&out, 40, 28}};
  {
    FreeHeapScope squeezed(static_cast<uint32_t>(ProgressiveJpeg::workspaceBytes(info, 3) + 16 * 1024 - 1));
    EXPECT_EQ(JpegThumbSession::begin(file, target, 1), nullptr);
    // ...and the one-shot conversion the caller falls back to still makes the thumbnail.
    ASSERT_TRUE(file.seek(0));
    ASSERT_TRUE(JpegToBmpConverter::jpegFileTo1BitBmpStreamsWithSizes(file, target, 1));
    ASSERT_GE(out.buf.size(), 26u);
    EXPECT_EQ(le32(out.buf, 18), 40);
    EXPECT_EQ(le32(out.buf, 22), -28);
  }
  // Control: with the heap back, the same image is sliced.
  ASSERT_TRUE(file.seek(0));
  MemoryPrint roomy;
  const JpegToBmpConverter::BmpTarget roomyTarget[1] = {{&roomy, 40, 28}};
  EXPECT_NE(JpegThumbSession::begin(file, roomyTarget, 1), nullptr);
  file.close();
}

// ---------------------------------------------------------------------------
// A cover whose decode fails partway -- a baseline file cut short -- or finishes short of what the
// file should hold (a progressive one cut in its last scan: ProgressiveJpeg's Degraded) keeps the
// rows decoded so far, and the rest of the BMP is white. It is still a complete BMP of its box, kept
// like any other (JpegToBmpConverter.h says why): before, such a cover failed and the book ended up
// with no cover at all. A decode that fails before its first row still fails.
// ---------------------------------------------------------------------------
namespace {

std::vector<uint8_t> fixtureBytes(const char* name) {
  std::vector<uint8_t> bytes;
  FILE* fp = fopen(fixture(name).c_str(), "rb");
  EXPECT_NE(fp, nullptr);
  if (!fp) return bytes;
  int c;
  while ((c = fgetc(fp)) != EOF) bytes.push_back(static_cast<uint8_t>(c));
  fclose(fp);
  return bytes;
}

// The first `length` bytes of a fixture, as a temporary file.
std::string cutCopy(const char* name, const size_t length) {
  const std::vector<uint8_t> whole = fixtureBytes(name);
  const std::string path = (std::filesystem::temp_directory_path() / (std::string("jpgbmp_cut_") + name)).string();
  FILE* fp = fopen(path.c_str(), "wb");
  EXPECT_NE(fp, nullptr);
  if (fp) {
    fwrite(whole.data(), 1, std::min(length, whole.size()), fp);
    fclose(fp);
  }
  return path;
}

// Where the first scan's entropy data starts.
size_t firstScanData(const std::vector<uint8_t>& d) {
  for (size_t i = 2; i + 3 < d.size();) {
    if (d[i] != 0xFF) return 0;
    const size_t length = (static_cast<size_t>(d[i + 2]) << 8) | d[i + 3];
    if (d[i + 1] == 0xDA) return i + 2 + length;
    i += 2 + length;
  }
  return 0;
}

struct PartialCover {
  bool ok = false;
  std::vector<uint8_t> px;  // 1-bit pixels, 1 = white
  int w = 0, h = 0;
};

PartialCover convertPath(const std::string& path, const bool sliced) {
  PartialCover out;
  FsFile file;
  EXPECT_TRUE(file.openForRead(path));
  MemoryPrint bmp;
  const JpegToBmpConverter::BmpTarget target[1] = {{&bmp, 90, 60}};
  if (sliced) {
    auto session = JpegThumbSession::begin(file, target, 1);
    EXPECT_NE(session, nullptr);
    if (session) {
      auto status = JpegThumbSession::Status::Running;
      while (status == JpegThumbSession::Status::Running) status = session->continueSteps(2);
      out.ok = status == JpegThumbSession::Status::Done;
    }
  } else {
    out.ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamsWithSizes(file, target, 1);
  }
  file.close();
  if (out.ok) out.px = unpack1BitBmp(bmp.buf, out.w, out.h);
  return out;
}

// Rows from the top that two pictures share.
int sameLeadingRows(const PartialCover& a, const PartialCover& b) {
  int rows = 0;
  while (rows < a.h &&
         std::equal(a.px.begin() + rows * a.w, a.px.begin() + (rows + 1) * a.w, b.px.begin() + rows * b.w))
    ++rows;
  return rows;
}

void expectPartialCover(const char* name, const size_t cutAt, const bool bottomWhite) {
  const PartialCover clean = convertPath(fixture(name), false);
  ASSERT_TRUE(clean.ok);
  const std::string cut = cutCopy(name, cutAt);
  for (const bool sliced : {false, true}) {
    SCOPED_TRACE(std::string(name) + (sliced ? ", sliced" : ", one-shot"));
    const PartialCover partial = convertPath(cut, sliced);
    ASSERT_TRUE(partial.ok) << "the rows so far make a cover";
    ASSERT_EQ(partial.w, 90);
    ASSERT_EQ(partial.h, 60);
    const int same = sameLeadingRows(partial, clean);
    EXPECT_GE(same, 10) << "the rows decoded before the failure are the clean cover's";
    EXPECT_LT(same, partial.h) << "the cut must cost something, or this test proves nothing";
    if (bottomWhite) {
      const auto last = partial.px.begin() + (partial.h - 1) * partial.w;
      EXPECT_TRUE(std::all_of(last, last + partial.w, [](const uint8_t v) { return v == 1; })) << "the rest is white";
    }
  }
  std::remove(cut.c_str());
}

}  // namespace

TEST(JpegToBmpConverter, ABaselineCoverCutShortKeepsItsTopRows) {
  expectPartialCover("prog_full_420_base.jpg", fixtureBytes("prog_full_420_base.jpg").size() / 2, true);
}

TEST(JpegToBmpConverter, AProgressiveCoverCutInItsLastScanKeepsEveryRow) {
  // prog_420_cut.jpg's cut: every row is decoded, and below the cut the last refinement is missing.
  expectPartialCover("prog_full_420.jpg", fixtureBytes("prog_420_cut.jpg").size(), false);
}

TEST(JpegToBmpConverter, ACoverThatFailsBeforeItsFirstRowStillFails) {
  const std::vector<uint8_t> whole = fixtureBytes("prog_full_420_base.jpg");
  const size_t data = firstScanData(whole);
  ASSERT_GT(data, 0u);
  const std::string cut = cutCopy("prog_full_420_base.jpg", data + 16);
  EXPECT_FALSE(convertPath(cut, false).ok);
  EXPECT_FALSE(convertPath(cut, true).ok);
  std::remove(cut.c_str());
}

TEST(JpegThumbSession, RejectsBadArguments) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("prog_full_420.jpg")));
  MemoryPrint out;
  const JpegToBmpConverter::BmpTarget three[3] = {{&out, 10, 10}, {&out, 10, 10}, {&out, 10, 10}};
  EXPECT_EQ(JpegThumbSession::begin(file, three, 3), nullptr);
  EXPECT_EQ(JpegThumbSession::begin(file, nullptr, 1), nullptr);
  const JpegToBmpConverter::BmpTarget noSink[1] = {{nullptr, 10, 10}};
  EXPECT_EQ(JpegThumbSession::begin(file, noSink, 1), nullptr);
  file.close();
}
