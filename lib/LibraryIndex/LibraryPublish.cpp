#include "LibraryPublish.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "LibraryBlob.h"
#include "LibraryFormat.h"
#include "LibraryKeys.h"

namespace {

// Keys are compared on this many bytes in the sort; equal prefixes are then put in full-key order.
constexpr size_t KEY_PREFIX = 8;
// Longest run of equal prefixes put in full-key order; a longer one keeps its hash order.
constexpr uint16_t MAX_TIE_RUN = 32;
// Filed after every real key, as no UTF-8 starts with 0xFF: books with no author, then books whose
// author is not known yet.
constexpr char UNKNOWN_KEY[] = "\xff\xfe";
constexpr char PENDING_KEY[] = "\xff\xff";
constexpr uint32_t NO_NAME = 0xFFFFFFFFu;
// Set in a names-file offset held in Tables::nameAt when that entry's key came from a file-as.
constexpr uint32_t FILE_AS_BIT = 0x80000000u;
constexpr uint8_t ENTRY_FILE_AS = 0x01;

struct KeyPrefix {
  char bytes[KEY_PREFIX];
};

void setPrefix(KeyPrefix& prefix, const std::string& key) {
  std::memset(prefix.bytes, 0, KEY_PREFIX);
  std::memcpy(prefix.bytes, key.data(), std::min(key.size(), KEY_PREFIX));
}

// The NEW_COUNT newest books: first seen in a later build first, then the newer file date.
struct Newest {
  struct Entry {
    uint16_t index;
    uint32_t firstSeen;
    uint32_t date;
    uint32_t identity;
  };
  Entry entries[library::NEW_COUNT];
  uint16_t size = 0;
  uint32_t newestDate = 0;  // over every book offered, not only the ten kept

  static bool newer(const Entry& a, const Entry& b) {
    if (a.firstSeen != b.firstSeen) return a.firstSeen > b.firstSeen;
    if (a.date != b.date) return a.date > b.date;
    return a.identity < b.identity;
  }

  void offer(const uint16_t index, const library::BookRecord& record) {
    const Entry entry{index, record.firstSeen, record.date, record.identity};
    newestDate = std::max(newestDate, record.date);
    if (size == library::NEW_COUNT && !newer(entry, entries[size - 1])) return;
    uint16_t at = size < library::NEW_COUNT ? size++ : static_cast<uint16_t>(size - 1);
    while (at > 0 && newer(entry, entries[at - 1])) {
      entries[at] = entries[at - 1];
      --at;
    }
    entries[at] = entry;
  }
};

// The arrays the author table is built in, all in the arena.
struct Tables {
  uint32_t* hashes = nullptr;  // distinct author hashes, ascending
  uint16_t* counts = nullptr;  // books per author, by hash index
  uint32_t* nameAt = nullptr;  // names-file offset (| FILE_AS_BIT) per hash index; its rank once written
  uint16_t* order = nullptr;   // hash index per rank; that rank's first slot once the table is written
  uint16_t* slots = nullptr;   // the author-books table
  uint16_t authors = 0;
};

// Reads every record once: its author into `hashes`, and the newest books into `newest`.
bool scanRecords(const std::string& path, const uint16_t books, uint32_t* hashes, Newest& newest) {
  HalFile records;
  if (!Storage.openFileForRead("LIB", path, records)) return false;
  library::BookRecord record{};
  for (uint16_t i = 0; i < books; ++i) {
    if (!library::readExact(records, &record, sizeof(record))) return false;
    hashes[i] = record.authorHash;
    newest.offer(i, record);
  }
  return true;
}

// Sorts `hashes` and folds it to its distinct values, counting each one's books into `counts`.
uint16_t distinctAuthors(uint32_t* hashes, uint16_t* counts, const uint16_t books) {
  std::sort(hashes, hashes + books);
  uint16_t authors = 0;
  for (uint16_t i = 0; i < books; ++i) {
    if (authors > 0 && hashes[authors - 1] == hashes[i]) {
      ++counts[authors - 1];
    } else {
      hashes[authors] = hashes[i];
      counts[authors] = 1;
      ++authors;
    }
  }
  return authors;
}

bool readNameEntry(HalFile& names, const uint32_t at, std::string& name, std::string& filing) {
  uint32_t hash = 0;
  uint8_t flags = 0;
  return names.seek(at & ~FILE_AS_BIT) && library::readExact(names, &hash, sizeof(hash)) &&
         library::readExact(names, &flags, sizeof(flags)) && library::readBlobString(names, name) &&
         library::readBlobString(names, filing);
}

// Every author's names-file entry, and its key prefix: one whose key came from a file-as beats one
// that did not, otherwise the first wins. An author with none -- no author at all, not resolved yet,
// or a name that never reached the file -- gets a key that files it last. A cut-short last entry
// ends the scan; the entries before it count.
void loadNames(const std::string& path, Tables& t, KeyPrefix* prefixes) {
  for (uint16_t i = 0; i < t.authors; ++i) {
    t.nameAt[i] = NO_NAME;
    setPrefix(prefixes[i], t.hashes[i] == library::AUTHOR_PENDING ? PENDING_KEY : UNKNOWN_KEY);
  }
  HalFile names;
  if (!Storage.openFileForRead("LIB", path, names)) return;
  std::string name;
  std::string filing;
  while (true) {
    const auto at = static_cast<uint32_t>(names.position());
    uint32_t hash = 0;
    uint8_t flags = 0;
    if (!library::readExact(names, &hash, sizeof(hash)) || !library::readExact(names, &flags, sizeof(flags)) ||
        !library::readBlobString(names, name) || !library::readBlobString(names, filing)) {
      break;
    }
    if (hash == library::AUTHOR_UNKNOWN || hash == library::AUTHOR_PENDING) continue;
    const uint32_t* found = std::lower_bound(t.hashes, t.hashes + t.authors, hash);
    if (found == t.hashes + t.authors || *found != hash) continue;
    const auto index = static_cast<uint16_t>(found - t.hashes);
    const bool fileAs = (flags & ENTRY_FILE_AS) != 0;
    const uint32_t held = t.nameAt[index];
    if (held != NO_NAME && ((held & FILE_AS_BIT) != 0 || !fileAs)) continue;
    t.nameAt[index] = at | (fileAs ? FILE_AS_BIT : 0);
    setPrefix(prefixes[index], LibraryKeys::filingKey(filing));
  }
}

void sortAuthors(Tables& t, const KeyPrefix* prefixes) {
  for (uint16_t i = 0; i < t.authors; ++i) t.order[i] = i;
  std::sort(t.order, t.order + t.authors, [prefixes](const uint16_t a, const uint16_t b) {
    const int c = std::memcmp(prefixes[a].bytes, prefixes[b].bytes, KEY_PREFIX);
    return c != 0 ? c < 0 : a < b;
  });
}

// Puts runs of authors whose keys fill and share the whole prefix into full-key order. A key shorter
// than the prefix has been compared whole already.
void orderTies(Tables& t, const KeyPrefix* prefixes, const std::string& namesPath) {
  HalFile names;
  bool opened = false;
  std::vector<std::pair<std::string, uint16_t>> run;
  std::string name;
  uint16_t start = 0;
  while (start < t.authors) {
    uint16_t end = start + 1;
    while (end < t.authors &&
           std::memcmp(prefixes[t.order[start]].bytes, prefixes[t.order[end]].bytes, KEY_PREFIX) == 0) {
      ++end;
    }
    const bool fullPrefix = std::memchr(prefixes[t.order[start]].bytes, 0, KEY_PREFIX) == nullptr;
    if (end - start > 1 && end - start <= MAX_TIE_RUN && fullPrefix) {
      if (!opened) opened = Storage.openFileForRead("LIB", namesPath, names);
      run.clear();
      std::string filing;
      for (uint16_t i = start; i < end; ++i) {
        filing.clear();
        const uint32_t at = t.nameAt[t.order[i]];
        if (opened && at != NO_NAME) readNameEntry(names, at, name, filing);
        run.emplace_back(LibraryKeys::filingKey(filing), t.order[i]);
      }
      std::sort(run.begin(), run.end());
      for (uint16_t i = start; i < end; ++i) t.order[i] = run[i - start].second;
    }
    start = end;
  }
}

// Writes the author table in key order, and each author's name and filing name into `section`. Leaves
// `nameAt` holding every author's rank and `order` every rank's first slot, with `counts` zeroed for
// placing the books.
bool writeAuthors(HalFile& out, HalFile& section, const std::string& namesPath, const uint32_t blobBase, Tables& t,
                  uint32_t& sectionBytes) {
  HalFile names;
  const bool haveNames = Storage.openFileForRead("LIB", namesPath, names);
  std::string name;
  std::string filing;
  uint16_t firstBook = 0;
  sectionBytes = 0;
  for (uint16_t rank = 0; rank < t.authors; ++rank) {
    const uint16_t index = t.order[rank];
    if (t.nameAt[index] == NO_NAME || !haveNames || !readNameEntry(names, t.nameAt[index], name, filing)) {
      name.clear();  // no author, or not known yet: the screen names those itself
      filing.clear();
    }
    const library::AuthorRecord record{t.hashes[index], blobBase + sectionBytes, firstBook, t.counts[index]};
    if (!library::writeExact(out, &record, sizeof(record)) || !library::writeBlobString(section, name) ||
        !library::writeBlobString(section, filing)) {
      return false;
    }
    sectionBytes += static_cast<uint32_t>(2 * sizeof(uint16_t) + name.size() + filing.size());
    t.nameAt[index] = rank;
    t.order[rank] = firstBook;
    firstBook = static_cast<uint16_t>(firstBook + t.counts[index]);
    t.counts[index] = 0;
  }
  return true;
}

// Puts every book into its author's slots, in record order.
bool placeBooks(const std::string& recordsPath, const uint16_t books, Tables& t) {
  HalFile records;
  if (!Storage.openFileForRead("LIB", recordsPath, records)) return false;
  library::BookRecord record{};
  for (uint16_t i = 0; i < books; ++i) {
    if (!library::readExact(records, &record, sizeof(record))) return false;
    const uint32_t* found = std::lower_bound(t.hashes, t.hashes + t.authors, record.authorHash);
    if (found == t.hashes + t.authors || *found != record.authorHash) return false;  // changed under us
    const auto index = static_cast<uint16_t>(found - t.hashes);
    t.slots[t.order[t.nameAt[index]] + t.counts[index]] = i;
    ++t.counts[index];
  }
  return true;
}

bool writeIndex(const LibraryPublish::Input& in, const std::string& tmpPath, const std::string& sectionPath, Tables& t,
                const Newest& newest) {
  HalFile paths;
  if (!Storage.openFileForRead("LIB", in.pathsPath, paths)) return false;
  const auto pathsBytes = static_cast<uint32_t>(paths.fileSize());

  library::Header header{};
  std::memcpy(header.magic, library::MAGIC, sizeof(header.magic));
  header.version = library::VERSION;
  header.flags = in.partial ? library::FLAG_PARTIAL : 0;
  header.acceptRules = in.acceptRules;
  header.buildGen = in.buildGen;
  header.bookCount = in.bookCount;
  header.authorCount = t.authors;
  header.newCount = newest.size;
  header.newestDate = std::max(newest.newestDate, in.newestFolderDate);
  header.recordsOff = sizeof(library::Header);
  header.newOff = header.recordsOff + in.bookCount * static_cast<uint32_t>(sizeof(library::BookRecord));
  header.authorsOff = header.newOff + newest.size * static_cast<uint32_t>(sizeof(uint16_t));
  header.authorBooksOff = header.authorsOff + t.authors * static_cast<uint32_t>(sizeof(library::AuthorRecord));
  header.blobOff = header.authorBooksOff + in.bookCount * static_cast<uint32_t>(sizeof(uint16_t));

  HalFile out;
  if (!Storage.openFileForWrite("LIB", tmpPath, out)) return false;
  bool ok = library::writeExact(out, &header, sizeof(header));  // blobLen is filled in at the end
  {
    HalFile records;
    ok = ok && Storage.openFileForRead("LIB", in.recordsPath, records) &&
         serialization::copyBytes(records, out, in.bookCount * static_cast<uint32_t>(sizeof(library::BookRecord)));
  }
  for (uint16_t i = 0; ok && i < newest.size; ++i) {
    ok = library::writeExact(out, &newest.entries[i].index, sizeof(uint16_t));
  }
  uint32_t sectionBytes = 0;
  {
    HalFile section;
    ok = ok && Storage.openFileForWrite("LIB", sectionPath, section) &&
         writeAuthors(out, section, in.namesPath, pathsBytes, t, sectionBytes);
  }
  ok = ok && placeBooks(in.recordsPath, in.bookCount, t) &&
       library::writeExact(out, t.slots, in.bookCount * sizeof(uint16_t)) &&
       serialization::copyBytes(paths, out, pathsBytes);
  {
    HalFile section;
    ok = ok && Storage.openFileForRead("LIB", sectionPath, section) &&
         serialization::copyBytes(section, out, sectionBytes);
  }
  header.blobLen = pathsBytes + sectionBytes;
  ok = ok && out.seek(0) && library::writeExact(out, &header, sizeof(header));
  out.close();
  return ok;
}

}  // namespace

namespace LibraryPublish {

bool publish(const Input& in, BuildArena& arena, const std::string& outPath) {
  const uint16_t books = in.bookCount;
  if (books > library::MAX_BOOKS || !arena.valid()) return false;
  arena.reset();
  void* newestAt = arena.alloc(sizeof(Newest), alignof(Newest));
  Tables t;
  t.hashes = arena.allocArray<uint32_t>(books);
  t.counts = arena.allocArray<uint16_t>(books);
  if (newestAt == nullptr || t.hashes == nullptr || t.counts == nullptr) {
    LOG_ERR("LIB", "publish: arena too small for %u books", static_cast<unsigned>(books));
    return false;
  }
  Newest& newest = *new (newestAt) Newest();
  if (!scanRecords(in.recordsPath, books, t.hashes, newest)) return false;
  t.authors = distinctAuthors(t.hashes, t.counts, books);

  t.nameAt = arena.allocArray<uint32_t>(t.authors);
  auto* prefixes = arena.allocArray<KeyPrefix>(t.authors);
  t.order = arena.allocArray<uint16_t>(t.authors);
  t.slots = arena.allocArray<uint16_t>(books);
  if (t.nameAt == nullptr || prefixes == nullptr || t.order == nullptr || t.slots == nullptr) {
    LOG_ERR("LIB", "publish: arena too small for %u authors", static_cast<unsigned>(t.authors));
    return false;
  }
  loadNames(in.namesPath, t, prefixes);
  sortAuthors(t, prefixes);
  orderTies(t, prefixes, in.namesPath);

  const std::string tmpPath = outPath + ".tmp";
  const std::string sectionPath = outPath + ".names";
  const bool written = writeIndex(in, tmpPath, sectionPath, t, newest);
  Storage.remove(sectionPath.c_str());
  if (!written) {
    LOG_ERR("LIB", "publish: writing %s failed", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  Storage.remove(outPath.c_str());
  return Storage.rename(tmpPath.c_str(), outPath.c_str());
}

}  // namespace LibraryPublish
