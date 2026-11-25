// BitmapRowCache keeps the 2-bit rows Bitmap::readNextRow() makes, so later draws of a sleep picture replay them
// instead of decoding and dithering the BMP again. A replay must hand out exactly the rows a decode makes, and a cache
// that no longer describes the file, the tone mapping or the panel's table must be recorded again, not replayed.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "Bitmap.h"
#include "BitmapRowCache.h"

namespace {

namespace fs = std::filesystem;

void putLE16(std::vector<uint8_t>& out, const uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}

void putLE32(std::vector<uint8_t>& out, const uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

constexpr uint32_t kGrayBmpPixelOffset = 14 + 40 + 256 * 4;

// An 8-bit gray BMP: a gradient with noise, so the dither has error to carry from row to row. It spans the middle of
// the range only, so a tone mapping has something to stretch.
void writeGrayBmp(const std::string& path, const int width, const int height, const uint32_t seed,
                  const bool topDown = false) {
  const int rowBytes = ((width * 8 + 31) / 32) * 4;
  std::vector<uint8_t> out;
  out.push_back('B');
  out.push_back('M');
  putLE32(out, kGrayBmpPixelOffset + static_cast<uint32_t>(rowBytes * height));
  putLE32(out, 0);
  putLE32(out, kGrayBmpPixelOffset);
  putLE32(out, 40);
  putLE32(out, static_cast<uint32_t>(width));
  putLE32(out, static_cast<uint32_t>(topDown ? -height : height));
  putLE16(out, 1);
  putLE16(out, 8);
  putLE32(out, 0);
  putLE32(out, static_cast<uint32_t>(rowBytes * height));
  putLE32(out, 2835);
  putLE32(out, 2835);
  putLE32(out, 256);
  putLE32(out, 256);
  for (int i = 0; i < 256; ++i) {
    for (int c = 0; c < 3; ++c) out.push_back(static_cast<uint8_t>(i));
    out.push_back(0);
  }
  std::mt19937 rng(seed);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < rowBytes; ++x) {
      const int base = 80 + (x * 90) / (width > 1 ? width - 1 : 1);
      const int noisy = base + static_cast<int>(rng() % 61) - 30;
      out.push_back(static_cast<uint8_t>(noisy < 0 ? 0 : (noisy > 255 ? 255 : noisy)));
    }
  }
  FILE* f = std::fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
}

using Rows = std::vector<std::vector<uint8_t>>;

// `count` rows from where the bitmap stands, as drawBitmap() reads them.
Rows readRows(const Bitmap& bitmap, const int count) {
  Rows rows;
  std::vector<uint8_t> raw(static_cast<size_t>(bitmap.getRowBytes()));
  for (int i = 0; i < count; ++i) {
    std::vector<uint8_t> row(static_cast<size_t>((bitmap.getWidth() + 3) / 4), 0);
    EXPECT_EQ(bitmap.readNextRow(row.data(), raw.data()), BmpReaderError::Ok) << "row " << i;
    rows.push_back(std::move(row));
  }
  return rows;
}

class BitmapRowCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("bitmap_row_cache_" + std::to_string(getpid()));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    bmpPath_ = (dir_ / "cover.bmp").string();
    cachePath_ = (dir_ / ".cover.bmp.t0.rows").string();
    writeGrayBmp(bmpPath_, kWidth, kHeight, 1);
    savedThreshold_ = gray4tuning::thresholds[0];
  }
  void TearDown() override {
    gray4tuning::thresholds[0] = savedThreshold_;
    fs::remove_all(dir_);
  }

  // Every row a plain decode makes, as every pass without a cache gets them.
  Rows decoded(const BitmapToneMapping tone = BitmapToneMapping::None) {
    FsFile file;
    EXPECT_TRUE(Storage.openFileForRead("T", bmpPath_, file));
    Bitmap bitmap(file, true, tone);
    EXPECT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    return readRows(bitmap, bitmap.getHeight());
  }

  // Records a cache for the file as it stands: one whole pass, then finish().
  void record(const BitmapToneMapping tone = BitmapToneMapping::None) {
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("T", bmpPath_, file));
    Bitmap bitmap(file, true, tone);
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    BitmapRowCache cache;
    ASSERT_TRUE(cache.attach(bitmap, cachePath_));
    ASSERT_TRUE(cache.recording());
    readRows(bitmap, bitmap.getHeight());
    cache.finish();
    ASSERT_TRUE(fs::exists(cachePath_));
  }

  // Attaches again; true when it replays. The rows it hands out must be a decode's either way, and a recording it
  // makes is published.
  bool replays(const BitmapToneMapping tone = BitmapToneMapping::None) {
    const Rows expected = decoded(tone);
    FsFile file;
    EXPECT_TRUE(Storage.openFileForRead("T", bmpPath_, file));
    Bitmap bitmap(file, true, tone);
    EXPECT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    BitmapRowCache cache;
    EXPECT_TRUE(cache.attach(bitmap, cachePath_));
    const bool replaying = cache.replaying();
    EXPECT_EQ(readRows(bitmap, bitmap.getHeight()), expected);
    cache.finish();
    return replaying;
  }

  // Width 61: not a whole number of bytes of 2-bit pixels. 300 rows of 16 bytes: more than one buffer of them.
  static constexpr int kWidth = 61;
  static constexpr int kHeight = 300;
  fs::path dir_;
  std::string bmpPath_;
  std::string cachePath_;
  uint8_t savedThreshold_ = 0;
};

TEST_F(BitmapRowCacheTest, RecordsAPassStoppedEarlyAndReplaysEveryRow) {
  const Rows expected = decoded();
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("T", bmpPath_, file));
  Bitmap bitmap(file, true);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  BitmapRowCache cache;
  ASSERT_TRUE(cache.attach(bitmap, cachePath_));
  ASSERT_TRUE(cache.recording());

  // A pass that stops before the last row, as drawBitmap() does for rows past the screen's edge.
  const Rows first = readRows(bitmap, 123);
  for (int i = 0; i < 123; ++i) ASSERT_EQ(first[i], expected[i]) << "row " << i;
  ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
  ASSERT_TRUE(cache.replaying());
  ASSERT_TRUE(fs::exists(cachePath_));
  EXPECT_FALSE(fs::exists(cachePath_ + ".part"));

  for (int pass = 0; pass < 2; ++pass) {
    EXPECT_EQ(readRows(bitmap, kHeight), expected) << "pass " << pass;
    ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
  }
  cache.finish();
}

TEST_F(BitmapRowCacheTest, ALaterSleepReplaysWhatAnEarlierOneRecorded) {
  record();  // one pass and no rewind: the 1-bit filters' single B/W plane
  EXPECT_TRUE(replays());
}

TEST_F(BitmapRowCacheTest, TopDownRowsOfWholeBytesReplayToo) {
  writeGrayBmp(bmpPath_, 64, 37, 5, /*topDown=*/true);
  record();
  EXPECT_TRUE(replays());
}

TEST_F(BitmapRowCacheTest, AnUnfinishedRecordingLeavesNoFile) {
  {
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("T", bmpPath_, file));
    Bitmap bitmap(file, true);
    ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
    BitmapRowCache cache;
    ASSERT_TRUE(cache.attach(bitmap, cachePath_));
    readRows(bitmap, 40);
  }  // dropped without finish()
  EXPECT_FALSE(fs::exists(cachePath_));
  EXPECT_FALSE(fs::exists(cachePath_ + ".part"));
}

// The host's files have no clusters and keep no time a test can tell apart, so this is the pixel samples' doing.
TEST_F(BitmapRowCacheTest, AnotherPictureOfTheSameSizeIsRecordedAgain) {
  record();
  writeGrayBmp(bmpPath_, kWidth, kHeight, 2);
  EXPECT_FALSE(replays());
}

TEST_F(BitmapRowCacheTest, AnotherToneMappingIsRecordedAgain) {
  ASSERT_NE(decoded(BitmapToneMapping::Adaptive), decoded());  // the stretch changes the rows
  record();
  EXPECT_FALSE(replays(BitmapToneMapping::Adaptive));
}

TEST_F(BitmapRowCacheTest, AnotherPanelTableIsRecordedAgain) {
  record();
  gray4tuning::thresholds[0] = static_cast<uint8_t>(gray4tuning::thresholds[0] + 7);
  EXPECT_FALSE(replays());
}

TEST_F(BitmapRowCacheTest, ATruncatedCacheIsRecordedAgain) {
  record();
  fs::resize_file(cachePath_, fs::file_size(cachePath_) - 5);
  EXPECT_FALSE(replays());
  EXPECT_TRUE(replays());  // and the new one is whole
}

// 400 x 300: 100-byte rows, 40 a read. The card loses the cache from row 100 on under an open replay, so the third
// read (rows 80..119) comes up short whatever the host's stdio has read ahead before.
TEST_F(BitmapRowCacheTest, AReadFailurePartwayIsReportedAndDropsTheCache) {
  writeGrayBmp(bmpPath_, 400, 300, 3);
  record();
  const Rows expected = decoded();
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("T", bmpPath_, file));
  Bitmap bitmap(file, true);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  BitmapRowCache cache;
  ASSERT_TRUE(cache.attach(bitmap, cachePath_));
  ASSERT_TRUE(cache.replaying());
  fs::resize_file(cachePath_, 16 + 100 * 100);

  std::vector<uint8_t> row(100);
  std::vector<uint8_t> raw(static_cast<size_t>(bitmap.getRowBytes()));
  int served = 0;
  while (served < 300 && bitmap.readNextRow(row.data(), raw.data()) == BmpReaderError::Ok) {
    ASSERT_EQ(row, expected[static_cast<size_t>(served)]) << "row " << served;
    served++;
  }
  EXPECT_EQ(served, 80);
  EXPECT_TRUE(cache.takeReplayFailure());
  EXPECT_FALSE(cache.takeReplayFailure());  // once
  EXPECT_FALSE(fs::exists(cachePath_));

  // The plane is drawn again from the BMP, as SleepActivity does.
  ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
  EXPECT_EQ(readRows(bitmap, 300), expected);
}

// The BMP loses its rows from 100 on while a pass records it: the recording must not go on from wherever the failed
// read left the file, so nothing is published.
TEST_F(BitmapRowCacheTest, AReadErrorWhileRecordingPublishesNothing) {
  writeGrayBmp(bmpPath_, 400, 300, 4);
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("T", bmpPath_, file));
  Bitmap bitmap(file, true);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  BitmapRowCache cache;
  ASSERT_TRUE(cache.attach(bitmap, cachePath_));
  readRows(bitmap, 50);
  fs::resize_file(bmpPath_, kGrayBmpPixelOffset + 100 * 400);

  std::vector<uint8_t> row(100);
  std::vector<uint8_t> raw(static_cast<size_t>(bitmap.getRowBytes()));
  int read = 50;
  while (read < 300 && bitmap.readNextRow(row.data(), raw.data()) == BmpReaderError::Ok) read++;
  EXPECT_EQ(read, 100);
  EXPECT_FALSE(cache.recording());
  ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
  cache.finish();
  EXPECT_FALSE(fs::exists(cachePath_));
  EXPECT_FALSE(fs::exists(cachePath_ + ".part"));
}

TEST_F(BitmapRowCacheTest, EightBitSamplesDropARecording) {
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("T", bmpPath_, file));
  Bitmap bitmap(file, true);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  BitmapRowCache cache;
  ASSERT_TRUE(cache.attach(bitmap, cachePath_));
  std::vector<uint8_t> raw(static_cast<size_t>(bitmap.getRowBytes()));
  std::vector<uint8_t> gray(kWidth);
  ASSERT_EQ(bitmap.readNextRow(nullptr, raw.data(), gray.data()), BmpReaderError::Ok);
  EXPECT_FALSE(cache.recording());
  EXPECT_FALSE(fs::exists(cachePath_ + ".part"));
}

}  // namespace
