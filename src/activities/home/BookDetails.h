#pragma once

#include <cstdint>
#include <string>

class BuildArena;

// What the book lists show for a book: its title, author and series as the book's own metadata (or
// its .opf sidecar) gives them rather than as the filename spells them, and the author the Library
// groups it by.
//
// Getting them costs a metadata-only OPF parse (~300 ms on the device) for an EPUB and a header read
// for an XTC; a TXT or Markdown book has only its sidecar, if any. The result is kept beside the
// book's other cache files as details.bin, so after the first time every book on the card is a
// small file read away.
struct BookDetails {
  std::string title;
  std::string author;         // every author the book credits, for display
  std::string primaryAuthor;  // the first creator credited as author: what the Library groups by
  std::string authorSort;     // its opf:file-as ("Le Guin, Ursula K."); "" when the book gives none
  std::string series;
  std::string seriesIndex;
  // The book's ISBN and Amazon ASIN, for KOReader sync's metadata; empty when it gives none.
  std::string isbn;
  std::string asin;
};

namespace BookDetailsCache {

// details.bin in a book's cache directory. Records the book's size, so a different book copied
// over the same name is not shown under the old one's title, and the stamp of its .opf sidecar
// (SidecarFiles::metadataStamp), so an edited sidecar is read again.
//
// An EPUB is recorded only when its parse succeeded: one that failed may have failed for want of
// memory, and recording it would show the book by its filename for good.
bool write(const std::string& path, uint32_t bookSize, uint32_t sidecarStamp, const BookDetails& details);
// False when the file is missing, from another format version, or recorded for a different book
// size or sidecar stamp. A bookSize of 0 means "not known" and skips that check.
bool read(const std::string& path, uint32_t bookSize, uint32_t sidecarStamp, BookDetails& out);

}  // namespace BookDetailsCache

namespace BookDetailsLookup {

// The answer without parsing anything: from details.bin, or, for a TXT or Markdown book with no
// sidecar (titled by its filename), from nothing at all. False for a book that needs parse() first.
bool cached(const std::string& bookPath, uint32_t bookSize, BookDetails& out);

// The slow path for a book cached() could not answer: an EPUB's OPF (always the OPF, never book.bin,
// which keeps no primary author), an XTC's header, and for every format the .opf sidecar over it.
// The result is recorded in details.bin. Always fills `out`. For an EPUB whose parse failed it is
// left empty and not recorded, so the next visit tries again: the failure may have been a lack of
// memory. `scratch` holds the OPF inflate ring instead of the heap when given. True when the lookup
// succeeded.
bool parse(const std::string& bookPath, uint32_t bookSize, BookDetails& out, BuildArena* scratch = nullptr);

}  // namespace BookDetailsLookup
