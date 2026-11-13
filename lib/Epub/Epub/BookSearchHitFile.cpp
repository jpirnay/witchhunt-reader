#include "BookSearchHitFile.h"

#include <Logging.h>

#include <cstring>

namespace BookSearch {
namespace {

void encode(const Hit& hit, uint8_t (&record)[HitFile::kRecordBytes]) {
  std::memcpy(record, &hit.spine, 2);
  std::memcpy(record + 2, &hit.chapterPermille, 2);
  std::memcpy(record + 4, &hit.offset, 4);
  record[8] = hit.snippetLength;
  std::memcpy(record + 9, hit.snippet, Hit::kSnippetBytes);
}

void decode(const uint8_t (&record)[HitFile::kRecordBytes], Hit& hit) {
  std::memcpy(&hit.spine, record, 2);
  std::memcpy(&hit.chapterPermille, record + 2, 2);
  std::memcpy(&hit.offset, record + 4, 4);
  hit.snippetLength = record[8] <= Hit::kSnippetBytes ? record[8] : static_cast<uint8_t>(Hit::kSnippetBytes);
  std::memcpy(hit.snippet, record + 9, Hit::kSnippetBytes);
}

}  // namespace

bool HitFile::create(const std::string& path) {
  if (open_) close();
  failed_ = false;
  written_ = 0;
  open_ = Storage.openFileForWrite("BSR", path, file_);
  if (!open_) LOG_ERR("BSR", "Cannot write the search's hits to %s", path.c_str());
  return open_;
}

void HitFile::add(const Hit& hit) {
  if (!open_ || failed_) return;
  uint8_t record[kRecordBytes];
  encode(hit, record);
  if (file_.write(record, kRecordBytes) != kRecordBytes) {
    failed_ = true;
    LOG_ERR("BSR", "Writing a search hit failed: the hits after it are not kept");
    return;
  }
  ++written_;
}

void HitFile::flush() {
  if (open_) file_.flush();
}

bool HitFile::close() {
  if (open_) {
    file_.close();
    open_ = false;
  }
  return !failed_;
}

size_t HitFile::read(const std::string& path, const size_t index, Hit* const out, const size_t n) {
  HalFile file;
  if (n == 0 || !Storage.openFileForRead("BSR", path, file)) return 0;
  size_t got = 0;
  if (file.seekSet(index * kRecordBytes)) {
    uint8_t record[kRecordBytes];
    while (got < n && file.read(record, kRecordBytes) == static_cast<int>(kRecordBytes)) decode(record, out[got++]);
  }
  return got;
}

}  // namespace BookSearch
