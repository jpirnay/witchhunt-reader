// Full progressive JPEG decoding (lib/ProgressiveJpeg).
//
// The DC-only fallback shows a progressive JPEG at 1/8 resolution, upscaled: text in a diagram
// becomes mush. ProgressiveJpeg decodes every scan, band by band, and must reproduce libjpeg's
// own luma -- at 1/1 exactly (up to IDCT rounding), and at 1/2..1/8 as the reduced-size IDCT of
// the same blocks.
//
// Fixtures (fixtures/generate_fixtures.py) use libjpeg's successive-approximation scan script:
// interleaved DC first + refinement, spectral-selection AC first scans with Al > 0, and AC
// refinement scans -- every decode path. One of them adds restart markers every 3 units.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "HalStorage.h"  // FsFile (stdio-backed shim)
#include "ProgressiveJpeg.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "."
#endif

namespace {

std::string fixture(const std::string& name) { return std::string(FIXTURE_DIR) + "/" + name; }

struct Gray {
  int w = 0;
  int h = 0;
  std::vector<uint8_t> px;
  int at(const int x, const int y) const { return px[static_cast<size_t>(y) * w + x]; }
};

Gray loadPgm(const std::string& path) {
  Gray out;
  FILE* fp = fopen(path.c_str(), "rb");
  EXPECT_NE(fp, nullptr) << path;
  if (!fp) return out;
  char magic[3] = {};
  int maxval = 0;
  if (fscanf(fp, "%2s %d %d %d", magic, &out.w, &out.h, &maxval) == 4 && std::string(magic) == "P5" && maxval == 255) {
    fgetc(fp);
    out.px.resize(static_cast<size_t>(out.w) * out.h);
    EXPECT_EQ(fread(out.px.data(), 1, out.px.size(), fp), out.px.size());
  }
  fclose(fp);
  return out;
}

// What a 1/2^s decode should equal, derived from libjpeg's full-resolution luma: each 8x8 block
// forward-transformed, its top-left n x n coefficients inverted with the n-point IDCT (n = 8 >> s),
// over the floor(w >> s) x floor(h >> s) output. Edge blocks are padded by replicating the last
// row/column, which is what the encoder did.
// `wholeBlocks`: every block, the partial ones at the right and bottom edges too.
Gray reducedReference(const Gray& full, const int shift, const bool wholeBlocks = false) {
  const int n = 8 >> shift;
  const double pi = 3.14159265358979323846;
  auto c = [](const int k) { return k == 0 ? 0.70710678118654752 : 1.0; };
  Gray out;
  out.w = wholeBlocks ? (full.w + 7) / 8 * n : full.w >> shift;
  out.h = wholeBlocks ? (full.h + 7) / 8 * n : full.h >> shift;
  out.px.resize(static_cast<size_t>(out.w) * out.h);
  for (int by = 0; by * 8 < full.h; ++by) {
    for (int bx = 0; bx * 8 < full.w; ++bx) {
      double f[8][8];
      for (int v = 0; v < n; ++v) {
        for (int u = 0; u < n; ++u) {
          double sum = 0;
          for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
              const int p = full.at(std::min(bx * 8 + x, full.w - 1), std::min(by * 8 + y, full.h - 1)) - 128;
              sum += p * std::cos((2 * x + 1) * u * pi / 16) * std::cos((2 * y + 1) * v * pi / 16);
            }
          }
          f[v][u] = c(u) * c(v) / 4 * sum;
        }
      }
      for (int Y = 0; Y < n; ++Y) {
        for (int X = 0; X < n; ++X) {
          const int ox = bx * n + X;
          const int oy = by * n + Y;
          if (ox >= out.w || oy >= out.h) continue;
          double value = 128;
          for (int v = 0; v < n; ++v) {
            for (int u = 0; u < n; ++u) {
              value += c(u) / 2 * c(v) / 2 * f[v][u] * std::cos((2 * X + 1) * u * pi / (2 * n)) *
                       std::cos((2 * Y + 1) * v * pi / (2 * n));
            }
          }
          out.px[static_cast<size_t>(oy) * out.w + ox] =
              static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(value)), 0, 255));
        }
      }
    }
  }
  return out;
}

struct Capture {
  Gray image;
  int bands = 0;
  bool orderOk = true;
  static bool accept(void* user, uint16_t y, const uint8_t* gray, uint16_t width, uint16_t rows, uint16_t stride) {
    auto& c = *static_cast<Capture*>(user);
    if (y != c.image.h || (c.image.w != 0 && c.image.w != width) || stride < width) c.orderOk = false;
    c.image.w = width;
    for (uint16_t r = 0; r < rows; ++r) {
      c.image.px.insert(c.image.px.end(), gray + static_cast<size_t>(r) * stride,
                        gray + static_cast<size_t>(r) * stride + width);
    }
    c.image.h += rows;
    ++c.bands;
    return true;
  }
};

ProgressiveJpeg::Result decode(const std::string& name, const int shift, Capture& capture, uint8_t* workspace = nullptr,
                               const size_t workspaceSize = 0) {
  FsFile file;
  EXPECT_TRUE(file.openForRead(fixture(name)));
  ProgressiveJpeg::DecodeOptions options;
  options.scaleShift = static_cast<uint8_t>(shift);
  options.workspace = workspace;
  options.workspaceSize = workspaceSize;
  const auto result = ProgressiveJpeg::decode(file, options, Capture::accept, &capture);
  file.close();
  return result;
}

struct Diff {
  double mean = 0;
  int max = 0;
};

Diff compare(const Gray& a, const Gray& b) {
  Diff d;
  long sum = 0;
  for (size_t i = 0; i < a.px.size(); ++i) {
    const int e = std::abs(static_cast<int>(a.px[i]) - static_cast<int>(b.px[i]));
    sum += e;
    d.max = std::max(d.max, e);
  }
  d.mean = a.px.empty() ? 0 : static_cast<double>(sum) / a.px.size();
  return d;
}

struct Case {
  const char* jpg;
  const char* reference;
};

class ProgressiveJpegMatchesLibjpeg : public testing::TestWithParam<std::tuple<Case, int>> {};

TEST_P(ProgressiveJpegMatchesLibjpeg, AtEveryScale) {
  const auto& [c, shift] = GetParam();
  const Gray full = loadPgm(fixture(c.reference));
  ASSERT_FALSE(full.px.empty());
  const Gray expected = reducedReference(full, shift);

  Capture capture;
  const auto result = decode(c.jpg, shift, capture);
  ASSERT_EQ(result, ProgressiveJpeg::Result::Ok) << ProgressiveJpeg::resultName(result);
  if (expected.h == 0) {  // floor(h >> s) is 0: a picture shorter than 2^s pixels has no row
    EXPECT_EQ(capture.bands, 0);
    return;
  }
  EXPECT_TRUE(capture.orderOk) << "bands must arrive in order, full width";
  ASSERT_EQ(capture.image.w, expected.w);
  ASSERT_EQ(capture.image.h, expected.h);

  const Diff d = compare(capture.image, expected);
  // Our integer IDCT differs from libjpeg's islow by a level or two. The scaled references start
  // from libjpeg's CLAMPED pixels, so next to saturated edges they are a little off by
  // construction. A decoding error (a lost refinement bit, a scan out of step) shows up as a
  // whole block tens of levels off.
  // Measured: 1/1 mean 0.04 max 1; scaled mean <= 0.37 max <= 4.
  EXPECT_LT(d.mean, shift == 0 ? 0.2 : 0.6) << c.jpg << " at 1/" << (1 << shift);
  EXPECT_LE(d.max, shift == 0 ? 2 : 12) << c.jpg << " at 1/" << (1 << shift);
}

INSTANTIATE_TEST_SUITE_P(Fixtures, ProgressiveJpegMatchesLibjpeg,
                         testing::Combine(testing::Values(Case{"prog_full_420.jpg", "prog_full_420.y.pgm"},
                                                          Case{"prog_full_gray.jpg", "prog_full_gray.y.pgm"},
                                                          Case{"prog_full_444_rst.jpg", "prog_full_444_rst.y.pgm"}),
                                          testing::Values(0, 1, 2, 3)));

// The Free-Ink SDK decoder's test matrix (see generate_fixtures.py): 4:2:2 with restarts inside MCU
// rows, a restart at every MCU row, grayscale with restarts, and an image smaller than one MCU.
INSTANTIATE_TEST_SUITE_P(FreeInkMatrix, ProgressiveJpegMatchesLibjpeg,
                         testing::Combine(testing::Values(Case{"prog_422_rst.jpg", "prog_422_rst.y.pgm"},
                                                          Case{"prog_420_rst_row.jpg", "prog_420_rst_row.y.pgm"},
                                                          Case{"prog_gray_rst.jpg", "prog_gray_rst.y.pgm"},
                                                          Case{"prog_tiny_9x5.jpg", "prog_tiny_9x5.y.pgm"}),
                                          testing::Values(0, 1, 2, 3)));

// decodeResized() at the size of the 1/8 picture over every block: nothing to interpolate, so it is
// the reduced reference of every block -- the partial ones at the right and bottom edges too, which
// a 1/8 decode leaves out (floor(width / 8)). Leaving them out stretched the rest over the box and
// put every sample up to a block away from where it belongs.
class ProgressiveJpegResizedIdentity : public testing::TestWithParam<Case> {};

TEST_P(ProgressiveJpegResizedIdentity, IsTheEighthOfEveryBlock) {
  const Case& c = GetParam();
  const Gray full = loadPgm(fixture(c.reference));
  ASSERT_FALSE(full.px.empty());
  const Gray expected = reducedReference(full, 3, /*wholeBlocks=*/true);
  ASSERT_TRUE(full.w % 8 != 0 || full.h % 8 != 0) << "the fixture must have a partial block";

  struct Rows {
    Gray image;
    static bool take(void* user, uint16_t y, const uint8_t* gray, uint16_t width) {
      auto& r = *static_cast<Rows*>(user);
      if (y != r.image.h) return false;
      r.image.w = width;
      r.image.px.insert(r.image.px.end(), gray, gray + width);
      ++r.image.h;
      return true;
    }
  } rows;
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture(c.jpg)));
  ProgressiveJpeg::DecodeOptions options;
  const auto result = ProgressiveJpeg::decodeResized(file, options, static_cast<uint16_t>(expected.w),
                                                     static_cast<uint16_t>(expected.h), Rows::take, &rows);
  file.close();
  ASSERT_EQ(result, ProgressiveJpeg::Result::Ok) << ProgressiveJpeg::resultName(result);
  ASSERT_EQ(rows.image.w, expected.w);
  ASSERT_EQ(rows.image.h, expected.h);
  const Diff d = compare(rows.image, expected);
  EXPECT_LT(d.mean, 0.6) << c.jpg;
  EXPECT_LE(d.max, 12) << c.jpg;
}

INSTANTIATE_TEST_SUITE_P(Fixtures, ProgressiveJpegResizedIdentity,
                         testing::Values(Case{"prog_full_420.jpg", "prog_full_420.y.pgm"},
                                         Case{"prog_full_gray.jpg", "prog_full_gray.y.pgm"},
                                         Case{"prog_full_444_rst.jpg", "prog_full_444_rst.y.pgm"}));

// A stream cut halfway through its last scan: what arrived is decoded, the rest of that scan is
// missing -- libjpeg's rows, not a failure and not the zero bits past the end decoded as data.
// The one block the data ends in is decoded from those zero bits by both decoders, and libjpeg
// carries on through symbols its rules reject; that block may differ. The last scan refines AC
// coefficients, which 1/8 does not keep: there the picture is whole.
class ProgressiveJpegTruncated : public testing::TestWithParam<int> {};

TEST_P(ProgressiveJpegTruncated, DecodesWhatArrivedLikeLibjpeg) {
  const int shift = GetParam();
  const Gray full = loadPgm(fixture("prog_420_cut.y.pgm"));
  ASSERT_FALSE(full.px.empty());
  const Gray expected = reducedReference(full, shift);

  Capture capture;
  const auto result = decode("prog_420_cut.jpg", shift, capture);
  ASSERT_EQ(result, shift == 3 ? ProgressiveJpeg::Result::Ok : ProgressiveJpeg::Result::Degraded)
      << ProgressiveJpeg::resultName(result);
  EXPECT_TRUE(capture.orderOk);
  ASSERT_EQ(capture.image.w, expected.w);
  ASSERT_EQ(capture.image.h, expected.h);
  const Diff d = compare(capture.image, expected);
  EXPECT_LT(d.mean, shift == 0 ? 0.2 : 0.6) << "at 1/" << (1 << shift);
  // Blocks (n x n output pixels each) past the tolerance the libjpeg match allows.
  const int n = 8 >> shift;
  const int limit = shift == 0 ? 2 : 12;
  int badBlocks = 0;
  for (int by = 0; by * n < expected.h; ++by) {
    for (int bx = 0; bx * n < expected.w; ++bx) {
      int worst = 0;
      for (int y = by * n; y < std::min((by + 1) * n, expected.h); ++y) {
        for (int x = bx * n; x < std::min((bx + 1) * n, expected.w); ++x) {
          worst = std::max(worst, std::abs(capture.image.at(x, y) - expected.at(x, y)));
        }
      }
      badBlocks += worst > limit;
    }
  }
  EXPECT_LE(badBlocks, 1) << "at 1/" << (1 << shift);
}

INSTANTIATE_TEST_SUITE_P(Scales, ProgressiveJpegTruncated, testing::Values(0, 1, 2, 3));

// ---- damaged copies of a fixture, made on the fly -----------------------------------------------

std::string tempPath(const std::string& name);

std::vector<uint8_t> readBytes(const std::string& path) {
  std::vector<uint8_t> bytes;
  FILE* fp = fopen(path.c_str(), "rb");
  if (!fp) return bytes;
  int c;
  while ((c = fgetc(fp)) != EOF) bytes.push_back(static_cast<uint8_t>(c));
  fclose(fp);
  return bytes;
}

std::string writeTemp(const std::string& name, const std::vector<uint8_t>& bytes) {
  const std::string path = tempPath(name);
  FILE* fp = fopen(path.c_str(), "wb");
  EXPECT_NE(fp, nullptr);
  if (fp) {
    fwrite(bytes.data(), 1, bytes.size(), fp);
    fclose(fp);
  }
  return path;
}

// One scan of a JPEG: its header fields and where its entropy-coded data lies.
struct ScanSpan {
  int components = 0;
  int firstComponent = 0;  // the first component's id
  int ss = 0, se = 0, ah = 0, al = 0;
  size_t sosStart = 0;  // its SOS marker
  size_t dataStart = 0;
  size_t dataEnd = 0;  // the marker that ends it
};

std::vector<ScanSpan> scansOf(const std::vector<uint8_t>& d) {
  std::vector<ScanSpan> scans;
  size_t i = 2;
  while (i + 1 < d.size()) {
    if (d[i] != 0xFF) {
      ++i;
      continue;
    }
    const uint8_t marker = d[i + 1];
    if (marker == 0x00 || marker == 0xFF || (marker >= 0xD0 && marker <= 0xD7)) {
      ++i;
      continue;
    }
    if (!scans.empty() && scans.back().dataEnd == 0) scans.back().dataEnd = i;
    if (marker == 0xD9 || i + 3 >= d.size()) break;
    const size_t length = (static_cast<size_t>(d[i + 2]) << 8) | d[i + 3];
    if (marker == 0xDA) {
      ScanSpan s;
      s.components = d[i + 4];
      s.firstComponent = d[i + 5];
      const size_t o = i + 5 + 2 * s.components;
      s.ss = d[o];
      s.se = d[o + 1];
      s.ah = d[o + 2] >> 4;
      s.al = d[o + 2] & 15;
      s.sosStart = i;
      s.dataStart = i + 2 + length;
      scans.push_back(s);
    }
    i += 2 + length;
  }
  if (!scans.empty() && scans.back().dataEnd == 0) scans.back().dataEnd = d.size();
  return scans;
}

// Where the k-th restart marker of a scan's data sits (0 when there are fewer).
size_t restartMarker(const std::vector<uint8_t>& d, const ScanSpan& scan, const int k) {
  int seen = 0;
  for (size_t i = scan.dataStart; i + 1 < scan.dataEnd; ++i) {
    if (d[i] == 0xFF && d[i + 1] >= 0xD0 && d[i + 1] <= 0xD7 && seen++ == k) return i;
  }
  return 0;
}

ProgressiveJpeg::Result decodePath(const std::string& path, const int shift, Capture& capture) {
  FsFile file;
  EXPECT_TRUE(file.openForRead(path));
  ProgressiveJpeg::DecodeOptions options;
  options.scaleShift = static_cast<uint8_t>(shift);
  const auto result = ProgressiveJpeg::decode(file, options, Capture::accept, &capture);
  file.close();
  return result;
}

// Damage in prog_full_444_rst.jpg (150 x 90, 4:4:4, a restart every 3 units): written just after
// restart marker 40 of a scan, it starts at unit 123 -- block 9 of block row 6 in the 19 x 12 grid
// an interleaved DC scan and a luma scan share.
constexpr int kRstLumaCols = 19;
constexpr int kRstMarker = 40;
constexpr int kRstDamagedUnit = (kRstMarker + 1) * 3;

// The first luma scan of prog_full_444_rst.jpg with these fields (the frame's luma id is 1).
const ScanSpan* lumaScan(const std::vector<ScanSpan>& scans, const int components, const int ss, const int ah) {
  for (const ScanSpan& s : scans) {
    if (s.firstComponent == 1 && s.components == components && s.ss == ss && s.ah == ah) return &s;
  }
  return nullptr;
}

// No code of a Huffman table libjpeg writes is all ones: 48 one bits are undecodable right away.
const std::vector<uint8_t> kBadCode = {0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00};

// prog_full_444_rst.jpg with `bytes` over the start of the interval after restart marker 40 of
// `scan` -- or, with `atMarker`, over the marker itself.
std::string damagedRstCopy(const std::string& name, const int components, const int ss, const int ah,
                           const std::vector<uint8_t>& bytes, const bool atMarker = false) {
  std::vector<uint8_t> data = readBytes(fixture("prog_full_444_rst.jpg"));
  const std::vector<ScanSpan> scans = scansOf(data);
  const ScanSpan* scan = lumaScan(scans, components, ss, ah);
  EXPECT_NE(scan, nullptr);
  if (!scan) return {};
  const size_t marker = restartMarker(data, *scan, kRstMarker);
  EXPECT_GT(marker, 0u);
  const size_t at = atMarker ? marker : marker + 2;
  std::copy(bytes.begin(), bytes.end(), data.begin() + static_cast<std::ptrdiff_t>(at));
  return writeTemp(name, data);
}

// The fixture with whole scans taken out (their SOS segments and data): still a valid progressive
// JPEG, which never had them. `drop` holds scan indices into scansOf().
std::string withoutScans(const std::string& name, const std::vector<uint8_t>& data, const std::vector<int>& drop) {
  const std::vector<ScanSpan> scans = scansOf(data);
  std::vector<uint8_t> out;
  size_t from = 0;
  for (const int s : drop) {
    out.insert(out.end(), data.begin() + from, data.begin() + scans[s].sosStart);
    from = scans[s].dataEnd;
  }
  out.insert(out.end(), data.begin() + from, data.end());
  return writeTemp(name, out);
}

int scanIndex(const std::vector<ScanSpan>& scans, const ScanSpan* scan) {
  return static_cast<int>(scan - scans.data());
}

// Each n x n block (one unit; `cols` a row) of `got` before `unit` must be `before`'s, and from
// `unit` on `after`'s.
void expectBlocks(const Gray& got, const Gray& before, const Gray& after, const int n, const int cols, const int unit) {
  ASSERT_EQ(got.w, before.w);
  ASSERT_EQ(got.h, before.h);
  ASSERT_EQ(got.w, after.w);
  ASSERT_EQ(got.h, after.h);
  for (int y = 0; y < got.h; ++y) {
    for (int x = 0; x < got.w; ++x) {
      const int u = (y / n) * cols + x / n;
      const Gray& want = u < unit ? before : after;
      ASSERT_EQ(got.at(x, y), want.at(x, y))
          << "block " << u << " pixel " << x << "," << y << (u < unit ? ": before the damage" : ": after it");
    }
  }
}

// A corrupt scan is decoded up to the damage; from there on the picture is the one the file would
// give without that scan -- and without every later refinement of its coefficients, which would
// otherwise read its bits against coefficients that are not what the encoder had, and paint noise.
// The other scans are untouched, and the decode says it is degraded.
struct DamageCase {
  const char* name;
  int components, ss, ah;              // the damaged scan (lumaScan)
  std::vector<uint8_t> bytes;          // written after its restart marker 40,
  bool atMarker;                       // or over that marker
  std::vector<std::vector<int>> gone;  // the scans the picture lacks from the damage on, as lumaScan keys
  std::vector<int> shifts;
};

void PrintTo(const DamageCase& c, std::ostream* os) { *os << c.name; }

class ProgressiveJpegDamagedScan : public testing::TestWithParam<DamageCase> {};

TEST_P(ProgressiveJpegDamagedScan, DecodesUpToTheDamageAndWithoutItsScansAfter) {
  const DamageCase& c = GetParam();
  const std::vector<uint8_t> whole = readBytes(fixture("prog_full_444_rst.jpg"));
  const std::vector<ScanSpan> scans = scansOf(whole);
  std::vector<int> drop;
  for (const auto& key : c.gone) {
    const ScanSpan* s = lumaScan(scans, key[0], key[1], key[2]);
    ASSERT_NE(s, nullptr);
    drop.push_back(scanIndex(scans, s));
  }
  std::sort(drop.begin(), drop.end());
  const std::string damaged =
      damagedRstCopy(std::string(c.name) + ".jpg", c.components, c.ss, c.ah, c.bytes, c.atMarker);
  const std::string without = withoutScans(std::string(c.name) + "_without.jpg", whole, drop);
  for (const int shift : c.shifts) {
    SCOPED_TRACE(std::string(c.name) + " at 1/" + std::to_string(1 << shift));
    Capture clean;
    ASSERT_EQ(decode("prog_full_444_rst.jpg", shift, clean), ProgressiveJpeg::Result::Ok);
    Capture lacking;
    ASSERT_EQ(decodePath(without, shift, lacking), ProgressiveJpeg::Result::Ok);
    ASSERT_FALSE(lacking.image.px == clean.image.px) << "the scans must matter, or this test proves nothing";
    Capture got;
    const auto result = decodePath(damaged, shift, got);
    EXPECT_EQ(result, ProgressiveJpeg::Result::Degraded) << ProgressiveJpeg::resultName(result);
    expectBlocks(got.image, clean.image, lacking.image, 8 >> shift, kRstLumaCols, kRstDamagedUnit);
  }
  std::remove(damaged.c_str());
  std::remove(without.c_str());
}

INSTANTIATE_TEST_SUITE_P(Scans, ProgressiveJpegDamagedScan,
                         testing::Values(
                             // The first AC 1-5 pass: it and both 1-63 refinements go, the 6-63 first pass stays.
                             DamageCase{
                                 "bad_ac_first", 1, 1, 0, kBadCode, false, {{1, 1, 0}, {1, 1, 2}, {1, 1, 1}}, {0, 1}},
                             // The first 1-63 refinement: it and the last one go.
                             DamageCase{"bad_ac_refine", 1, 1, 2, kBadCode, false, {{1, 1, 2}, {1, 1, 1}}, {0, 1}},
                             // The DC scan: no DC level from there on -- mid grey under the AC detail, as libjpeg
                             // leaves the blocks it has no data for -- but the DC refinement still adds its bit.
                             DamageCase{"bad_dc", 3, 0, 0, kBadCode, false, {{3, 0, 0}}, {0, 1, 3}},
                             // A restart marker that is not there (FF 00 is a data byte): the last refinement cannot
                             // find where its next interval starts, and stops there.
                             DamageCase{"no_rst", 1, 1, 1, {0xFF, 0x00}, true, {{1, 1, 1}}, {0, 1}}),
                         [](const testing::TestParamInfo<DamageCase>& info) { return std::string(info.param.name); });

// The same at 1/8, where the AC scans are not decoded: a corrupt one costs nothing. The DC scan's
// blocks after the damage are mid grey plus the refinement bit.
TEST(ProgressiveJpegDamageAtAnEighth, ACorruptAcScanIsNotEvenRead) {
  const std::string path = damagedRstCopy("bad_ac_eighth.jpg", 1, 1, 0, kBadCode);
  Capture clean;
  ASSERT_EQ(decode("prog_full_444_rst.jpg", 3, clean), ProgressiveJpeg::Result::Ok);
  Capture got;
  EXPECT_EQ(decodePath(path, 3, got), ProgressiveJpeg::Result::Ok);
  EXPECT_TRUE(got.image.px == clean.image.px);
  std::remove(path.c_str());
}

TEST(ProgressiveJpegDamageAtAnEighth, ACorruptDcScanLeavesMidGreyFromThereOn) {
  const std::string path = damagedRstCopy("bad_dc_eighth.jpg", 3, 0, 0, kBadCode);
  Capture got;
  EXPECT_EQ(decodePath(path, 3, got), ProgressiveJpeg::Result::Degraded);
  ASSERT_GT(got.image.h, kRstDamagedUnit / kRstLumaCols);
  for (int y = 0; y < got.image.h; ++y) {
    for (int x = 0; x < got.image.w; ++x) {  // 18 pixels a row: the 19th block is cut by floor()
      if (y * kRstLumaCols + x >= kRstDamagedUnit) {
        EXPECT_NEAR(got.image.at(x, y), 128, 2) << x << "," << y;
      }
    }
  }
  std::remove(path.c_str());
}

// A stream that ends between two scans has no unit that ran short, but it lacks the scans after
// the cut: degraded too.
TEST(ProgressiveJpegDamage, AStreamCutBetweenScansIsDegraded) {
  const std::vector<uint8_t> whole = readBytes(fixture("prog_full_420.jpg"));
  const std::vector<ScanSpan> scans = scansOf(whole);
  ASSERT_GE(scans.size(), 2u);
  // Cut at the marker that ends the second-to-last scan's data (a DHT or the last SOS).
  const size_t cut = scans[scans.size() - 2].dataEnd;
  const std::string path = writeTemp("cut_between.jpg", std::vector<uint8_t>(whole.begin(), whole.begin() + cut));
  Capture got;
  EXPECT_EQ(decodePath(path, 0, got), ProgressiveJpeg::Result::Degraded);
  EXPECT_EQ(got.image.h, 141);
  std::remove(path.c_str());
}

// A file that holds every scan but ends without its EOI marker is whole: its rows are the complete
// file's, so it is no Degraded picture -- the reader caches it, as it did before Degraded existed.
TEST(ProgressiveJpegDamage, AFileThatOnlyLacksItsEoiIsWhole) {
  for (const char* jpg : {"prog_full_420.jpg", "prog_full_gray.jpg", "prog_full_444_rst.jpg"}) {
    const std::vector<uint8_t> whole = readBytes(fixture(jpg));
    ASSERT_GE(whole.size(), 2u);
    ASSERT_EQ(whole[whole.size() - 2], 0xFF);
    ASSERT_EQ(whole[whole.size() - 1], 0xD9) << "the fixture ends with its EOI";
    const std::string path =
        writeTemp(std::string("no_eoi_") + jpg, std::vector<uint8_t>(whole.begin(), whole.end() - 2));
    for (int shift = 0; shift <= 3; ++shift) {
      SCOPED_TRACE(std::string(jpg) + " at 1/" + std::to_string(1 << shift));
      Capture clean;
      ASSERT_EQ(decode(jpg, shift, clean), ProgressiveJpeg::Result::Ok);
      Capture got;
      const auto result = decodePath(path, shift, got);
      EXPECT_EQ(result, ProgressiveJpeg::Result::Ok) << ProgressiveJpeg::resultName(result);
      EXPECT_EQ(got.image.h, clean.image.h);
      EXPECT_TRUE(got.image.px == clean.image.px);
    }
    std::remove(path.c_str());
  }
}

// ...but a file cut after its last scan's header, before that scan's data, lacks the scan: the
// header promises the last bit of every AC coefficient, and none arrived. At 1/8, which keeps no AC
// coefficient, the picture is whole.
TEST(ProgressiveJpegDamage, AFileCutBeforeItsLastScansDataIsDegraded) {
  const std::vector<uint8_t> whole = readBytes(fixture("prog_full_420.jpg"));
  const std::vector<ScanSpan> scans = scansOf(whole);
  ASSERT_FALSE(scans.empty());
  const ScanSpan& last = scans.back();
  ASSERT_GT(last.ss, 0) << "the fixture ends with an AC scan";
  ASSERT_EQ(last.al, 0) << "...that completes its coefficients";
  const std::string path =
      writeTemp("cut_before_data.jpg", std::vector<uint8_t>(whole.begin(), whole.begin() + last.dataStart));
  for (int shift = 0; shift <= 3; ++shift) {
    SCOPED_TRACE("at 1/" + std::to_string(1 << shift));
    Capture clean;
    ASSERT_EQ(decode("prog_full_420.jpg", shift, clean), ProgressiveJpeg::Result::Ok);
    Capture got;
    const auto result = decodePath(path, shift, got);
    EXPECT_EQ(got.image.h, clean.image.h) << "every row is emitted";
    if (shift == 3) {
      EXPECT_EQ(result, ProgressiveJpeg::Result::Ok) << ProgressiveJpeg::resultName(result);
      EXPECT_TRUE(got.image.px == clean.image.px);
    } else {
      EXPECT_EQ(result, ProgressiveJpeg::Result::Degraded) << ProgressiveJpeg::resultName(result);
    }
  }
  std::remove(path.c_str());
}

// Cut inside the table segment before the last scan: the segment is incomplete, so indexing ends
// before it, as if the stream had ended there.
TEST(ProgressiveJpegDamage, AStreamCutInsideASegmentIsDegraded) {
  const std::vector<uint8_t> whole = readBytes(fixture("prog_full_420.jpg"));
  const std::vector<ScanSpan> scans = scansOf(whole);
  ASSERT_GE(scans.size(), 2u);
  const size_t marker = scans[scans.size() - 2].dataEnd;
  ASSERT_EQ(whole[marker + 1], 0xC4) << "the last scan's Huffman table comes before it";
  const std::string path =
      writeTemp("cut_in_dht.jpg", std::vector<uint8_t>(whole.begin(), whole.begin() + marker + 12));
  Capture got;
  EXPECT_EQ(decodePath(path, 0, got), ProgressiveJpeg::Result::Degraded);
  EXPECT_EQ(got.image.h, 141);
  std::remove(path.c_str());
}

// Where a scan stopped is part of the decode's state: a checkpoint taken before or after the
// damage resumes to the same rows, and the resumed decode still says it is degraded.
TEST(ProgressiveJpegDamage, ACheckpointCarriesAStoppedScan) {
  const std::string path = damagedRstCopy("bad_ac_ckpt.jpg", 1, 1, 0, kBadCode);
  Capture oneShot;
  ASSERT_EQ(decodePath(path, 0, oneShot), ProgressiveJpeg::Result::Degraded);
  const std::string ckpt = tempPath("bad_ac.ckpt");
  int stop = 1;
  for (;; ++stop) {
    SCOPED_TRACE("checkpoint after step " + std::to_string(stop));
    Capture capture;
    ProgressiveJpeg::DecodeOptions options;
    bool finishedFirst = false;
    {
      FsFile file;
      ASSERT_TRUE(file.openForRead(path));
      ProgressiveJpeg::Decoder first;
      ASSERT_EQ(first.begin(file, options, Capture::accept, &capture), ProgressiveJpeg::Result::Ok);
      for (int i = 0; i < stop && !finishedFirst; ++i)
        finishedFirst = first.step(1) != ProgressiveJpeg::Result::Pending;
      if (!finishedFirst) {
        FsFile out;
        ASSERT_TRUE(out.openForWrite(ckpt));
        ASSERT_TRUE(first.writeCheckpoint(out));
        out.close();
      }
      first.end();
      file.close();
    }
    if (finishedFirst) break;
    {
      FsFile file;
      ASSERT_TRUE(file.openForRead(path));
      FsFile in;
      ASSERT_TRUE(in.openForRead(ckpt));
      ProgressiveJpeg::Decoder second;
      ASSERT_EQ(second.resume(file, options, Capture::accept, &capture, in), ProgressiveJpeg::Result::Ok);
      in.close();
      ProgressiveJpeg::Result r;
      do {
        r = second.step(1);
      } while (r == ProgressiveJpeg::Result::Pending);
      EXPECT_EQ(r, ProgressiveJpeg::Result::Degraded);
      second.end();
      file.close();
    }
    EXPECT_TRUE(capture.image.px == oneShot.image.px);
  }
  EXPECT_GT(stop, 10) << "the decode must have been stopped on both sides of the damage";
  std::remove(ckpt.c_str());
  std::remove(path.c_str());
}

// A file cut just before a restart marker of its last scan: the data ends where an interval ends.
// The rows above that interval are the complete file's; nothing fails.
TEST(ProgressiveJpeg, AStreamCutAtARestartMarkerKeepsWhatArrived) {
  const std::vector<uint8_t> whole = readBytes(fixture("prog_full_444_rst.jpg"));
  const std::vector<ScanSpan> scans = scansOf(whole);
  ASSERT_FALSE(scans.empty());
  const ScanSpan& last = scans.back();
  ASSERT_EQ(last.components, 1) << "the fixture ends with a luma scan";
  constexpr int kInterval = 3;  // restart_marker_blocks in generate_fixtures.py
  constexpr int kMarker = 40;   // 41 intervals arrive: blocks 0-122 of 19 x 12, block rows 0-5 whole
  const size_t cut = restartMarker(whole, last, kMarker);
  ASSERT_GT(cut, 0u);
  const std::string path = writeTemp("cut_at_rst.jpg", std::vector<uint8_t>(whole.begin(), whole.begin() + cut));

  Capture clean;
  ASSERT_EQ(decode("prog_full_444_rst.jpg", 0, clean), ProgressiveJpeg::Result::Ok);
  Capture cutRows;
  FsFile file;
  ASSERT_TRUE(file.openForRead(path));
  ProgressiveJpeg::DecodeOptions options;
  const auto result = ProgressiveJpeg::decode(file, options, Capture::accept, &cutRows);
  file.close();
  std::remove(path.c_str());
  ASSERT_EQ(result, ProgressiveJpeg::Result::Degraded) << ProgressiveJpeg::resultName(result);
  ASSERT_EQ(cutRows.image.h, clean.image.h);
  const int lumaCols = (clean.image.w + 7) / 8;
  const int firstMissingRow = (kMarker + 1) * kInterval / lumaCols * 8;
  ASSERT_GT(firstMissingRow, 0);
  for (int y = 0; y < firstMissingRow; ++y) {
    ASSERT_TRUE(std::equal(cutRows.image.px.begin() + y * clean.image.w,
                           cutRows.image.px.begin() + (y + 1) * clean.image.w,
                           clean.image.px.begin() + y * clean.image.w))
        << "row " << y;
  }
}

TEST(ProgressiveJpeg, ProbeReportsGeometry) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("prog_full_420.jpg")));
  ProgressiveJpeg::ImageInfo info;
  ASSERT_EQ(ProgressiveJpeg::probe(file, info), ProgressiveJpeg::Result::Ok);
  EXPECT_EQ(file.position(), 0u) << "probe must rewind";
  file.close();
  EXPECT_EQ(info.width, 203);
  EXPECT_EQ(info.height, 141);
  EXPECT_EQ(info.componentCount, 3);
  EXPECT_EQ(info.maxHorizontal, 2);
  EXPECT_EQ(info.maxVertical, 2);
}

TEST(ProgressiveJpeg, RejectsBaselineWithoutEmittingRows) {
  Capture capture;
  EXPECT_EQ(decode("contrast_420.jpg", 0, capture), ProgressiveJpeg::Result::Unsupported);
  EXPECT_EQ(capture.bands, 0);
}

// The workspace is the decoder's whole working set; a caller-supplied block (the reader's scratch
// arena) must be enough on its own, even misaligned, and one byte short must be refused up front.
TEST(ProgressiveJpeg, RunsInACallerWorkspaceAndRefusesAShortOne) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("prog_full_420.jpg")));
  ProgressiveJpeg::ImageInfo info;
  ASSERT_EQ(ProgressiveJpeg::probe(file, info), ProgressiveJpeg::Result::Ok);
  file.close();
  const size_t need = ProgressiveJpeg::workspaceBytes(info, 1);
  ASSERT_GT(need, 0u);

  std::vector<uint8_t> block(need + 1);
  Capture ok;
  EXPECT_EQ(decode("prog_full_420.jpg", 1, ok, block.data() + 1, need), ProgressiveJpeg::Result::Ok)
      << "an odd address must still work: the decoder aligns inside its own slack";
  EXPECT_EQ(ok.image.h, 141 >> 1);

  Capture refused;
  EXPECT_EQ(decode("prog_full_420.jpg", 1, refused, block.data(), need - 9), ProgressiveJpeg::Result::OutOfMemory);
  EXPECT_EQ(refused.bands, 0);
}

// Home's cover pass drives the decoder a slice at a time and reads other files in between. However
// the work is cut, the rows must be the one-shot decode's, byte for byte.
struct SteppedRun {
  Capture capture;
  int indexSteps = 0;
  int bandSteps = 0;
  ProgressiveJpeg::Result result = ProgressiveJpeg::Result::Ok;
};

SteppedRun decodeStepped(const std::string& name, const int shift, const uint16_t units) {
  SteppedRun run;
  FsFile file;
  EXPECT_TRUE(file.openForRead(fixture(name)));
  ProgressiveJpeg::DecodeOptions options;
  options.scaleShift = static_cast<uint8_t>(shift);
  ProgressiveJpeg::Decoder decoder;
  run.result = decoder.begin(file, options, Capture::accept, &run.capture);
  if (run.result != ProgressiveJpeg::Result::Ok) return run;
  EXPECT_TRUE(decoder.active());
  for (int guard = 0; guard < 100000; ++guard) {
    const int bandsBefore = run.capture.bands;
    run.result = decoder.step(units);
    (run.capture.bands == bandsBefore ? run.indexSteps : run.bandSteps)++;
    // Someone else moves the file position between slices.
    file.seek(3);
    uint8_t junk[7];
    file.read(junk, sizeof(junk));
    if (run.result != ProgressiveJpeg::Result::Pending) break;
  }
  decoder.end();
  EXPECT_FALSE(decoder.active());
  EXPECT_EQ(file.position(), 0u) << "end() rewinds to the JPEG's start";
  file.close();
  return run;
}

// The scale picks only the IDCT size, not how the decode is sliced, so one shift is enough here:
// ProgressiveJpegCheckpoint steps one unit at a time at shifts 0 and 1.
class ProgressiveJpegStepped : public testing::TestWithParam<std::tuple<const char*, int>> {};

TEST_P(ProgressiveJpegStepped, MatchesTheOneShotDecode) {
  const auto& [jpg, shift] = GetParam();
  Capture oneShot;
  ASSERT_EQ(decode(jpg, shift, oneShot), ProgressiveJpeg::Result::Ok);

  for (const uint16_t units : {uint16_t{1}, uint16_t{3}, uint16_t{UINT16_MAX}}) {
    const SteppedRun run = decodeStepped(jpg, shift, units);
    ASSERT_EQ(run.result, ProgressiveJpeg::Result::Ok) << ProgressiveJpeg::resultName(run.result);
    EXPECT_TRUE(run.capture.orderOk);
    EXPECT_EQ(run.capture.image.w, oneShot.image.w);
    EXPECT_EQ(run.capture.image.h, oneShot.image.h);
    EXPECT_TRUE(run.capture.image.px == oneShot.image.px) << jpg << " units=" << units;
    if (units == 1) {
      EXPECT_GT(run.indexSteps, 1) << "a 4 KB index step must not read the whole file";
      // A band is one MCU row: one callback per block row it holds (two for 4:2:0).
      EXPECT_GT(run.bandSteps, 1);
      EXPECT_LE(run.bandSteps, run.capture.bands);
      EXPECT_GE(run.bandSteps * 2, run.capture.bands) << "one band per unit";
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Fixtures, ProgressiveJpegStepped,
                         testing::Combine(testing::Values("prog_full_420.jpg", "prog_full_gray.jpg",
                                                          "prog_full_444_rst.jpg"),
                                          testing::Values(3)));

// A pass that gives up halfway (the reader opened a book) must leave nothing behind, and the next
// attempt starts clean.
TEST(ProgressiveJpeg, EndMidDecodeReleasesAndRestartsClean) {
  Capture oneShot;
  ASSERT_EQ(decode("prog_full_420.jpg", 1, oneShot), ProgressiveJpeg::Result::Ok);

  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("prog_full_420.jpg")));
  ProgressiveJpeg::DecodeOptions options;
  options.scaleShift = 1;
  Capture partial;
  ProgressiveJpeg::Decoder decoder;
  ASSERT_EQ(decoder.begin(file, options, Capture::accept, &partial), ProgressiveJpeg::Result::Ok);
  ProgressiveJpeg::Result r = ProgressiveJpeg::Result::Pending;
  while (r == ProgressiveJpeg::Result::Pending && partial.bands < 2) r = decoder.step(1);
  ASSERT_EQ(r, ProgressiveJpeg::Result::Pending);
  decoder.end();
  decoder.end();  // idempotent
  EXPECT_EQ(file.position(), 0u);
  EXPECT_EQ(decoder.step(1), ProgressiveJpeg::Result::InvalidData) << "no session after end()";

  Capture again;
  ASSERT_EQ(decoder.begin(file, options, Capture::accept, &again), ProgressiveJpeg::Result::Ok);
  do {
    r = decoder.step(2);
  } while (r == ProgressiveJpeg::Result::Pending);
  EXPECT_EQ(r, ProgressiveJpeg::Result::Ok);
  EXPECT_TRUE(again.image.px == oneShot.image.px);
  file.close();
}

TEST(ProgressiveJpeg, StepPollsTheAbortHook) {
  FsFile file;
  ASSERT_TRUE(file.openForRead(fixture("prog_full_gray.jpg")));  // one block row per band
  struct Hook {
    int calls = 0;
    int abortAt = 0;
    static bool poll(void* user) {
      auto& h = *static_cast<Hook*>(user);
      return ++h.calls >= h.abortAt;
    }
  } hook;
  hook.abortAt = 3;  // two bands, then abort before the third
  ProgressiveJpeg::DecodeOptions options;
  options.shouldAbort = Hook::poll;
  options.abortUser = &hook;
  Capture capture;
  ProgressiveJpeg::Decoder decoder;
  ASSERT_EQ(decoder.begin(file, options, Capture::accept, &capture), ProgressiveJpeg::Result::Ok);
  ProgressiveJpeg::Result r;
  do {
    r = decoder.step(1);
  } while (r == ProgressiveJpeg::Result::Pending);
  EXPECT_EQ(r, ProgressiveJpeg::Result::Aborted);
  EXPECT_EQ(capture.bands, 2);
  EXPECT_TRUE(decoder.active()) << "an abort leaves the session resumable";
  hook.abortAt = 1000;
  do {
    r = decoder.step(1);
  } while (r == ProgressiveJpeg::Result::Pending);
  EXPECT_EQ(r, ProgressiveJpeg::Result::Ok) << "stepping on after an abort finishes the decode";
  decoder.end();
  file.close();
}

// The reader's image lane has to give its workspace back on every page turn. A checkpoint taken
// at any step -- mid index pass or between bands -- and resumed by a fresh decoder in a fresh
// workspace must continue with exactly the rows the uninterrupted decode would have emitted.
std::string tempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() / ("pjpeg_" + name)).string();
}

class ProgressiveJpegCheckpoint : public testing::TestWithParam<std::tuple<const char*, int>> {};

TEST_P(ProgressiveJpegCheckpoint, ResumesWithIdenticalRowsFromEveryStep) {
  const auto& [jpg, shift] = GetParam();
  Capture oneShot;
  ASSERT_EQ(decode(jpg, shift, oneShot), ProgressiveJpeg::Result::Ok);
  const std::string ckpt = tempPath(std::string(jpg) + "_" + std::to_string(shift) + ".ckpt");

  // How many 1-unit steps the whole decode takes.
  const int totalSteps = [&] {
    SteppedRun run = decodeStepped(jpg, shift, 1);
    return run.indexSteps + run.bandSteps;
  }();
  ASSERT_GT(totalSteps, 3);

  for (int stop = 1; stop < totalSteps; ++stop) {
    SCOPED_TRACE("checkpoint after step " + std::to_string(stop));
    Capture capture;
    ProgressiveJpeg::DecodeOptions options;
    options.scaleShift = static_cast<uint8_t>(shift);
    {
      FsFile file;
      ASSERT_TRUE(file.openForRead(fixture(jpg)));
      ProgressiveJpeg::Decoder first;
      ASSERT_EQ(first.begin(file, options, Capture::accept, &capture), ProgressiveJpeg::Result::Ok);
      for (int i = 0; i < stop; ++i) ASSERT_EQ(first.step(1), ProgressiveJpeg::Result::Pending);
      FsFile out;
      ASSERT_TRUE(out.openForWrite(ckpt));
      ASSERT_TRUE(first.writeCheckpoint(out));
      EXPECT_EQ(out.position(), first.checkpointSize());
      out.close();
      first.end();
      file.close();
    }
    // A different workspace address: the checkpoint must not depend on where the first one sat.
    std::vector<uint8_t> elsewhere;
    {
      FsFile file;
      ASSERT_TRUE(file.openForRead(fixture(jpg)));
      ProgressiveJpeg::ImageInfo info;
      ASSERT_EQ(ProgressiveJpeg::probe(file, info), ProgressiveJpeg::Result::Ok);
      elsewhere.resize(ProgressiveJpeg::workspaceBytes(info, static_cast<uint8_t>(shift)) + 3);
      options.workspace = elsewhere.data() + 3;
      options.workspaceSize = elsewhere.size() - 3;
      FsFile in;
      ASSERT_TRUE(in.openForRead(ckpt));
      ProgressiveJpeg::Decoder second;
      ASSERT_EQ(second.resume(file, options, Capture::accept, &capture, in), ProgressiveJpeg::Result::Ok);
      in.close();
      ProgressiveJpeg::Result r;
      do {
        r = second.step(1);
      } while (r == ProgressiveJpeg::Result::Pending);
      ASSERT_EQ(r, ProgressiveJpeg::Result::Ok);
      second.end();
      file.close();
    }
    EXPECT_TRUE(capture.orderOk);
    EXPECT_EQ(capture.image.h, oneShot.image.h);
    EXPECT_TRUE(capture.image.px == oneShot.image.px);
  }
  std::remove(ckpt.c_str());
}

// 1/8 decodes no AC scan (their cursors never move), so it is checkpointed on its own.
INSTANTIATE_TEST_SUITE_P(Fixtures, ProgressiveJpegCheckpoint,
                         testing::Combine(testing::Values("prog_full_420.jpg", "prog_full_gray.jpg",
                                                          "prog_full_444_rst.jpg"),
                                          testing::Values(0, 1, 3)));

// A checkpoint only resumes the decode it was taken of.
TEST(ProgressiveJpeg, ResumeRefusesAForeignCheckpoint) {
  const std::string ckpt = tempPath("foreign.ckpt");
  Capture capture;
  {
    FsFile file;
    ASSERT_TRUE(file.openForRead(fixture("prog_full_420.jpg")));
    ProgressiveJpeg::DecodeOptions options;
    options.scaleShift = 1;
    ProgressiveJpeg::Decoder d;
    ASSERT_EQ(d.begin(file, options, Capture::accept, &capture), ProgressiveJpeg::Result::Ok);
    ASSERT_EQ(d.step(1), ProgressiveJpeg::Result::Pending);
    FsFile out;
    ASSERT_TRUE(out.openForWrite(ckpt));
    ASSERT_TRUE(d.writeCheckpoint(out));
    out.close();
  }
  const auto resumeWith = [&](const char* jpg, const int shift, const size_t truncateTo = 0) {
    if (truncateTo) std::filesystem::resize_file(ckpt, truncateTo);
    FsFile file;
    EXPECT_TRUE(file.openForRead(fixture(jpg)));
    ProgressiveJpeg::DecodeOptions options;
    options.scaleShift = static_cast<uint8_t>(shift);
    FsFile in;
    EXPECT_TRUE(in.openForRead(ckpt));
    ProgressiveJpeg::Decoder d;
    Capture c;
    const auto r = d.resume(file, options, Capture::accept, &c, in);
    EXPECT_FALSE(d.active() && r != ProgressiveJpeg::Result::Ok) << "a refused resume holds nothing";
    return r;
  };
  EXPECT_EQ(resumeWith("prog_full_420.jpg", 2), ProgressiveJpeg::Result::InvalidData) << "another scale";
  EXPECT_EQ(resumeWith("prog_full_gray.jpg", 1), ProgressiveJpeg::Result::InvalidData) << "another image";
  EXPECT_EQ(resumeWith("prog_full_420.jpg", 1), ProgressiveJpeg::Result::Ok) << "the image it was taken of";
  EXPECT_EQ(resumeWith("prog_full_420.jpg", 1, 100), ProgressiveJpeg::Result::InvalidData) << "a truncated file";
  std::remove(ckpt.c_str());
}

// Given read windows (DecodeOptions::readWindows, scanWindowBytes()), the decode reads each scan
// through a window of its own. With one buffer they all share, every scan re-read its 512 bytes in every band: a
// 106 KB cover took 1275 reads and 650 KB. The rows must not change, and a checkpoint taken with or
// without the windows must resume into either.
// The windows sit in the bit reader and the file is read the same way at every scale: one shift.
class ProgressiveJpegReadWindows : public testing::TestWithParam<std::tuple<const char*, int>> {};

struct WindowedRun {
  ProgressiveJpeg::Result result = ProgressiveJpeg::Result::InvalidData;
  ProgressiveJpeg::DecodeStats stats;
  Capture capture;
};

WindowedRun decodeWith(const std::string& name, const int shift, const bool windows) {
  WindowedRun run;
  FsFile file;
  EXPECT_TRUE(file.openForRead(fixture(name)));
  ProgressiveJpeg::ImageInfo info;
  EXPECT_EQ(ProgressiveJpeg::probe(file, info), ProgressiveJpeg::Result::Ok);
  std::vector<uint8_t> workspace(ProgressiveJpeg::workspaceBytes(info, static_cast<uint8_t>(shift)));
  // One byte off alignment: the decoder aligns the windows itself.
  std::vector<uint8_t> readWindows(windows ? ProgressiveJpeg::scanWindowBytes() + 4 : 0);
  ProgressiveJpeg::DecodeOptions options;
  options.scaleShift = static_cast<uint8_t>(shift);
  options.workspace = workspace.data();
  options.workspaceSize = workspace.size();
  if (windows) {
    options.readWindows = readWindows.data() + 1;
    options.readWindowsSize = readWindows.size() - 1;
  }
  options.stats = &run.stats;
  run.result = ProgressiveJpeg::decode(file, options, Capture::accept, &run.capture);
  file.close();
  return run;
}

TEST_P(ProgressiveJpegReadWindows, ReadTheFileFarLessOftenForTheSameRows) {
  const auto& [jpg, shift] = GetParam();
  const WindowedRun shared = decodeWith(jpg, shift, false);
  const WindowedRun windowed = decodeWith(jpg, shift, true);
  ASSERT_EQ(shared.result, ProgressiveJpeg::Result::Ok);
  ASSERT_EQ(windowed.result, ProgressiveJpeg::Result::Ok);
  EXPECT_TRUE(windowed.capture.orderOk);
  EXPECT_EQ(windowed.capture.image.h, shared.capture.image.h);
  EXPECT_TRUE(windowed.capture.image.px == shared.capture.image.px) << "the windows change only how the file is read";
  std::printf("[ pjpeg    ] %s 1/%d: %u reads / %u bytes with one shared buffer, %u / %u with the windows\n", jpg,
              1 << shift, static_cast<unsigned>(shared.stats.reads), static_cast<unsigned>(shared.stats.bytesRead),
              static_cast<unsigned>(windowed.stats.reads), static_cast<unsigned>(windowed.stats.bytesRead));
  EXPECT_LE(windowed.stats.reads * 4, shared.stats.reads);
  EXPECT_LT(windowed.stats.bytesRead, shared.stats.bytesRead);
}

INSTANTIATE_TEST_SUITE_P(Fixtures, ProgressiveJpegReadWindows,
                         testing::Combine(testing::Values("prog_full_420.jpg", "prog_full_gray.jpg",
                                                          "prog_full_444_rst.jpg"),
                                          testing::Values(0)));

TEST(ProgressiveJpeg, ACheckpointResumesWithOrWithoutTheReadWindows) {
  const char* const jpg = "prog_full_420.jpg";
  constexpr int kShift = 1;
  Capture oneShot;
  ASSERT_EQ(decode(jpg, kShift, oneShot), ProgressiveJpeg::Result::Ok);
  const std::string ckpt = tempPath("windows.ckpt");
  FsFile probeFile;
  ASSERT_TRUE(probeFile.openForRead(fixture(jpg)));
  ProgressiveJpeg::ImageInfo info;
  ASSERT_EQ(ProgressiveJpeg::probe(probeFile, info), ProgressiveJpeg::Result::Ok);
  probeFile.close();
  const size_t minimum = ProgressiveJpeg::workspaceBytes(info, kShift);
  for (const bool firstWindows : {false, true}) {
    for (const bool secondWindows : {false, true}) {
      // Stopped partway through the index (step 2), and partway through the bands.
      for (const int stop : {2, 12}) {
        SCOPED_TRACE(std::string("windows ") + (firstWindows ? "on" : "off") + " -> " + (secondWindows ? "on" : "off") +
                     ", stopped after step " + std::to_string(stop));
        Capture capture;
        ProgressiveJpeg::DecodeOptions options;
        options.scaleShift = kShift;
        std::vector<uint8_t> a(minimum);
        std::vector<uint8_t> aWindows(ProgressiveJpeg::scanWindowBytes());
        {
          FsFile file;
          ASSERT_TRUE(file.openForRead(fixture(jpg)));
          options.workspace = a.data();
          options.workspaceSize = a.size();
          options.readWindows = firstWindows ? aWindows.data() : nullptr;
          options.readWindowsSize = firstWindows ? aWindows.size() : 0;
          ProgressiveJpeg::Decoder first;
          ASSERT_EQ(first.begin(file, options, Capture::accept, &capture), ProgressiveJpeg::Result::Ok);
          for (int i = 0; i < stop; ++i) ASSERT_EQ(first.step(1), ProgressiveJpeg::Result::Pending);
          FsFile out;
          ASSERT_TRUE(out.openForWrite(ckpt));
          ASSERT_TRUE(first.writeCheckpoint(out));
          out.close();
          first.end();
          file.close();
        }
        std::vector<uint8_t> b(minimum + 5);
        std::vector<uint8_t> bWindows(ProgressiveJpeg::scanWindowBytes());
        {
          FsFile file;
          ASSERT_TRUE(file.openForRead(fixture(jpg)));
          options.workspace = b.data() + 5;
          options.workspaceSize = b.size() - 5;
          options.readWindows = secondWindows ? bWindows.data() : nullptr;
          options.readWindowsSize = secondWindows ? bWindows.size() : 0;
          FsFile in;
          ASSERT_TRUE(in.openForRead(ckpt));
          ProgressiveJpeg::Decoder second;
          ASSERT_EQ(second.resume(file, options, Capture::accept, &capture, in), ProgressiveJpeg::Result::Ok);
          in.close();
          ProgressiveJpeg::Result r;
          do {
            r = second.step(1);
          } while (r == ProgressiveJpeg::Result::Pending);
          ASSERT_EQ(r, ProgressiveJpeg::Result::Ok);
          second.end();
          file.close();
        }
        EXPECT_TRUE(capture.orderOk);
        EXPECT_TRUE(capture.image.px == oneShot.image.px);
      }
    }
  }
  std::remove(ckpt.c_str());
}

// At 1/8 a block is one pixel, its DC level: the AC scans cannot change it, so the decode skips
// them. Rows must stay byte for byte what decoding every scan gave (recorded below from the decoder
// that still decoded them), and the file must be read less than at 1/4, which still needs them.
uint64_t fnv1a(const std::vector<uint8_t>& bytes) {
  uint64_t h = 1469598103934665603ULL;
  for (const uint8_t b : bytes) h = (h ^ b) * 1099511628211ULL;
  return h;
}

struct EighthCase {
  const char* jpg;
  uint64_t rowsHash;  // FNV-1a of the 1/8 rows when every scan was decoded
};

class ProgressiveJpegEighth : public testing::TestWithParam<EighthCase> {};

TEST_P(ProgressiveJpegEighth, SkipsTheAcScansWithTheSameRows) {
  const EighthCase& c = GetParam();
  for (const bool windows : {false, true}) {
    SCOPED_TRACE(windows ? "with read windows" : "with the shared buffer");
    const WindowedRun eighth = decodeWith(c.jpg, 3, windows);
    const WindowedRun quarter = decodeWith(c.jpg, 2, windows);
    ASSERT_EQ(eighth.result, ProgressiveJpeg::Result::Ok);
    ASSERT_EQ(quarter.result, ProgressiveJpeg::Result::Ok);
    EXPECT_TRUE(eighth.capture.orderOk);
    EXPECT_EQ(fnv1a(eighth.capture.image.px), c.rowsHash) << c.jpg;
    std::printf("[ pjpeg    ] %s %s: 1/8 %u reads / %u bytes, 1/4 %u / %u\n", c.jpg, windows ? "windows" : "shared",
                static_cast<unsigned>(eighth.stats.reads), static_cast<unsigned>(eighth.stats.bytesRead),
                static_cast<unsigned>(quarter.stats.reads), static_cast<unsigned>(quarter.stats.bytesRead));
    EXPECT_LT(eighth.stats.reads, quarter.stats.reads);
    EXPECT_LT(eighth.stats.bytesRead, quarter.stats.bytesRead);
  }
}

INSTANTIATE_TEST_SUITE_P(Fixtures, ProgressiveJpegEighth,
                         testing::Values(EighthCase{"prog_full_420.jpg", 13821861766860504631ULL},
                                         EighthCase{"prog_full_gray.jpg", 7087090682878387780ULL},
                                         EighthCase{"prog_full_444_rst.jpg", 10387207835323122819ULL},
                                         EighthCase{"thin_lines_prog.jpg", 2001519597461149831ULL},
                                         EighthCase{"progressive_tall.jpg", 11623068769939918297ULL}));

// Smaller output keeps fewer coefficients per block: the working set has to shrink with it.
TEST(ProgressiveJpeg, WorkspaceShrinksWithScale) {
  ProgressiveJpeg::ImageInfo info;
  info.width = 1210;
  info.height = 1327;
  info.componentCount = 3;
  const size_t full = ProgressiveJpeg::workspaceBytes(info, 0);
  const size_t half = ProgressiveJpeg::workspaceBytes(info, 1);
  const size_t eighth = ProgressiveJpeg::workspaceBytes(info, 3);
  EXPECT_GT(full, half);
  EXPECT_GT(half, eighth);
  EXPECT_LT(half, 20u * 1024) << "the Strange Pictures diagrams decode at 1/2; this is their budget";
}

// At 1/8 a block is its DC level, so the session holds only what the DC scans need: the decode the
// converters fall back to when the heap is short costs what a DC-only preview decoder costs
// (about 2 KB of state plus a row of blocks), not the 13-16 KB of a full session. Host sizes are
// 64-bit; the C3's are smaller.
TEST(ProgressiveJpeg, AnEighthNeedsNoMoreThanTheDcScans) {
  ProgressiveJpeg::ImageInfo info;
  info.width = 1200;
  info.height = 1600;
  info.componentCount = 3;
  info.maxHorizontal = 2;
  info.maxVertical = 2;
  const size_t eighth = ProgressiveJpeg::workspaceBytes(info, 3);
  EXPECT_LT(eighth, 4u * 1024);
  EXPECT_LT(eighth * 4, ProgressiveJpeg::workspaceBytes(info, 2));
}

}  // namespace
