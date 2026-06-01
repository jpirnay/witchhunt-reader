#include "ImageDecoderFactory.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <ZipFile.h>

#include <memory>
#include <string>

#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder = nullptr;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder = nullptr;

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  std::string ext = imagePath;
  size_t dotPos = ext.rfind('.');
  if (dotPos != std::string::npos) {
    ext = ext.substr(dotPos);
    for (auto& c : ext) {
      c = tolower(c);
    }
  } else {
    ext = "";
  }

  if (JpegToFramebufferConverter::supportsFormat(ext)) {
    if (!jpegDecoder) {
      jpegDecoder.reset(new JpegToFramebufferConverter());
    }
    return jpegDecoder.get();
  } else if (PngToFramebufferConverter::supportsFormat(ext)) {
    if (!pngDecoder) {
      pngDecoder.reset(new PngToFramebufferConverter());
    }
    return pngDecoder.get();
  }

  LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) { return getDecoder(imagePath) != nullptr; }

bool ImageDecoderFactory::getDimensionsFromZipEntry(const std::string& epubFilePath, const std::string& entryPath,
                                                    ImageDimensions& out) {
  // JPEG: SOF marker is typically within the first 4 KB.
  // PNG: IHDR is always in the first 24 bytes.
  // We read up to 4 KB to cover both formats; for PNG this wastes a few bytes but is harmless.
  constexpr size_t kHeaderBufSize = 4 * 1024;
  uint8_t buf[kHeaderBufSize];

  const std::string normalised = FsHelpers::normalisePath(entryPath);
  const size_t bytesRead = ZipFile(epubFilePath).readBytesFromEntry(normalised.c_str(), buf, kHeaderBufSize);
  if (bytesRead == 0) {
    LOG_ERR("DEC", "getDimensionsFromZipEntry: failed to read header from %s:%s", epubFilePath.c_str(),
            entryPath.c_str());
    return false;
  }

  // Try JPEG first (most common for EPUBs)
  if (bytesRead >= 2 && buf[0] == 0xFF && buf[1] == 0xD8) {
    return JpegToFramebufferConverter::getDimensionsFromBuffer(buf, bytesRead, out);
  }
  // Try PNG
  if (bytesRead >= 8 && buf[0] == 0x89 && buf[1] == 0x50) {
    return PngToFramebufferConverter::getDimensionsFromBuffer(buf, bytesRead, out);
  }

  LOG_ERR("DEC", "getDimensionsFromZipEntry: unrecognised format for %s", entryPath.c_str());
  return false;
}
