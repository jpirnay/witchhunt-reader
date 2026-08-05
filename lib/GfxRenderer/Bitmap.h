#pragma once

#include <HalStorage.h>

#include <cstdint>

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

// Adaptive: derive black/white points from the image's own luminance histogram
// and stretch toward them before dithering, for images whose useful tonal range
// sits in a narrow band. Implies the Native quantizer -- see analyzeAdaptiveTone().
enum class BitmapToneMapping : uint8_t { None, Adaptive };

class Bitmap {
 public:
  static const char* errorToString(BmpReaderError err);

  explicit Bitmap(FsFile& file, bool dithering = false, BitmapToneMapping toneMapping = BitmapToneMapping::None)
      : file(file), dithering(dithering), toneMapping(toneMapping) {}
  ~Bitmap();
  BmpReaderError parseHeaders();
  BmpReaderError readNextRow(uint8_t* data, uint8_t* rowBuffer) const;
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
  bool adaptiveToneActive = false;
  uint8_t adaptiveBlackPoint = 0;
  uint8_t adaptiveWhitePoint = 255;
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
