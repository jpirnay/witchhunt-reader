#include "LibraryIndexReader.h"

#include <cstring>

#include "LibraryBlob.h"

namespace {

bool fits(const uint32_t offset, const uint64_t length, const uint32_t fileSize) {
  return uint64_t{offset} + length <= fileSize;
}

bool sectionsFit(const library::Header& h, const uint32_t size) {
  return h.bookCount <= library::MAX_BOOKS && h.newCount <= library::NEW_COUNT && h.newCount <= h.bookCount &&
         h.authorCount <= h.bookCount && h.recordsOff >= sizeof(library::Header) &&
         fits(h.recordsOff, uint64_t{h.bookCount} * sizeof(library::BookRecord), size) &&
         fits(h.newOff, uint64_t{h.newCount} * sizeof(uint16_t), size) &&
         fits(h.authorsOff, uint64_t{h.authorCount} * sizeof(library::AuthorRecord), size) &&
         fits(h.authorBooksOff, uint64_t{h.bookCount} * sizeof(uint16_t), size) && fits(h.blobOff, h.blobLen, size);
}

}  // namespace

bool LibraryIndexReader::open(const std::string& path) {
  close();
  attempted = true;
  if (!Storage.openFileForRead("LIB", path, file)) return false;
  library::Header h{};
  const auto size = static_cast<uint32_t>(file.fileSize());
  if (!readAt(0, &h, sizeof(h)) || std::memcmp(h.magic, library::MAGIC, sizeof(h.magic)) != 0 ||
      h.version != library::VERSION || !sectionsFit(h, size)) {
    close();
    return false;
  }
  hdr = h;
  opened = true;
  return true;
}

void LibraryIndexReader::close() {
  if (attempted) file.close();
  attempted = false;
  opened = false;
  hdr = {};
}

bool LibraryIndexReader::readAt(const uint32_t offset, void* out, const size_t len) {
  return file.seek(offset) && library::readExact(file, out, len);
}

bool LibraryIndexReader::book(const uint16_t index, library::BookRecord& out) {
  if (!opened || index >= hdr.bookCount) return false;
  return readAt(hdr.recordsOff + uint32_t{index} * sizeof(library::BookRecord), &out, sizeof(out));
}

bool LibraryIndexReader::newBook(const uint16_t rank, uint16_t& recordIndex) {
  if (!opened || rank >= hdr.newCount) return false;
  return readAt(hdr.newOff + uint32_t{rank} * sizeof(uint16_t), &recordIndex, sizeof(recordIndex)) &&
         recordIndex < hdr.bookCount;
}

bool LibraryIndexReader::author(const uint16_t index, library::AuthorRecord& out) {
  if (!opened || index >= hdr.authorCount) return false;
  return readAt(hdr.authorsOff + uint32_t{index} * sizeof(library::AuthorRecord), &out, sizeof(out));
}

bool LibraryIndexReader::authorBook(const uint32_t slot, uint16_t& recordIndex) {
  if (!opened || slot >= hdr.bookCount) return false;
  return readAt(hdr.authorBooksOff + slot * sizeof(uint16_t), &recordIndex, sizeof(recordIndex)) &&
         recordIndex < hdr.bookCount;
}

bool LibraryIndexReader::blobString(const uint32_t offset, std::string& out) {
  uint16_t len = 0;
  if (!opened || uint64_t{offset} + sizeof(len) > hdr.blobLen) return false;
  if (!readAt(hdr.blobOff + offset, &len, sizeof(len))) return false;
  if (len > library::MAX_STRING || uint64_t{offset} + sizeof(len) + len > hdr.blobLen) return false;
  out.resize(len);
  return library::readExact(file, len > 0 ? &out[0] : nullptr, len);
}

bool LibraryIndexReader::authorName(const library::AuthorRecord& author, std::string& name, std::string* key) {
  if (!blobString(author.nameOff, name)) return false;
  return key == nullptr || blobString(author.nameOff + sizeof(uint16_t) + static_cast<uint32_t>(name.size()), *key);
}
