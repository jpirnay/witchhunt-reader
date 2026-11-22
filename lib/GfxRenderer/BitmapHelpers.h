#pragma once

#include <cstdint>
#include <cstring>
#include <new>

struct BmpHeader;
class Print;

// Helper functions
uint8_t quantize(int gray, int x, int y);
uint8_t quantizeSimple(int gray);
uint8_t quantize1bit(int gray, int x, int y);
int adjustPixel(int gray);

// Result of a 4-level quantization: the packed 2-bit index written to the output
// stream, plus the luminance that index actually represents. Error-diffusion
// ditherers need the latter to compute the residual they push to neighbours.
struct QuantizedGray4 {
  uint8_t index;
  uint8_t value;
};

// A 4-level error-diffusion quantizer needs two things, and both belong to the panel:
//
//   - THRESHOLDS decide which level a pixel lands on.
//   - FEEDBACK values are what each level is taken to show: `error = wanted - value` is what the ditherer pushes to
//     the neighbouring pixels, so they must say where the panel's levels really sit. Atkinson carries 6/8 of the
//     error, so a uniform gray settles on a pure level wherever 4*gray - 3*feedback falls inside that level's band:
//     with feedback away from the panel's levels, no thresholds put a level where the panel shows it.
//
// The table is the running panel's, HalCapabilities::imageGray4Levels(), which setupDisplayAndFonts() copies here:
// each level's lightness measured against a reference card, fed back as the sRGB gray of that lightness. Until a
// panel is measured it keeps what every board had before, the even 0/85/170/255 with the X4-tuned 30/50/140. On
// Murphy that table drew uniform grays up to 8.6 L* too dark, and 128 darker than 112.
//
// An earlier change (8bc41a1fb) fed back guessed values, 15/30/80/210: three levels in the bottom 80 left a 130-wide
// gap below white, which midtones can only fill with coarse speckle, and covers lost their midtone structure. Its
// measure was the correlation between the source and the level-index map, which favours even spacing whatever the
// glass shows. A panel whose levels really are uneven either pays some of that speckle or shows its midtones too
// dark, so judge a table by the panel's lightness and by covers on the glass, not by the index map.
// (The reader's in-book image path uses the untuned midpoints, quantizeGray4Level() in
// Epub/converters/DitherUtils.h, which returns only an index.)
//
// Development builds' CMD:SLEEP:SCREEN:T=<a>,<b>,<c>:F=<a>,<b>,<c>,<d> overwrite the table to try values on the glass
// without a rebuild (docs/serial-test-api.md).
namespace gray4tuning {
inline uint8_t thresholds[3] = {30, 50, 140};
inline uint8_t feedback[4] = {0, 85, 170, 255};
}  // namespace gray4tuning

inline QuantizedGray4 quantizeGray4(int gray) {
  if (gray < 0) gray = 0;
  if (gray > 255) gray = 255;

  // The panel's table (see above).
  const uint8_t* t = gray4tuning::thresholds;
  const uint8_t* f = gray4tuning::feedback;
  if (gray < t[0]) return {0, f[0]};
  if (gray < t[1]) return {1, f[1]};
  if (gray < t[2]) return {2, f[2]};
  return {3, f[3]};
}

enum class BmpRowOrder { BottomUp, TopDown };

// Populates a 1-bit BMP header in the provided memory.
void createBmpHeader(BmpHeader* bmpHeader, int width, int height, BmpRowOrder rowOrder);

// Writes a top-down grayscale BMP header and palette. Returns the padded row
// size, or 0 when bitsPerPixel is not 1, 2, or 8.
int writeGrayscaleBmpHeader(Print& output, int width, int height, uint8_t bitsPerPixel);

// 1-bit Atkinson dithering - better quality than noise dithering for thumbnails
// Error distribution pattern (same as 2-bit but quantizes to 2 levels):
//     X  1/8 1/8
// 1/8 1/8 1/8
//     1/8
class Atkinson1BitDitherer {
 public:
  explicit Atkinson1BitDitherer(int width) : width(width) {
    const size_t stride = static_cast<size_t>(width + 4);
    errorRows = new (std::nothrow) int16_t[stride * 3]();
    if (errorRows) {
      errorRow0 = errorRows;
      errorRow1 = errorRows + stride;
      errorRow2 = errorRows + stride * 2;
    }
  }

  ~Atkinson1BitDitherer() { delete[] errorRows; }

  // The heap the constructor takes beside the object: its three error rows.
  static size_t errorRowBytesFor(const int width) { return static_cast<size_t>(width + 4) * 3 * sizeof(int16_t); }

  // EXPLICITLY DELETE THE COPY CONSTRUCTOR
  Atkinson1BitDitherer(const Atkinson1BitDitherer& other) = delete;

  // EXPLICITLY DELETE THE COPY ASSIGNMENT OPERATOR
  Atkinson1BitDitherer& operator=(const Atkinson1BitDitherer& other) = delete;

  uint8_t processPixel(int gray, int x) {
    // Apply brightness/contrast/gamma adjustments
    gray = adjustPixel(gray);

    // Add accumulated error
    int adjusted = gray + (errorRows ? errorRow0[x + 2] : 0);
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;

    // Quantize to 2 levels (1-bit): 0 = black, 1 = white
    uint8_t quantized;
    int quantizedValue;
    if (adjusted < 128) {
      quantized = 0;
      quantizedValue = 0;
    } else {
      quantized = 1;
      quantizedValue = 255;
    }

    if (!errorRows) return quantized;

    // Calculate error (only distribute 6/8 = 75%)
    int error = (adjusted - quantizedValue) >> 3;  // error/8

    // Distribute 1/8 to each of 6 neighbors
    errorRow0[x + 3] += error;  // Right
    errorRow0[x + 4] += error;  // Right+1
    errorRow1[x + 1] += error;  // Bottom-left
    errorRow1[x + 2] += error;  // Bottom
    errorRow1[x + 3] += error;  // Bottom-right
    errorRow2[x + 2] += error;  // Two rows down

    return quantized;
  }

  void nextRow() {
    if (!errorRows) return;
    int16_t* temp = errorRow0;
    errorRow0 = errorRow1;
    errorRow1 = errorRow2;
    errorRow2 = temp;
    memset(errorRow2, 0, (width + 4) * sizeof(int16_t));
  }

  void reset() {
    if (!errorRows) return;
    memset(errorRow0, 0, (width + 4) * sizeof(int16_t));
    memset(errorRow1, 0, (width + 4) * sizeof(int16_t));
    memset(errorRow2, 0, (width + 4) * sizeof(int16_t));
  }

  // The error it carries, for a decode that is checkpointed and resumed (JpegToFramebufferConverter):
  // rows 0 (current), 1 and 2, stateRowLength() entries each; 0 when the rows were never allocated.
  size_t stateRowLength() const { return errorRows ? static_cast<size_t>(width + 4) : 0; }
  int16_t* stateRow(const int i) { return i == 0 ? errorRow0 : i == 1 ? errorRow1 : errorRow2; }
  const int16_t* stateRow(const int i) const { return i == 0 ? errorRow0 : i == 1 ? errorRow1 : errorRow2; }

 private:
  int width;
  int16_t* errorRows{nullptr};
  int16_t* errorRow0{nullptr};
  int16_t* errorRow1{nullptr};
  int16_t* errorRow2{nullptr};
};

// Atkinson dithering - distributes only 6/8 (75%) of error for cleaner results
// Error distribution pattern:
//     X  1/8 1/8
// 1/8 1/8 1/8
//     1/8
// Less error buildup = fewer artifacts than Floyd-Steinberg
class AtkinsonDitherer {
 public:
  explicit AtkinsonDitherer(int width) : width(width) {
    const size_t stride = static_cast<size_t>(width + 4);
    errorRows = new (std::nothrow) int16_t[stride * 3]();
    if (errorRows) {
      errorRow0 = errorRows;
      errorRow1 = errorRows + stride;
      errorRow2 = errorRows + stride * 2;
    }
  }

  ~AtkinsonDitherer() { delete[] errorRows; }
  // **1. EXPLICITLY DELETE THE COPY CONSTRUCTOR**
  AtkinsonDitherer(const AtkinsonDitherer& other) = delete;

  // **2. EXPLICITLY DELETE THE COPY ASSIGNMENT OPERATOR**
  AtkinsonDitherer& operator=(const AtkinsonDitherer& other) = delete;

  uint8_t processPixel(int gray, int x) {
    // Add accumulated error
    int adjusted = gray + (errorRows ? errorRow0[x + 2] : 0);
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;

    const QuantizedGray4 quantized = quantizeGray4(adjusted);

    if (!errorRows) return quantized.index;

    // Calculate error (only distribute 6/8 = 75%)
    int error = (adjusted - quantized.value) >> 3;  // error/8

    // Distribute 1/8 to each of 6 neighbors
    errorRow0[x + 3] += error;  // Right
    errorRow0[x + 4] += error;  // Right+1
    errorRow1[x + 1] += error;  // Bottom-left
    errorRow1[x + 2] += error;  // Bottom
    errorRow1[x + 3] += error;  // Bottom-right
    errorRow2[x + 2] += error;  // Two rows down

    return quantized.index;
  }

  void nextRow() {
    if (!errorRows) return;
    int16_t* temp = errorRow0;
    errorRow0 = errorRow1;
    errorRow1 = errorRow2;
    errorRow2 = temp;
    memset(errorRow2, 0, (width + 4) * sizeof(int16_t));
  }

  void reset() {
    if (!errorRows) return;
    memset(errorRow0, 0, (width + 4) * sizeof(int16_t));
    memset(errorRow1, 0, (width + 4) * sizeof(int16_t));
    memset(errorRow2, 0, (width + 4) * sizeof(int16_t));
  }

 private:
  int width;
  int16_t* errorRows{nullptr};
  int16_t* errorRow0{nullptr};
  int16_t* errorRow1{nullptr};
  int16_t* errorRow2{nullptr};
};

// Floyd-Steinberg error diffusion dithering with serpentine scanning
// Serpentine scanning alternates direction each row to reduce "worm" artifacts
// Error distribution pattern (left-to-right):
//       X   7/16
// 3/16 5/16 1/16
// Error distribution pattern (right-to-left, mirrored):
// 1/16 5/16 3/16
//      7/16  X
class FloydSteinbergDitherer {
 public:
  explicit FloydSteinbergDitherer(int width) : width(width), rowCount(0) {
    const size_t stride = static_cast<size_t>(width + 2);
    errorRows = new (std::nothrow) int16_t[stride * 2]();
    if (errorRows) {
      errorCurRow = errorRows;
      errorNextRow = errorRows + stride;
    }
  }

  ~FloydSteinbergDitherer() { delete[] errorRows; }

  // **1. EXPLICITLY DELETE THE COPY CONSTRUCTOR**
  FloydSteinbergDitherer(const FloydSteinbergDitherer& other) = delete;

  // **2. EXPLICITLY DELETE THE COPY ASSIGNMENT OPERATOR**
  FloydSteinbergDitherer& operator=(const FloydSteinbergDitherer& other) = delete;

  // Process a single pixel and return quantized 2-bit value
  // x is the logical x position (0 to width-1), direction handled internally
  uint8_t processPixel(int gray, int x) {
    // Add accumulated error to this pixel
    int adjusted = gray + (errorRows ? errorCurRow[x + 1] : 0);

    // Clamp to valid range
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;

    const QuantizedGray4 quantized = quantizeGray4(adjusted);

    if (!errorRows) return quantized.index;

    // Calculate error
    int error = adjusted - quantized.value;

    // Distribute error to neighbors (serpentine: direction-aware)
    if (!isReverseRow()) {
      // Left to right: standard distribution
      // Right: 7/16
      errorCurRow[x + 2] += (error * 7) >> 4;
      // Bottom-left: 3/16
      errorNextRow[x] += (error * 3) >> 4;
      // Bottom: 5/16
      errorNextRow[x + 1] += (error * 5) >> 4;
      // Bottom-right: 1/16
      errorNextRow[x + 2] += (error) >> 4;
    } else {
      // Right to left: mirrored distribution
      // Left: 7/16
      errorCurRow[x] += (error * 7) >> 4;
      // Bottom-right: 3/16
      errorNextRow[x + 2] += (error * 3) >> 4;
      // Bottom: 5/16
      errorNextRow[x + 1] += (error * 5) >> 4;
      // Bottom-left: 1/16
      errorNextRow[x] += (error) >> 4;
    }

    return quantized.index;
  }

  // Call at the end of each row to swap buffers
  void nextRow() {
    if (!errorRows) return;
    // Swap buffers
    int16_t* temp = errorCurRow;
    errorCurRow = errorNextRow;
    errorNextRow = temp;
    // Clear the next row buffer
    memset(errorNextRow, 0, (width + 2) * sizeof(int16_t));
    rowCount++;
  }

  // Check if current row should be processed in reverse
  bool isReverseRow() const { return (rowCount & 1) != 0; }

  // Reset for a new image or MCU block
  void reset() {
    if (!errorRows) return;
    memset(errorCurRow, 0, (width + 2) * sizeof(int16_t));
    memset(errorNextRow, 0, (width + 2) * sizeof(int16_t));
    rowCount = 0;
  }

 private:
  int width;
  int rowCount;
  int16_t* errorRows{nullptr};
  int16_t* errorCurRow{nullptr};
  int16_t* errorNextRow{nullptr};
};
