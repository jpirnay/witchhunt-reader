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

// A 4-level error-diffusion quantizer needs two things, and they are not the same thing:
//
//   - THRESHOLDS decide which level a pixel lands on. This is where a display tuning
//     belongs: the X4's greys read darker than their nominal value, so pushing the
//     thresholds down (30/50/140 rather than the even 43/128/213) promotes pixels a
//     level and compensates. That is a deliberate, output-referred brightening.
//   - REPRESENTED VALUES are the feedback term: `error = wanted - value` is what the
//     ditherer pushes to the neighbouring pixels. They must describe how far apart the
//     levels are, because that spacing is what the error is measured against.
//
// These were conflated. DisplayTuned used to report 15/30/80/210 as its values, on the
// reasoning that they encode how dark each level really renders. As a description of the
// panel that may well be right, but as a feedback term it is ruinous: three of the four
// levels sit inside the bottom 80 and there is a 130-wide gap below white. Any image
// whose mass sits in 50..140 -- 76% of the pixels on a typical low-contrast cover -- then
// has only levels 2 and 3 to work with, 130 apart, and comes back as coarse speckle with
// its midtone structure gone. Measured as the correlation between the source and the
// rendered level map (both box-averaged 4x4, so the metric does not depend on what the
// panel does with each level), 15/30/80/210 lost structure on all seven sample covers,
// r = 0.87-0.98 where even spacing held r > 0.99 -- worst exactly where tone mapping had
// widened the midtones first, which is why this surfaced as "the adaptive filter washes
// covers out".
//
// So both modes now feed error diffusion the even 0/85/170/255 spacing, and differ only
// in their thresholds -- which is the half a display tuning was ever meant to touch:
//   DisplayTuned: X4-tuned thresholds 30/50/140. The brightening is preserved.
//   Native: even thresholds 43/128/213, the untuned midpoints.
// Note: `Native` matches quantizeGray4Level() in Epub/converters/DitherUtils.h, which
// serves the reader's in-book image path -- and that path really does use it, because
// the AtkinsonDitherer in JpegToFramebufferConverter sits inside
// ENABLE_IMAGE_DITHERING_EXTENSION, which no build defines. Kept as separate enumerators
// because quantizeGray4Level returns only an index and has no threshold counterpart.
enum class Gray4QuantizationMode : uint8_t { DisplayTuned, Native };

inline QuantizedGray4 quantizeGray4(int gray, const Gray4QuantizationMode mode) {
  if (gray < 0) gray = 0;
  if (gray > 255) gray = 255;

  if (mode == Gray4QuantizationMode::Native) {
    // Untuned midpoints between the four levels.
    if (gray < 43) return {0, 0};
    if (gray < 128) return {1, 85};
    if (gray < 213) return {2, 170};
    return {3, 255};
  }

  // X4-tuned thresholds; same level spacing fed back to the diffuser (see above).
  if (gray < 30) return {0, 0};
  if (gray < 50) return {1, 85};
  if (gray < 140) return {2, 170};
  return {3, 255};
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
  explicit AtkinsonDitherer(int width, Gray4QuantizationMode quantizationMode = Gray4QuantizationMode::DisplayTuned)
      : width(width), quantizationMode(quantizationMode) {
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

    const QuantizedGray4 quantized = quantizeGray4(adjusted, quantizationMode);

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
  Gray4QuantizationMode quantizationMode;
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
  explicit FloydSteinbergDitherer(int width,
                                  Gray4QuantizationMode quantizationMode = Gray4QuantizationMode::DisplayTuned)
      : width(width), quantizationMode(quantizationMode), rowCount(0) {
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

    const QuantizedGray4 quantized = quantizeGray4(adjusted, quantizationMode);

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
  Gray4QuantizationMode quantizationMode;
  int rowCount;
  int16_t* errorRows{nullptr};
  int16_t* errorCurRow{nullptr};
  int16_t* errorNextRow{nullptr};
};
