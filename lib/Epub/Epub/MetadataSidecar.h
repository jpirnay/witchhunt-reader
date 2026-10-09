#pragma once

#include <cstdint>
#include <string>

// What a Calibre-style metadata sidecar ("Book.opf" beside "Book.epub") says about its book: the
// fields the firmware lets it override (docs/sidecar-files.md). Empty means "not supplied", and an
// empty field never blanks the book's own value.
struct MetadataSidecarFields {
  std::string title;
  std::string author;         // every dc:creator, joined for display
  std::string primaryAuthor;  // the first creator credited as author
  std::string authorSort;     // its opf:file-as
  std::string language;
  std::string series;
  std::string seriesIndex;
  std::string description;
};

namespace MetadataSidecar {

enum class Result : uint8_t {
  Read,         // `out` holds what the sidecar says (a malformed one may say nothing)
  None,         // there is no sidecar beside the book
  Ignored,      // there is one, but it is empty or larger than Epub::MAX_METADATA_SIDECAR_BYTES
  Unavailable,  // there is one, but it could not be read just now (out of memory, open failed)
};

// Reads the metadata sidecar beside `bookPath`, whatever the book's format: the same OPF parser the
// book's own metadata goes through, over a plain file, with no ZIP and no manifest. Anything but
// Read leaves the book with its own metadata. Unavailable is the one answer that may change by
// itself, so a caller that records what it learns must not record it.
Result read(const std::string& bookPath, MetadataSidecarFields& out);

// Lays the sidecar's primary author over a book's. The filing name travels with the name it files:
// a sidecar naming a different author brings its own opf:file-as (or none), and the book's is kept
// only when the sidecar names the same author and gives no file-as of its own.
void overlayPrimaryAuthor(const MetadataSidecarFields& sidecar, std::string& primaryAuthor, std::string& authorSort);

}  // namespace MetadataSidecar
