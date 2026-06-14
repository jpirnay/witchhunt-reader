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

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "HalStorage.h"  // FsFile (stdio-backed shim)
#include "JpegToBmpConverter.h"
#include "Print.h"

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

}  // namespace

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

// ---------------------------------------------------------------------------
// Bug 1 end-to-end: the thumbnail converter on an odd-width baseline JPEG that
// forces a 1/2 DCT pre-scale. Before the floor fix this wrote 0 rows and failed.
// ---------------------------------------------------------------------------
TEST(JpegToBmpConverter, OddDimensionThumbnailDecodesAllRows) {
  FsFile f;
  ASSERT_TRUE(f.openForRead(fixture("odd_420.jpg")));

  MemoryPrint out;
  // 35x53 -> target 15x22 forces 1/2 DCT (effSrc 17x26) then fine-scales down,
  // so needsScaling=true and the MCU-row completion path is exercised.
  const bool ok = JpegToBmpConverter::jpegFileToBmpStreamWithSize(f, out, 15, 22);
  f.close();

  ASSERT_TRUE(ok) << "converter failed on odd-dimension cover (descale-width regression)";
  ASSERT_GE(out.buf.size(), 70u);
  ASSERT_EQ(out.buf[0], 'B');
  ASSERT_EQ(out.buf[1], 'M');
  EXPECT_EQ(le32(out.buf, 18), 15);   // BMP width
  EXPECT_EQ(le32(out.buf, 22), -22);  // BMP height (top-down => negative)
}

// Even-dimension control: the same path must keep working after the floor fix.
TEST(JpegToBmpConverter, EvenDimensionThumbnailDecodesAllRows) {
  FsFile f;
  ASSERT_TRUE(f.openForRead(fixture("contrast_420.jpg")));

  MemoryPrint out;
  // 96x64 -> target 40x26 forces 1/2 DCT (effSrc 48x32) then fine-scales down.
  const bool ok = JpegToBmpConverter::jpegFileToBmpStreamWithSize(f, out, 40, 26);
  f.close();

  ASSERT_TRUE(ok);
  ASSERT_GE(out.buf.size(), 70u);
  EXPECT_EQ(le32(out.buf, 18), 40);
  EXPECT_EQ(le32(out.buf, 22), -26);
}
