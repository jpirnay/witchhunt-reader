#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "ImageToFramebufferDecoder.h"

class JpegToFramebufferConverter;
class PngToFramebufferConverter;
class GifToFramebufferConverter;
class Epub;

class ImageDecoderFactory {
 public:
  // Returns non-owning pointer - factory owns the decoder lifetime
  static ImageToFramebufferDecoder* getDecoder(const std::string& imagePath);
  static bool isFormatSupported(const std::string& imagePath);

  // Read image dimensions directly from a ZIP entry header — no SD extraction needed.
  // epubFilePath is the .epub file on SD; entryPath is the internal EPUB path (e.g. "OEBPS/images/foo.jpg").
  // Returns false if the format is unsupported or the header is unreadable.
  static bool getDimensionsFromZipEntry(const std::string& epubFilePath, const std::string& entryPath,
                                        ImageDimensions& out);

 private:
  static std::unique_ptr<JpegToFramebufferConverter> jpegDecoder;
  static std::unique_ptr<PngToFramebufferConverter> pngDecoder;
  static std::unique_ptr<GifToFramebufferConverter> gifDecoder;
};
