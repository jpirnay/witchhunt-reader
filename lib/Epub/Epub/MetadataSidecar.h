#pragma once

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

// Reads the metadata sidecar beside `bookPath`, whatever the book's format: the same OPF parser the
// book's own metadata goes through, over a plain file, with no ZIP and no manifest. False when there
// is none, or it is empty or larger than Epub::MAX_METADATA_SIDECAR_BYTES, or it cannot be read. A
// malformed one yields no usable fields. Either way the book keeps its own metadata.
bool read(const std::string& bookPath, MetadataSidecarFields& out);

// Lays the sidecar's primary author over a book's. The filing name travels with the name it files:
// a sidecar naming a different author brings its own opf:file-as (or none), and the book's is kept
// only when the sidecar names the same author and gives no file-as of its own.
void overlayPrimaryAuthor(const MetadataSidecarFields& sidecar, std::string& primaryAuthor, std::string& authorSort);

}  // namespace MetadataSidecar
