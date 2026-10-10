#pragma once

#include <string>

#include "KOReaderSyncClient.h"

namespace KOReaderSyncMetadata {

// What Send Metadata puts beside a progress push: the book's file name, its title and authors as
// the reader shows them, and its ISBN, ASIN and series from details.bin. Those last four are read
// from details.bin and nothing else -- never an OPF parse -- because a push is built on a page turn
// or just before a TLS session; a book the Library has not described yet goes without them.
KOReaderMetadata forBook(const std::string& bookPath, const std::string& title, const std::string& authors);

}  // namespace KOReaderSyncMetadata
