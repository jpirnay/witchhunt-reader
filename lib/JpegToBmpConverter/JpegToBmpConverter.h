#pragma once

#include <HalStorage.h>

class Print;
class ZipFile;

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true, bool eightBit = false);

 public:
  // grayscale8Bit: emit an 8-bit BMP instead of quantizing to the display's four
  // levels here, deferring dithering to draw time. Only worth it for a consumer
  // that can use the extra tonal range (the sleep screen's adaptive tone filter);
  // it costs 4x the SD footprint of a 2-bit cover.
  static bool jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop = true, bool grayscale8Bit = false);
  // Convert with custom target size (for thumbnails)
  static bool jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};
