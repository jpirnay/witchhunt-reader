#include "LibraryJoin.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <string>

#include "LibraryBlob.h"
#include "LibraryFormat.h"
#include "LibraryIndexReader.h"
#include "LibraryKeys.h"

namespace {

bool readStage(const std::string& path, library::StagedBook* staged, const uint16_t count) {
  if (count == 0) return true;
  HalFile stage;
  return Storage.openFileForRead("LIB", path, stage) &&
         library::readExact(stage, staged, count * sizeof(library::StagedBook));
}

// The previous index's named authors, in the names-file format, so publish finds their names without
// resolving any of their books again.
bool carryNames(LibraryIndexReader& previous, const bool hasPrevious, const std::string& namesPath) {
  HalFile names;
  if (!Storage.openFileForWrite("LIB", namesPath, names)) return false;
  if (!hasPrevious) return true;
  library::AuthorRecord author{};
  std::string name;
  std::string key;
  for (uint16_t i = 0; i < previous.header().authorCount; ++i) {
    if (!previous.author(i, author) || !previous.authorName(author, name, &key)) return false;
    if (author.hash == library::AUTHOR_UNKNOWN || author.hash == library::AUTHOR_PENDING) continue;
    // The index keeps no flag: a key that is not what the name alone gives came from a file-as.
    const uint8_t flags = key != LibraryKeys::authorSortKey(name, "") ? 1 : 0;
    if (!library::writeExact(names, &author.hash, sizeof(author.hash)) ||
        !library::writeExact(names, &flags, sizeof(flags)) || !library::writeBlobString(names, name) ||
        !library::writeBlobString(names, key)) {
      return false;
    }
  }
  return true;
}

}  // namespace

namespace LibraryJoin {

bool join(const Input& in, BuildArena& arena, Result& out) {
  out = {};
  const uint16_t count = in.stageCount;
  if (count > library::MAX_BOOKS || !arena.valid()) return false;
  arena.reset();
  auto* staged = arena.allocArray<library::StagedBook>(count);
  if (staged == nullptr) {
    LOG_ERR("LIB", "join: arena too small for %u books", static_cast<unsigned>(count));
    return false;
  }
  if (!readStage(in.stagePath, staged, count)) return false;
  std::sort(staged, staged + count, [](const library::StagedBook& a, const library::StagedBook& b) {
    return a.identity != b.identity ? a.identity < b.identity : a.pathOff < b.pathOff;
  });

  LibraryIndexReader previous;
  const bool hasPrevious = previous.open(in.previousPath);
  const uint32_t previousGen = hasPrevious ? previous.header().buildGen : 0;
  if (!carryNames(previous, hasPrevious, in.namesPath)) return false;

  HalFile records;
  if (!Storage.openFileForWrite("LIB", in.recordsPath, records)) return false;
  const uint32_t newGen = previousGen + 1;
  const uint16_t previousCount = hasPrevious ? previous.header().bookCount : 0;
  uint16_t at = 0;
  library::BookRecord known{};
  bool haveKnown = previousCount > 0 && previous.book(0, known);
  bool anyNew = false;
  for (uint16_t i = 0; i < count; ++i) {
    const library::StagedBook& book = staged[i];
    while (haveKnown && known.identity < book.identity) {
      ++at;
      haveKnown = at < previousCount && previous.book(at, known);
    }
    library::BookRecord record{book.identity, library::AUTHOR_PENDING, book.date,
                               book.pathOff,  book.sidecarSig,         newGen};
    if (haveKnown && known.identity == book.identity) {
      record.firstSeen = known.firstSeen;
      const bool sameSidecar = known.sidecarSig == book.sidecarSig && book.sidecarSig != library::SIDECAR_UNKNOWN;
      if (!in.resolveAll && sameSidecar) record.authorHash = known.authorHash;
    } else {
      anyNew = true;
    }
    if (record.authorHash == library::AUTHOR_PENDING) ++out.pending;
    if (!library::writeExact(records, &record, sizeof(record))) return false;
  }
  out.buildGen = (anyNew || !hasPrevious) ? newGen : previousGen;
  return true;
}

}  // namespace LibraryJoin
