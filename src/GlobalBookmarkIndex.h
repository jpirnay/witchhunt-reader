#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BookmarkFile.h"
#include "BookmarkStore.h"

// Aggregates bookmarks from every indexed book into a single queryable catalog.
// Persisted as /.crosspoint/global_bookmarks.bin; updated incrementally when
// per-book BookmarkStore instances save, and reconciled against the filesystem
// when GlobalBookmarksActivity opens.
//
// Held in RAM only while a screen or a call needs it (Hold), as RecentBooksStore is: an index of
// many books loaded at boot took its share of the heap for the whole session, the reader included.
class GlobalBookmarkIndex {
 public:
  struct Entry {
    std::string sourcePath;  // e.g. /books/foo.epub
    std::string cacheDir;    // e.g. /.crosspoint/epub_12345
    std::string title;       // display title
    bool isTxt = false;      // hint for jump dispatch
    std::vector<BookmarkStore::Bookmark> bookmarks;
  };

  // Singleton access.
  static GlobalBookmarkIndex& getInstance() { return instance; }

  // Keeps the index in RAM for as long as it lives: the first Hold loads it from the card, the last
  // one to go lets it go. Hold one across anything that keeps a reference from getEntries(). With
  // too little heap to load it stays out, getEntries() is empty and the file is left as it is.
  class Hold {
   public:
    Hold();
    ~Hold();
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
  };

  // Persist the index to /.crosspoint/global_bookmarks.bin. Refused while it is not loaded: there
  // is nothing in RAM to write over the file with.
  void save() const;

  // Per-book update. Replaces (or removes when empty) the entry for this source path.
  // Safe to call during BookmarkStore::save() hook.
  void upsertFromStore(const std::string& sourcePath, const std::string& cacheDir, const std::string& title, bool isTxt,
                       const std::vector<BookmarkStore::Bookmark>& bookmarks);

  // Drop an entry (e.g. when a book is deleted). No-op if not indexed.
  void removeBySourcePath(const std::string& sourcePath);

  // A book renamed or moved on the card (BookRelocation): its entry follows it, and so does the
  // cache folder its bookmarks are read from. No-op if not indexed.
  void relocate(const std::string& oldPath, const std::string& newPath, const std::string& newCacheDir);

  // Convenience wrapper that pulls bookmarks out of a BookmarkStore and upserts.
  // Also removes the entry if the store is empty.
  void syncFromStore(const BookmarkStore& store, const std::string& sourcePath, const std::string& cacheDir,
                     const std::string& title, bool isTxt);

  // Walk every entry; stat sourcePath + cacheDir. Drop entries whose source file
  // is missing. Called on GlobalBookmarksActivity entry.
  // Returns true if anything changed (callers can decide whether to persist).
  bool reconcile();

  [[nodiscard]] const std::vector<Entry>& getEntries() const { return entries; }
  // Whether any book has a bookmark. When the index is not loaded, from what load() or save() last
  // learned of the file, else from its header, read once and remembered.
  [[nodiscard]] bool hasAny() const;
  // Forget what hasAny() remembers, for a file changed behind the index's back. Nothing in the
  // firmware writes it except save(), so only the tests need this; a reboot forgets it too.
  void forgetFileState() { fileState = FileState::Unknown; }

  // Total bookmark count across all entries.
  [[nodiscard]] size_t totalBookmarkCount() const;

 private:
  static GlobalBookmarkIndex instance;

  std::vector<Entry> entries;
  bool loaded = false;
  uint8_t holds = 0;

  // Whether the file holds any entry, kept while the index is not loaded, so that Home, which asks
  // on every entry, reads the card once per boot and not every time.
  enum class FileState : uint8_t { Unknown, Empty, HasEntries };
  mutable FileState fileState = FileState::Unknown;
  void noteFileHolds(const bool any) const { fileState = any ? FileState::HasEntries : FileState::Empty; }

  static constexpr uint8_t FILE_VERSION = 1;
  static constexpr const char* FILE_PATH = "/.crosspoint/global_bookmarks.bin";

  void load();
  bookmark_file::ReadResult readEntries(FsFile& f);
  std::vector<Entry>::iterator findBySourcePath(const std::string& sourcePath);
};

#define GLOBAL_BOOKMARKS GlobalBookmarkIndex::getInstance()
