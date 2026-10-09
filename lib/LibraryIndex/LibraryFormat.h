#pragma once

#include <cstdint>

// The book index the Library's New and Authors tabs read, and the working files a build assembles
// it from. Design: docs/superpowers/specs/2026-10-09-library-view-design.md, section 3.
namespace library {

constexpr char MAGIC[4] = {'W', 'L', 'I', 'B'};
constexpr uint8_t VERSION = 1;

// Books beyond this are left out and the index is marked partial: publish sorts the whole author
// table in one pass through the lent framebuffer, which this many books fit.
constexpr uint16_t MAX_BOOKS = 2000;
// How many books the New tab lists.
constexpr uint16_t NEW_COUNT = 10;
// Reserved author hashes: a book with no author at all, and one not resolved yet.
constexpr uint32_t AUTHOR_UNKNOWN = 0;
constexpr uint32_t AUTHOR_PENDING = 0xFFFFFFFFu;
// Sidecar signatures: no metadata sidecar, and one the walk could not pair with its book (the
// folder's sidecar table overflowed), which always goes back through details.bin.
constexpr uint32_t SIDECAR_NONE = 0;
constexpr uint32_t SIDECAR_UNKNOWN = 0xFFFFFFFFu;
// Header flags.
constexpr uint8_t FLAG_PARTIAL = 0x01;
// Longest blob string: a path, an author's name or sort key.
constexpr uint16_t MAX_STRING = 1024;

// Where the index and a build's working files live on the card.
constexpr const char* DIR = "/.crosspoint/library";
constexpr const char* INDEX_PATH = "/.crosspoint/library/library.bin";

#pragma pack(push, 1)
struct Header {
  char magic[4];
  uint8_t version;
  uint8_t flags;
  uint8_t acceptRules;  // the walk's showHiddenFiles: another setting lists other books
  uint8_t reserved;
  uint32_t buildGen;
  uint16_t bookCount;
  uint16_t authorCount;
  uint16_t newCount;
  uint16_t reserved2;
  uint32_t recordsOff;
  uint32_t newOff;
  uint32_t authorsOff;
  uint32_t authorBooksOff;
  uint32_t blobOff;
  uint32_t blobLen;
};

// One book, in identity order. pathOff is into the blob.
struct BookRecord {
  uint32_t identity;
  uint32_t authorHash;
  uint32_t date;  // FAT (date << 16) | time, the later of modified and created
  uint32_t pathOff;
  uint32_t sidecarSig;
  uint32_t firstSeen;  // buildGen of the build that first saw this identity
};

// One author, in sort-key order. nameOff (into the blob) holds the display name and then the sort
// key; the author's books are the author-books table's slots [firstBook, firstBook + count).
struct AuthorRecord {
  uint32_t hash;
  uint32_t nameOff;
  uint16_t firstBook;
  uint16_t count;
};

// One book as the walk found it, before any metadata. pathOff is into the paths working file.
struct StagedBook {
  uint32_t identity;
  uint32_t date;
  uint32_t sidecarSig;
  uint32_t pathOff;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 44, "header layout");
static_assert(sizeof(BookRecord) == 24, "book record layout");
static_assert(sizeof(AuthorRecord) == 12, "author record layout");
static_assert(sizeof(StagedBook) == 16, "staged book layout");

}  // namespace library
