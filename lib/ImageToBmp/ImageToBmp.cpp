#include "ImageToBmp.h"

#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <PngToBmpConverter.h>

namespace ImageToBmp {

bool convert(const char* tag, FsFile& source, const Format format, const std::string& targetPath,
             const Target& target) {
  FsFile out;
  if (!Storage.openFileForWrite(tag, targetPath, out)) {
    source.close();
    return false;
  }
  const bool sized = target.width > 0 && target.height > 0;
  bool ok = false;
  switch (format) {
    case Format::Jpeg:
      ok = sized ? JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(source, out, target.width, target.height,
                                                                       target.scratch, target.crop)
                 : JpegToBmpConverter::jpegFileToBmpStream(source, out, target.crop, /*grayscale8Bit=*/true,
                                                           target.scratch);
      break;
    case Format::Png:
      ok = sized ? PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(source, out, target.width, target.height,
                                                                     target.crop)
                 : PngToBmpConverter::pngFileToBmpStream(source, out, target.crop, /*grayscale8Bit=*/true);
      break;
  }
  source.close();
  out.close();
  if (!ok) Storage.remove(targetPath.c_str());
  return ok;
}

bool convertFile(const char* tag, const std::string& sourcePath, const std::string& targetPath, const Target& target) {
  Format format;
  if (FsHelpers::hasJpgExtension(sourcePath)) {
    format = Format::Jpeg;
  } else if (FsHelpers::hasPngExtension(sourcePath)) {
    format = Format::Png;
  } else {
    return false;
  }
  FsFile source;
  if (!Storage.openFileForRead(tag, sourcePath, source)) return false;
  return convert(tag, source, format, targetPath, target);
}

}  // namespace ImageToBmp
