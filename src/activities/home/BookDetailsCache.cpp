#include <HalStorage.h>
#include <Serialization.h>

#include "BookDetails.h"

namespace BookDetailsCache {
namespace {
// Bumped whenever the layout below changes; an older file is then simply parsed again. 3 added the
// ISBN and ASIN.
constexpr uint8_t VERSION = 3;
}  // namespace

bool write(const std::string& path, const uint32_t bookSize, const uint32_t sidecarStamp, const BookDetails& details) {
  FsFile file;
  if (!Storage.openFileForWrite("BKD", path, file)) return false;
  serialization::writePod(file, VERSION);
  serialization::writePod(file, bookSize);
  serialization::writePod(file, sidecarStamp);
  serialization::writeString(file, details.title);
  serialization::writeString(file, details.author);
  serialization::writeString(file, details.series);
  serialization::writeString(file, details.seriesIndex);
  serialization::writeString(file, details.primaryAuthor);
  serialization::writeString(file, details.authorSort);
  serialization::writeString(file, details.isbn);
  serialization::writeString(file, details.asin);
  file.close();
  return true;
}

bool read(const std::string& path, const uint32_t bookSize, const uint32_t sidecarStamp, BookDetails& out) {
  FsFile file;
  if (!Storage.openFileForRead("BKD", path, file)) return false;
  uint8_t version = 0;
  uint32_t recordedSize = 0;
  uint32_t recordedStamp = 0;
  serialization::readPod(file, version);
  serialization::readPod(file, recordedSize);
  serialization::readPod(file, recordedStamp);
  const bool current =
      version == VERSION && (bookSize == 0 || recordedSize == bookSize) && recordedStamp == sidecarStamp;
  BookDetails details;
  const bool complete =
      current && serialization::readString(file, details.title) && serialization::readString(file, details.author) &&
      serialization::readString(file, details.series) && serialization::readString(file, details.seriesIndex) &&
      serialization::readString(file, details.primaryAuthor) && serialization::readString(file, details.authorSort) &&
      serialization::readString(file, details.isbn) && serialization::readString(file, details.asin);
  file.close();
  if (!complete) return false;
  out = std::move(details);
  return true;
}

}  // namespace BookDetailsCache
