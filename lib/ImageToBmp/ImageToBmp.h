#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>

class BuildArena;

// A JPEG or PNG made into a BMP file, the one way the firmware does it: the converter for the format,
// the target opened, both files closed, and the target removed when the conversion fails, so no
// screen finds half a BMP. Epub's and Txt's covers and thumbnails, the reader's sidecar covers, the
// Finished Book screen and Home's pictures each carried their own copy of this.
namespace ImageToBmp {

enum class Format : uint8_t { Jpeg, Png };

struct Target {
  // 0 x 0: the image at its own size as an 8-bit grey BMP, a cover the sleep screen dithers as it
  // draws. Otherwise a 1-bit BMP of at most width x height, a thumbnail drawn 1:1.
  int width = 0;
  int height = 0;
  bool crop = true;               // fill the size and cut what is over, rather than fit inside it
  BuildArena* scratch = nullptr;  // working memory for the JPEG decoder, where one is lent
};

// From `source`, open at the image's first byte; it is closed on return.
bool convert(const char* tag, FsFile& source, Format format, const std::string& targetPath, const Target& target);

// From the file at `sourcePath`, a JPEG or PNG by its extension. False for any other file and for one
// that cannot be opened, with nothing written.
bool convertFile(const char* tag, const std::string& sourcePath, const std::string& targetPath, const Target& target);

}  // namespace ImageToBmp
