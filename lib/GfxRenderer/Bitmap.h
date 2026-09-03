#pragma once

#include <HalStorage.h>

#include <cstdint>

#include "AdaptiveTone.h"
#include "BitmapHelpers.h"

#pragma pack(push, 1)
struct BmpHeader {
  struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
  } fileHeader;
  struct {
    uint32_t biSize;
    int32_t biWidth;
    int32_t biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t biXPelsPerMeter;
    int32_t biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
  } infoHeader;
  struct RgbQuad {
    uint8_t rgbBlue;
    uint8_t rgbGreen;
    uint8_t rgbRed;
    uint8_t rgbReserved;
  };
  RgbQuad colors[2];
};
#pragma pack(pop)

enum class BmpReaderError : uint8_t {
  Ok = 0,
  FileInvalid,
  SeekStartFailed,

  NotBMP,
  DIBTooSmall,

  BadPlanes,
  UnsupportedBpp,
  UnsupportedCompression,

  BadDimensions,
  ImageTooLarge,
  PaletteTooLarge,

  SeekPixelDataFailed,
  BufferTooSmall,
  OomRowBuffer,
  ShortReadRow,
};

// Adaptive: derive black/white points from the image's own luminance histogram and
// stretch toward them before dithering, for images whose useful tonal range sits in a
// narrow band.
// Equalize: map through the histogram's own CDF instead, which also reaches images
// whose percentiles already span the range but whose mass does not (see AdaptiveTone.h).
// Both feed the DisplayTuned quantizer, like every other path -- see analyzeAdaptiveTone().
enum class BitmapToneMapping : uint8_t { None, Adaptive, Equalize };

// The analysis mode a tone-mapping selection implies. None never reaches derivePoints().
inline adaptive_tone::Mode toneAnalysisMode(const BitmapToneMapping toneMapping) {
  return toneMapping == BitmapToneMapping::Equalize ? adaptive_tone::Mode::Equalize : adaptive_tone::Mode::Stretch;
}

class Bitmap {
 public:
  static const char* errorToString(BmpReaderError err);

  // `eqBlendNum` reaches adaptive_tone::derivePoints for Equalize analysis; leave
  // it defaulted unless the image is bound for a panel that resolves more than the
  // dual-plane four levels. See EQ_BLEND_NUM_DEEP.
  explicit Bitmap(FsFile& file, bool dithering = false, BitmapToneMapping toneMapping = BitmapToneMapping::None,
                  int eqBlendNum = adaptive_tone::EQ_BLEND_NUM)
      : file(file), dithering(dithering), toneMapping(toneMapping), eqBlendNum(eqBlendNum) {}
  ~Bitmap();
  BmpReaderError parseHeaders();
  // `data` receives the row packed at 2 bits per pixel, as always.
  //
  // `gray8Row`, when non-null, ALSO receives the tone-mapped 8-bit luminance of
  // every pixel -- the sample each 2-bit value is quantised from, one byte per
  // pixel, `width` bytes. Panels that resolve more than four levels consume that
  // instead and quantise for themselves at their own depth. Purely additive: the
  // 2-bit output and the dither state are byte-for-byte what they were without it.
  BmpReaderError readNextRow(uint8_t* data, uint8_t* rowBuffer, uint8_t* gray8Row = nullptr) const;
  BmpReaderError rewindToData() const;
  int getWidth() const { return width; }
  int getHeight() const { return height; }
  bool isTopDown() const { return topDown; }
  bool hasGreyscale() const { return bpp > 1; }
  int getRowBytes() const { return rowBytes; }
  bool is1Bit() const { return bpp == 1; }
  uint16_t getBpp() const { return bpp; }
  // True if the file actually contains every declared pixel row, i.e. it was not truncated by an
  // interrupted/aborted write (a partial thumbnail left on the SD card after a reboot mid-decode).
  // Call after parseHeaders() returns Ok. Cheap: compares file size against the pixel-data offset
  // plus rowBytes*height; readNextRow() would otherwise fail with ShortReadRow partway through.
  bool isComplete() const {
    const long need = static_cast<long>(bfOffBits) + static_cast<long>(rowBytes) * static_cast<long>(height);
    return static_cast<long>(file.size()) >= need;
  }

 private:
  static uint16_t readLE16(FsFile& f);
  static uint32_t readLE32(FsFile& f);
  bool analyzeAdaptiveTone();
  uint8_t applyAdaptiveTone(uint8_t luminance) const;

  FsFile& file;
  bool dithering = false;
  BitmapToneMapping toneMapping = BitmapToneMapping::None;
  int eqBlendNum = adaptive_tone::EQ_BLEND_NUM;
  adaptive_tone::Points adaptiveTonePoints;
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint32_t bfOffBits = 0;
  uint16_t bpp = 0;
  uint32_t colorsUsed = 0;
  bool nativePalette = false;  // true if all palette entries map to native gray levels
  int rowBytes = 0;
  uint8_t paletteLum[256] = {};

  // Dithering state (mutable for const methods)
  mutable int16_t* errorCurRow = nullptr;
  mutable int16_t* errorNextRow = nullptr;
  mutable int prevRowY = -1;  // Track row progression for error propagation

  mutable AtkinsonDitherer* atkinsonDitherer = nullptr;
  mutable FloydSteinbergDitherer* fsDitherer = nullptr;
};
