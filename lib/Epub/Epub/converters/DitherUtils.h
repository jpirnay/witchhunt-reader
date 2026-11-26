#pragma once

#include <stdint.h>

#include <cstring>
#include <new>

// 4x4 Bayer matrix for ordered dithering
inline constexpr uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

constexpr uint8_t quantizeGray4Level(uint8_t gray) { return gray >= 213 ? 3 : static_cast<uint8_t>((gray + 42) / 85); }

namespace bayer4_detail {

// A cell's offset, (bayer - 8) * 5, is -40..35: about half the quantization step of 85 either way.
constexpr int kLowestOffset = -40;
constexpr int kOffsetSpan = 75;

// What applyBayerDither4Level() reads. The level of clamp(gray + offset) depends on the sum alone: quantizeGray4Level()
// steps at 43, 128 and 213, all inside 0..255, so the clamp never moves a sum across a step. So a cell keeps its
// offset, shifted to 0..75, and `level` holds the level of every sum from -40 to 290, shifted the same way: per pixel
// two table reads, where the clamps and the quantization took compares, branches and a division by 85.
struct Tables {
  uint8_t offset[4][4];
  uint8_t level[256 + kOffsetSpan];
};

constexpr Tables makeTables() {
  Tables t{};
  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      t.offset[y][x] = static_cast<uint8_t>((bayer4x4[y][x] - 8) * 5 - kLowestOffset);
    }
  }
  for (int i = 0; i < 256 + kOffsetSpan; i++) {
    int sum = i + kLowestOffset;
    if (sum < 0) sum = 0;
    if (sum > 255) sum = 255;
    t.level[i] = quantizeGray4Level(static_cast<uint8_t>(sum));
  }
  return t;
}

inline constexpr Tables kTables = makeTables();

}  // namespace bayer4_detail

// Apply Bayer dithering and quantize to 4 levels (0-3): quantizeGray4Level(clamp(gray + (bayer - 8) * 5)).
// Stateless - works correctly with any pixel processing order
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  return bayer4_detail::kTables.level[gray + bayer4_detail::kTables.offset[y & 3][x & 3]];
}

#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
class DiffusedBayerDitherer {
 public:
  explicit DiffusedBayerDitherer(int width) : width(width) {
    const size_t stride = static_cast<size_t>(width + 2);
    errorRows = new (std::nothrow) int16_t[stride * 2]();
    if (errorRows) {
      errorCurRow = errorRows;
      errorNextRow = errorRows + stride;
    }
  }

  ~DiffusedBayerDitherer() { delete[] errorRows; }

  DiffusedBayerDitherer(const DiffusedBayerDitherer&) = delete;
  DiffusedBayerDitherer& operator=(const DiffusedBayerDitherer&) = delete;

  uint8_t processPixel(int gray, int x, int screenX, int screenY) {
    int adjusted = gray + (errorRows ? errorCurRow[x + 1] : 0);
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;

    int thresholdAdjusted = adjusted + (bayer4x4[screenY & 3][screenX & 3] - 8) * 5;
    if (thresholdAdjusted < 0) thresholdAdjusted = 0;
    if (thresholdAdjusted > 255) thresholdAdjusted = 255;

    uint8_t quantized = quantizeGray4Level((uint8_t)thresholdAdjusted);
    if (!errorRows) return quantized;
    int quantizedValue = quantized * 85;
    int error = adjusted - quantizedValue;

    errorCurRow[x + 2] += (error * 7) / 16;
    errorNextRow[x] += (error * 3) / 16;
    errorNextRow[x + 1] += (error * 5) / 16;
    errorNextRow[x + 2] += error / 16;

    return quantized;
  }

  void nextRow() {
    if (!errorRows) return;
    int16_t* tmp = errorCurRow;
    errorCurRow = errorNextRow;
    errorNextRow = tmp;
    memset(errorNextRow, 0, (width + 2) * sizeof(int16_t));
  }

  void reset() {
    if (!errorRows) return;
    memset(errorCurRow, 0, (width + 2) * sizeof(int16_t));
    memset(errorNextRow, 0, (width + 2) * sizeof(int16_t));
  }

 private:
  int width;
  int16_t* errorRows{nullptr};
  int16_t* errorCurRow{nullptr};
  int16_t* errorNextRow{nullptr};
};
#endif
