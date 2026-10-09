#include "MetadataSidecar.h"

#include <HalStorage.h>
#include <Logging.h>
#include <SidecarFiles.h>

#include "parsers/ContentOpfParser.h"

namespace MetadataSidecar {

Result read(const std::string& bookPath, MetadataSidecarFields& out) {
  const std::string path = SidecarFiles::metadataPath(bookPath);
  if (path.empty()) return Result::None;

  size_t size = 0;
  {
    HalFile probe;
    if (!Storage.openFileForRead("MSC", path, probe)) return Result::Unavailable;
    size = probe.fileSize();
  }
  if (size == 0 || size > Epub::MAX_METADATA_SIDECAR_BYTES) {
    LOG_DBG("MSC", "Ignoring metadata sidecar, %u bytes: %s", static_cast<unsigned>(size), path.c_str());
    return Result::Ignored;
  }

  // Null cache: a sidecar carries no real manifest or spine, so no item index must be built from it.
  // The parser holds both paths by reference, so they must outlive it.
  const std::string noCachePath;
  const std::string noContentBase;
  ContentOpfParser parser(noCachePath, noContentBase, size, nullptr);
  // The parser's ~10 KB state did not fit: the sidecar is there, the heap was short.
  if (!parser.setup()) return Result::Unavailable;
  // A stream that stops early is a document the parser rejected: as permanent as the file.
  if (!Storage.readFileToStream(path.c_str(), parser, 1024)) {
    LOG_DBG("MSC", "Could not read metadata sidecar: %s", path.c_str());
    return Result::Ignored;
  }

  out.title = std::move(parser.title);
  out.author = std::move(parser.author);
  out.primaryAuthor = std::move(parser.primaryAuthor);
  out.authorSort = std::move(parser.authorSort);
  out.language = std::move(parser.language);
  out.series = std::move(parser.series);
  out.seriesIndex = std::move(parser.seriesIndex);
  out.description = std::move(parser.description);
  return Result::Read;
}

void overlayPrimaryAuthor(const MetadataSidecarFields& sidecar, std::string& primaryAuthor, std::string& authorSort) {
  if (sidecar.primaryAuthor.empty()) return;
  if (!sidecar.authorSort.empty() || sidecar.primaryAuthor != primaryAuthor) authorSort = sidecar.authorSort;
  primaryAuthor = sidecar.primaryAuthor;
}

}  // namespace MetadataSidecar
