#include "RecentBooksStore.h"

#include <HalStorage.h>
#include <HeapFit.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "JsonSettingsIO.h"
#include "RecentBookMetadata.h"
#include "RecentBooksFile.h"
#include "SerialTestApi.h"

namespace {

using RecentBooksFile::Entry;
using RecentBooksFile::File;
using RecentBooksFile::pathHash;

static_assert(RecentBooksStore::kMaxBooks == RecentBooksFile::kSlotCount, "the store holds what the file does");

constexpr char kLegacyJsonPath[] = "/.crosspoint/recent.json";
constexpr size_t kMaxLegacyJson = 32 * 1024;
// A seq this high is a table a power cut tore, not opens counted: the entries are numbered afresh before it wraps.
constexpr uint32_t kRenumberAt = 0x7FFFFFFFu;
// Entries a page or Home's newest few pick in one pass over the table.
constexpr uint8_t kPick = 8;

// What one pass over the table found for a path: its slot (its book read back and checked against it), the newest
// seq, a free slot on the card, and the entry opened longest ago.
struct Scan {
  int found = -1;
  Entry foundEntry;
  RecentBook foundBook;
  uint32_t maxSeq = 0;
  int oldest = -1;
  uint32_t oldestSeq = UINT32_MAX;
  int firstFree = -1;
  uint8_t used = 0;
  uint8_t onCard = 0;
};

bool scanFor(File& file, const std::string& path, Scan& out) {
  const uint32_t hash = pathHash(path);
  out.onCard = file.slotsOnCard();
  // Two paths can share a hash: each entry that names it is checked against its slot below.
  constexpr uint8_t kCandidates = 4;
  uint8_t candidates[kCandidates];
  Entry candidateEntries[kCandidates];
  uint8_t candidateCount = 0;
  const bool read = file.forEachEntry([&](const uint8_t slot, const Entry& entry) {
    if (!entry.used()) {
      if (out.firstFree < 0 && slot < out.onCard) out.firstFree = slot;
      return;
    }
    ++out.used;
    out.maxSeq = std::max(out.maxSeq, entry.seq);
    if (entry.seq < out.oldestSeq) {
      out.oldestSeq = entry.seq;
      out.oldest = slot;
    }
    if (entry.hash == hash && candidateCount < kCandidates) {
      candidates[candidateCount] = slot;
      candidateEntries[candidateCount] = entry;
      ++candidateCount;
    }
  });
  if (!read) return false;
  for (uint8_t i = 0; i < candidateCount && out.found < 0; ++i) {
    RecentBook book;
    if (file.readSlot(candidates[i], book) && book.path == path) {
      out.found = candidates[i];
      out.foundEntry = candidateEntries[i];
      out.foundBook = std::move(book);
    }
  }
  return true;
}

// The seq the next open is given. Numbered afresh, in the same order, when a torn table left one near the top.
uint32_t nextSeq(File& file, const Scan& scan) {
  if (scan.maxSeq < kRenumberAt) return scan.maxSeq + 1;
  struct Numbered {
    uint8_t slot;
    Entry entry;
  };
  auto all = makeUniqueNoThrow<Numbered[]>(RecentBooksFile::kSlotCount);
  if (!all) return scan.maxSeq + 1;
  uint8_t count = 0;
  file.forEachEntry([&](const uint8_t slot, const Entry& entry) {
    if (entry.used()) all[count++] = {slot, entry};
  });
  std::sort(all.get(), all.get() + count,
            [](const Numbered& a, const Numbered& b) { return a.entry.seq < b.entry.seq; });
  for (uint8_t i = 0; i < count; ++i) {
    all[i].entry.seq = static_cast<uint32_t>(i + 1);
    file.writeEntry(all[i].slot, all[i].entry);
  }
  LOG_INF("RBS", "Recent books renumbered (%u)", static_cast<unsigned>(count));
  return static_cast<uint32_t>(count + 1);
}

// The used entries opened before `before`, newest first, at most `max` of them: one pass over the table, `max` kept.
struct Pick {
  uint8_t slot;
  Entry entry;
};

uint8_t pickBefore(File& file, const uint32_t before, Pick* out, const uint8_t max) {
  uint8_t count = 0;
  file.forEachEntry([&](const uint8_t slot, const Entry& entry) {
    if (!entry.used() || entry.seq >= before) return;
    if (count == max && entry.seq <= out[max - 1].entry.seq) return;
    uint8_t at = count < max ? count++ : static_cast<uint8_t>(max - 1);
    while (at > 0 && out[at - 1].entry.seq < entry.seq) {
      out[at] = out[at - 1];
      --at;
    }
    out[at] = Pick{slot, entry};
  });
  return count;
}

// The picked entry's book, its slot checked. A slot whose CRC fails is freed. One whose path no longer hashes to its
// entry -- the table's sector torn by a power cut, or a new book's slot written and its entry not -- is the slot's
// book: its entry is put right.
bool readRow(File& file, const Pick& pick, RecentBook& out) {
  if (!file.readSlot(pick.slot, out)) {
    LOG_ERR("RBS", "Recent-books slot %u unreadable: dropped", pick.slot);
    file.writeEntry(pick.slot, Entry{});
    return false;
  }
  const uint32_t hash = pathHash(out.path);
  if (hash != pick.entry.hash) file.writeEntry(pick.slot, Entry{hash, pick.entry.seq});
  return true;
}

}  // namespace

RecentBooksStore RecentBooksStore::instance;

void RecentBooksStore::useFilesForTest(std::string binPath, std::string legacyJsonPath) {
  binPath_ = std::move(binPath);
  legacyJsonPath_ = std::move(legacyJsonPath);
}

namespace {

// recent-books.bin, open; started afresh when it is not there or not of this layout, from recent.json the first time.
// False when it cannot be opened now, or the move from recent.json has to wait (too little memory to read it).
bool openList(File& file, const std::string& binPathSet, const std::string& jsonPathSet) {
  const char* const binPath = binPathSet.empty() ? RecentBooksFile::kPath : binPathSet.c_str();
  const char* const jsonPath = jsonPathSet.empty() ? kLegacyJsonPath : jsonPathSet.c_str();
  switch (file.open(binPath)) {
    case File::Open::Ok:
      return true;
    case File::Open::Unreadable:
      LOG_ERR("RBS", "Could not open %s", binPath);
      return false;
    case File::Open::Missing:
    case File::Open::Foreign:
      break;
  }
  std::vector<LegacyRecentBook> legacy;
  if (Storage.exists(jsonPath)) {
    size_t size = 0;
    {
      HalFile probe;
      if (!Storage.openFileForRead("RBS", jsonPath, probe)) return false;
      size = probe.fileSize();
    }
    // Ten rows are a few KB; a file far past that is not one recent.json ever was, and is let go with the rest.
    if (size > 0 && size <= kMaxLegacyJson) {
      // The text in one block, from the throwing allocator: asked for only where it fits, or the move waits.
      std::string json;
      if (!heapHasBlockFor(size + 1) || !Storage.readFileToString("RBS", jsonPath, kMaxLegacyJson, json)) {
        LOG_ERR("RBS", "Could not read %s (%u B): its move waits", jsonPath, static_cast<unsigned>(size));
        return false;
      }
      if (JsonSettingsIO::loadRecentBooks(legacy, json.c_str()) == RecentBooksLoad::ShortOfMemory) return false;
    }
  }
  Storage.mkdir("/.crosspoint");
  if (!file.create(binPath)) return false;
  // Newest first in recent.json: the first row takes the highest seq.
  uint8_t slot = 0;
  for (size_t i = 0; i < legacy.size() && slot < RecentBooksFile::kSlotCount; ++i) {
    const LegacyRecentBook& row = legacy[i];
    // The settings a row still carries are the later copy: only a firmware that kept them in recent.json wrote them.
    const std::string cacheDir = RecentBookMetadata::overridesCacheDir(row.book.path);
    if (!cacheDir.empty() && row.overrides.any() && BookReaderOverridesFile::save(cacheDir, row.overrides)) {
      LOG_INF("RBS", "Moved a book's own settings from recent.json into %s", cacheDir.c_str());
    }
    if (!file.writeSlot(slot, row.book)) continue;
    file.writeEntry(slot, Entry{pathHash(row.book.path), static_cast<uint32_t>(legacy.size() - i)});
    ++slot;
  }
  if (Storage.exists(jsonPath)) Storage.remove(jsonPath);
  if (!legacy.empty()) LOG_INF("RBS", "Recent books moved from recent.json (%u)", static_cast<unsigned>(slot));
  return true;
}

}  // namespace

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& series, const std::string& coverBmpPath) {
  if (SerialTestApi::isTestBook(path)) return;  // a device test's book takes no reader's book's place
  RecentBook book{path, title, author, series, coverBmpPath};
  // Stamped with the sidecar its metadata came from, so newestBooks() does not redo it.
  book.metadataStamp = RecentBookMetadata::openStamp(path);
  File file;
  Scan scan;
  if (!openList(file, binPath_, legacyJsonPath_) || !scanFor(file, path, scan)) return;
  const uint32_t seq = nextSeq(file, scan);
  const uint32_t hash = pathHash(path);
  if (scan.found >= 0) {
    const RecentBook& stored = scan.foundBook;
    // The slot only when what it keeps changed: an open of a listed book is one entry.
    if (stored.title != book.title || stored.author != book.author || stored.series != book.series ||
        stored.coverBmpPath != book.coverBmpPath || stored.metadataStamp != book.metadataStamp) {
      file.writeSlot(static_cast<uint8_t>(scan.found), book);
    }
    file.writeEntry(static_cast<uint8_t>(scan.found), Entry{hash, seq});
    return;
  }
  // A free slot on the card, else the next one, else the one of the book opened longest ago.
  const int slot = scan.firstFree >= 0                         ? scan.firstFree
                   : scan.onCard < RecentBooksFile::kSlotCount ? scan.onCard
                                                               : scan.oldest;
  if (slot < 0 || !file.writeSlot(static_cast<uint8_t>(slot), book)) return;
  file.writeEntry(static_cast<uint8_t>(slot), Entry{hash, seq});
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& series, const std::string& coverBmpPath) {
  File file;
  Scan scan;
  if (!openList(file, binPath_, legacyJsonPath_) || !scanFor(file, path, scan) || scan.found < 0) return;
  RecentBook book = std::move(scan.foundBook);
  if (book.title == title && book.author == author && book.series == series && book.coverBmpPath == coverBmpPath) {
    return;
  }
  book.title = title;
  book.author = author;
  book.series = series;
  book.coverBmpPath = coverBmpPath;
  file.writeSlot(static_cast<uint8_t>(scan.found), book);
}

void RecentBooksStore::removeBook(const std::string& path) {
  File file;
  Scan scan;
  if (!openList(file, binPath_, legacyJsonPath_) || !scanFor(file, path, scan) || scan.found < 0) return;
  file.writeEntry(static_cast<uint8_t>(scan.found), Entry{});
}

void RecentBooksStore::relocate(const std::string& oldPath, const std::string& newPath, const std::string& oldCacheDir,
                                const std::string& newCacheDir) {
  File file;
  Scan scan;
  if (!openList(file, binPath_, legacyJsonPath_) || !scanFor(file, oldPath, scan) || scan.found < 0) return;
  // A row left for a book deleted from the new path would now name this one twice.
  Scan atNew;
  if (scanFor(file, newPath, atNew) && atNew.found >= 0 && atNew.found != scan.found) {
    file.writeEntry(static_cast<uint8_t>(atNew.found), Entry{});
  }
  RecentBook book = std::move(scan.foundBook);
  book.path = newPath;
  const std::string oldPrefix = oldCacheDir + "/";
  if (!oldCacheDir.empty() && book.coverBmpPath.compare(0, oldPrefix.size(), oldPrefix) == 0) {
    book.coverBmpPath = newCacheDir + book.coverBmpPath.substr(oldCacheDir.size());
  }
  // TXT and Markdown books are listed under their file name.
  const auto nameOf = [](const std::string& path) { return path.substr(path.rfind('/') + 1); };
  if (book.title == nameOf(oldPath)) book.title = nameOf(newPath);
  // The slot first: a power cut before the entry leaves a slot whose path no longer hashes to its entry, which the
  // next read puts right (readRow).
  if (!file.writeSlot(static_cast<uint8_t>(scan.found), book)) return;
  file.writeEntry(static_cast<uint8_t>(scan.found), Entry{pathHash(newPath), scan.foundEntry.seq});
}

std::vector<RecentBook> RecentBooksStore::newestBooks(const size_t max) {
  std::vector<RecentBook> out;
  File file;
  if (max == 0 || !openList(file, binPath_, legacyJsonPath_)) return out;
  Pick picks[kPick];
  uint32_t before = kNewest;
  while (out.size() < max) {
    const uint8_t picked = pickBefore(file, before, picks, kPick);
    for (uint8_t i = 0; i < picked && out.size() < max; ++i) {
      before = picks[i].entry.seq;
      RecentBook book;
      if (!readRow(file, picks[i], book) || isMissing(book)) continue;
      if (std::any_of(out.begin(), out.end(), [&](const RecentBook& seen) { return seen.path == book.path; })) continue;
      if (RecentBookMetadata::refreshFromSidecar(book)) file.writeSlot(picks[i].slot, book);
      out.push_back(std::move(book));
    }
    if (picked < kPick) break;
  }
  return out;
}

void RecentBooksStore::rowsBefore(const uint32_t before, const size_t max, std::vector<Row>& out) {
  File file;
  if (max == 0 || !openList(file, binPath_, legacyJsonPath_)) return;
  Pick picks[kPick];
  uint32_t cursor = before;
  size_t added = 0;
  while (added < max) {
    const uint8_t picked = pickBefore(file, cursor, picks, kPick);
    for (uint8_t i = 0; i < picked && added < max; ++i) {
      cursor = picks[i].entry.seq;
      RecentBook book;
      if (!readRow(file, picks[i], book)) continue;
      if (isMissing(book)) {
        // Gone from the card: off the list for good, as the Library's Recent tab always took them.
        file.writeEntry(picks[i].slot, Entry{});
        continue;
      }
      out.push_back(Row{std::move(book.path), picks[i].entry.seq});
      ++added;
    }
    if (picked < kPick) break;
  }
}

size_t RecentBooksStore::count() {
  File file;
  if (!openList(file, binPath_, legacyJsonPath_)) return 0;
  size_t used = 0;
  file.forEachEntry([&](uint8_t, const Entry& entry) {
    if (entry.used()) ++used;
  });
  return used;
}

RecentBook RecentBooksStore::getBookByPath(const std::string& path) {
  File file;
  Scan scan;
  if (!openList(file, binPath_, legacyJsonPath_) || !scanFor(file, path, scan) || scan.found < 0) return RecentBook{};
  return std::move(scan.foundBook);
}

struct RecentBooksStore::Lookup::Impl {
  File file;
  bool open = false;
  Entry entries[RecentBooksFile::kSlotCount];
};

RecentBooksStore::Lookup::Lookup() : impl_(makeUniqueNoThrow<Impl>()) {
  if (!impl_) return;
  impl_->open =
      openList(impl_->file, instance.binPath_, instance.legacyJsonPath_) &&
      impl_->file.forEachEntry([this](const uint8_t slot, const Entry& entry) { impl_->entries[slot] = entry; });
}

RecentBooksStore::Lookup::~Lookup() = default;

bool RecentBooksStore::Lookup::find(const std::string& path, RecentBook& out) {
  if (!impl_ || !impl_->open) return false;
  const uint32_t hash = pathHash(path);
  for (uint8_t slot = 0; slot < RecentBooksFile::kSlotCount; ++slot) {
    if (impl_->entries[slot].hash == hash && impl_->file.readSlot(slot, out) && out.path == path) return true;
  }
  return false;
}

bool RecentBooksStore::isMissing(const RecentBook& book) { return !Storage.exists(book.path.c_str()); }

BookReaderOverrides RecentBooksStore::readerOverrides(const std::string& path) const {
  const std::string cacheDir = RecentBookMetadata::overridesCacheDir(path);
  BookReaderOverrides overrides;
  if (!cacheDir.empty()) BookReaderOverridesFile::load(cacheDir, overrides);
  return overrides;
}

bool RecentBooksStore::setReaderOverrides(const std::string& path, const BookReaderOverrides& overrides) {
  const std::string cacheDir = RecentBookMetadata::overridesCacheDir(path);
  return !cacheDir.empty() && BookReaderOverridesFile::save(cacheDir, overrides);
}
