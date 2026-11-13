#pragma once

#include <cstdint>
#include <string>

struct RecentBook;

// What the recent-books list asks of the books themselves: their metadata sidecars and their cache folders. Apart from
// RecentBooksStore so the store builds without the book formats (and a host test can stand in for them).
namespace RecentBookMetadata {

// The metadata sidecar's stamp a book is listed with when it is opened (SidecarFiles::metadataStamp): the EPUB and
// text readers pass metadata they have just loaded, sidecar applied. An XTC is left unstamped (0): its reader passes
// the book's own header, so its sidecar, if any, is applied by refreshFromSidecar().
uint32_t openStamp(const std::string& path);

// The book's title, author and series again where its metadata sidecar was added, edited or removed since they were
// taken. True when they changed. The stamp stays stale when the book does not load, so the next look tries again.
bool refreshFromSidecar(RecentBook& book);

// Where an EPUB keeps its own reader settings (its cache folder); "" for any other book, which has none.
std::string overridesCacheDir(const std::string& path);

}  // namespace RecentBookMetadata
