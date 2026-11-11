#include "CardImageFile.h"

#include <HalStorage.h>
#include <Logging.h>

uint32_t CardImageFile::size() {
  if (!Storage.exists(path_)) return 0;
  HalFile file;
  if (!Storage.openFileForRead("FLA", path_, file)) return 0;
  return static_cast<uint32_t>(file.fileSize());
}

bool CardImageFile::read(const uint32_t offset, void* dst, const uint32_t len) {
  HalFile file;
  if (!Storage.openFileForRead("FLA", path_, file) || !file.seekSet(offset)) return false;
  return file.read(dst, len) == static_cast<int>(len);
}

bool CardImageFile::write(const uint32_t offset, const void* src, const uint32_t len) {
  HalFile file;
  if (!Storage.openFileForUpdate("FLA", path_, file) || !file.seekSet(offset)) return false;
  if (file.write(src, len) != len) {
    LOG_ERR("FLA", "%s: %u B at %u not written", path_, static_cast<unsigned>(len), static_cast<unsigned>(offset));
    return false;
  }
  // close(), not the destructor: its result says whether the bytes reached the card.
  return file.close();
}

bool CardImageFile::truncate() {
  HalFile file;
  if (!Storage.openFileForWrite("FLA", path_, file)) return false;
  return file.close();
}
