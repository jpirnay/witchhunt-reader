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
Gray reducedReference(const Gray& full, const int shift) {
  const int n = 8 >> shift;
  const double pi = 3.14159265358979323846;
  auto c = [](const int k) { return k == 0 ? 0.70710678118654752 : 1.0; };
  Gray out;
  out.w = full.w >> shift;
  out.h = full.h >> shift;
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

}  // namespace
