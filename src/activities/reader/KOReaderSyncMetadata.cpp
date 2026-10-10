#include "KOReaderSyncMetadata.h"

#include <HalStorage.h>

#include <utility>

#include "activities/home/BookDetails.h"

namespace KOReaderSyncMetadata {

KOReaderMetadata forBook(const std::string& bookPath, const std::string& title, const std::string& authors) {
  KOReaderMetadata meta;
  const size_t slash = bookPath.rfind('/');
  meta.filename = slash == std::string::npos ? bookPath : bookPath.substr(slash + 1);
  meta.title = title;
  meta.authors = authors;

  // The book's size, so a details.bin recorded for another book copied over the same name is not used.
  uint32_t size = 0;
  if (HalFile book = Storage.open(bookPath.c_str())) {
    size = static_cast<uint32_t>(book.fileSize());
    book.close();
  }
  BookDetails details;
  if (size > 0 && BookDetailsLookup::cached(bookPath, size, details)) {
    meta.isbn = std::move(details.isbn);
    meta.asin = std::move(details.asin);
    meta.series = std::move(details.series);
    meta.seriesIndex = std::move(details.seriesIndex);
  }
  return meta;
}

}  // namespace KOReaderSyncMetadata
