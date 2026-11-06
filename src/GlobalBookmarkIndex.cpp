#include "GlobalBookmarkIndex.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <numeric>

GlobalBookmarkIndex GlobalBookmarkIndex::instance;

namespace {
using bookmark_file::ReadResult;

constexpr uint16_t MAX_ENTRIES = 1000;
constexpr uint16_t MAX_BOOKMARKS_PER_ENTRY = 1000;
constexpr size_t MAX_FIELD_BYTES = UINT16_MAX;
// Three empty strings, isTxt and the bookmark count.
constexpr size_t MIN_ENTRY_BYTES = 3 * sizeof(uint16_t) + 1 + sizeof(uint16_t);

// How many of `count` records of at least `minBytes` the rest of the file can hold.
size_t recordsThatFit(FsFile& f, const uint16_t count, const size_t minBytes) {
  return std::min<size_t>(count, static_cast<size_t>(f.available()) / minBytes);
}

ReadResult readEntry(FsFile& f, GlobalBookmarkIndex::Entry& e) {
  for (std::string* field : {&e.sourcePath, &e.cacheDir, &e.title}) {
    const ReadResult result = bookmark_file::readString(f, *field, MAX_FIELD_BYTES);
    if (result != ReadResult::Ok) return result;
  }
  uint8_t isTxtByte = 0;
  uint16_t bmCount = 0;
  if (!bookmark_file::readPod(f, isTxtByte) || !bookmark_file::readPod(f, bmCount) ||
      bmCount > MAX_BOOKMARKS_PER_ENTRY) {
    return ReadResult::Damaged;
  }
  e.isTxt = (isTxtByte != 0);

  const size_t fits = recordsThatFit(f, bmCount, bookmark_file::kMinRecordBytes);
  if (fits < bmCount) return ReadResult::Damaged;
  if (!heapHasBlockFor(fits * sizeof(BookmarkStore::Bookmark))) return ReadResult::NoMemory;
  e.bookmarks.reserve(fits);
  for (size_t j = 0; j < fits; j++) {
    BookmarkStore::Bookmark bm;
    const ReadResult result = bookmark_file::readBookmark(f, bm);
    if (result != ReadResult::Ok) return result;
    e.bookmarks.push_back(std::move(bm));
  }
  return ReadResult::Ok;
}
}  // namespace

GlobalBookmarkIndex::Hold::Hold() {
  ++instance.holds;
  if (!instance.loaded) instance.load();
}

GlobalBookmarkIndex::Hold::~Hold() {
  if (instance.holds > 0) --instance.holds;
  if (instance.holds == 0 && instance.loaded) {
    std::vector<Entry>().swap(instance.entries);  // clear() would keep the capacity
    instance.loaded = false;
  }
}

std::vector<GlobalBookmarkIndex::Entry>::iterator GlobalBookmarkIndex::findBySourcePath(const std::string& sourcePath) {
  return std::find_if(entries.begin(), entries.end(),
                      [&sourcePath](const Entry& e) { return e.sourcePath == sourcePath; });
}

bookmark_file::ReadResult GlobalBookmarkIndex::readEntries(FsFile& f) {
  uint8_t version = 0;
  if (!bookmark_file::readPod(f, version) || version != FILE_VERSION) {
    LOG_ERR("GBI", "Bad version: %u", version);
    return ReadResult::Damaged;
  }

  uint16_t entryCount = 0;
  if (!bookmark_file::readPod(f, entryCount) || entryCount > MAX_ENTRIES) {
    LOG_ERR("GBI", "Invalid entry count: %u", static_cast<unsigned>(entryCount));
    return ReadResult::Damaged;
  }

  // A count the file cannot hold is damage, and what it can hold is all there is to read.
  const size_t fits = recordsThatFit(f, entryCount, MIN_ENTRY_BYTES);
  if (!heapHasBlockFor(fits * sizeof(Entry))) return ReadResult::NoMemory;
  entries.reserve(fits);
  for (size_t i = 0; i < fits; i++) {
    Entry e;
    const ReadResult result = readEntry(f, e);
    if (result != ReadResult::Ok) return result;
    entries.push_back(std::move(e));
  }
  return fits == entryCount ? ReadResult::Ok : ReadResult::Damaged;
}

void GlobalBookmarkIndex::load() {
  std::vector<Entry>().swap(entries);
  loaded = false;
  bookmark_file::recoverReplace(FILE_PATH);

  FsFile f;
  if (!Storage.openFileForRead("GBI", FILE_PATH, f)) {
    LOG_DBG("GBI", "No existing global bookmarks file");
    loaded = true;
    return;
  }
  const ReadResult result = readEntries(f);
  f.close();

  switch (result) {
    case ReadResult::Ok:
      LOG_DBG("GBI", "Loaded %u entries", static_cast<unsigned>(entries.size()));
      break;
    case ReadResult::Damaged:
      // The entries before the damage stand, and the next save writes them back whole.
      LOG_ERR("GBI", "Damaged file, kept %u entries", static_cast<unsigned>(entries.size()));
      break;
    case ReadResult::NoMemory:
      // Not loaded: nothing writes over the file from a part of it, and the next Hold tries again.
      LOG_ERR("GBI", "Not enough memory to load");
      std::vector<Entry>().swap(entries);
      return;
  }
  loaded = true;
}

void GlobalBookmarkIndex::save() const {
  if (!loaded) {
    LOG_ERR("GBI", "Not loaded: nothing to save");
    return;
  }
  Storage.mkdir("/.crosspoint");
  const bool saved = bookmark_file::replaceFile("GBI", FILE_PATH, [this](FsFile& f) {
    const auto entryCount = static_cast<uint16_t>(std::min<size_t>(entries.size(), UINT16_MAX));
    bool ok = bookmark_file::writePod(f, FILE_VERSION) && bookmark_file::writePod(f, entryCount);
    for (uint16_t i = 0; ok && i < entryCount; i++) {
      const Entry& e = entries[i];
      const uint8_t isTxtByte = e.isTxt ? 1 : 0;
      const auto bmCount = static_cast<uint16_t>(std::min<size_t>(e.bookmarks.size(), UINT16_MAX));
      ok = bookmark_file::writeString(f, e.sourcePath, MAX_FIELD_BYTES) &&
           bookmark_file::writeString(f, e.cacheDir, MAX_FIELD_BYTES) &&
           bookmark_file::writeString(f, e.title, MAX_FIELD_BYTES) && bookmark_file::writePod(f, isTxtByte) &&
           bookmark_file::writePod(f, bmCount);
      for (uint16_t j = 0; ok && j < bmCount; j++) ok = bookmark_file::writeBookmark(f, e.bookmarks[j]);
    }
    return ok;
  });
  if (saved) {
    LOG_DBG("GBI", "Saved %u entries", static_cast<unsigned>(entries.size()));
  } else {
    LOG_ERR("GBI", "Failed to save");
  }
}

bool GlobalBookmarkIndex::hasAny() const {
  if (loaded) return !entries.empty();
  bookmark_file::recoverReplace(FILE_PATH);
  FsFile f;
  if (!Storage.openFileForRead("GBI", FILE_PATH, f)) return false;
  uint8_t version = 0;
  uint16_t entryCount = 0;
  const bool read = bookmark_file::readPod(f, version) && bookmark_file::readPod(f, entryCount);
  f.close();
  return read && version == FILE_VERSION && entryCount != 0;
}

void GlobalBookmarkIndex::upsertFromStore(const std::string& sourcePath, const std::string& cacheDir,
                                          const std::string& title, bool isTxt,
                                          const std::vector<BookmarkStore::Bookmark>& bookmarks) {
  const Hold hold;
  if (!loaded) return;

  auto it = findBySourcePath(sourcePath);
  if (bookmarks.empty()) {
    if (it != entries.end()) {
      entries.erase(it);
      save();
    }
    return;
  }

  if (it == entries.end()) {
    Entry e;
    e.sourcePath = sourcePath;
    e.cacheDir = cacheDir;
    e.title = title;
    e.isTxt = isTxt;
    e.bookmarks = bookmarks;
    entries.push_back(std::move(e));
  } else {
    it->cacheDir = cacheDir;
    it->title = title;
    it->isTxt = isTxt;
    it->bookmarks = bookmarks;
  }
  save();
}

void GlobalBookmarkIndex::syncFromStore(const BookmarkStore& store, const std::string& sourcePath,
                                        const std::string& cacheDir, const std::string& title, bool isTxt) {
  upsertFromStore(sourcePath, cacheDir, title, isTxt, store.getAll());
}

void GlobalBookmarkIndex::removeBySourcePath(const std::string& sourcePath) {
  const Hold hold;
  if (!loaded) return;
  auto it = findBySourcePath(sourcePath);
  if (it != entries.end()) {
    entries.erase(it);
    save();
  }
}

void GlobalBookmarkIndex::relocate(const std::string& oldPath, const std::string& newPath,
                                   const std::string& newCacheDir) {
  const Hold hold;
  if (!loaded) return;
  auto it = findBySourcePath(oldPath);
  if (it == entries.end()) return;
  it->sourcePath = newPath;
  it->cacheDir = newCacheDir;
  save();
}

bool GlobalBookmarkIndex::reconcile() {
  const Hold hold;
  if (!loaded) return false;
  const size_t before = entries.size();
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [](const Entry& e) {
                                 if (!Storage.exists(e.sourcePath.c_str())) {
                                   LOG_DBG("GBI", "Dropping orphan entry: missing sourcePath=%s", e.sourcePath.c_str());
                                   return true;
                                 }
                                 if (!Storage.exists(e.cacheDir.c_str())) {
                                   LOG_DBG("GBI", "Dropping orphan entry: missing cacheDir=%s", e.cacheDir.c_str());
                                   return true;
                                 }
                                 return false;
                               }),
                entries.end());
  const bool changed = entries.size() != before;
  if (changed) save();
  return changed;
}

size_t GlobalBookmarkIndex::totalBookmarkCount() const {
  return std::accumulate(entries.begin(), entries.end(), static_cast<size_t>(0),
                         [](size_t sum, const Entry& e) { return sum + e.bookmarks.size(); });
}
