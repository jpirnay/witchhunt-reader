#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BookReaderOverrides.h"

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string series;
  std::string coverBmpPath;
  // SidecarFiles::metadataStamp() of the metadata sidecar title/author/series were
  // taken with (0 = none). EPUB only; see RecentBooksStore::newestBooks().
  uint32_t metadataStamp = 0;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

// A row of recent.json, the list's file before recent.bin: the book, and the reader settings of its own a row of a
// firmware from before they moved to the book's folder still carries. Read once, when the list moves to recent.bin.
struct LegacyRecentBook {
  RecentBook book;
  BookReaderOverrides overrides;
};

// What JsonSettingsIO::loadRecentBooks made of recent.json's text: its rows; a file it cannot read, which stands for an
// empty list; or too little memory to read it with, which stands for nothing yet (the move waits for the next call).
enum class RecentBooksLoad : uint8_t { Loaded, Broken, ShortOfMemory };

namespace JsonSettingsIO {
RecentBooksLoad loadRecentBooks(std::vector<LegacyRecentBook>& out, const char* json);
}  // namespace JsonSettingsIO

// The books lately opened, newest first, in /.crosspoint/recent.bin (RecentBooksFile): at most kMaxBooks, the one
// opened longest ago making room for a new one. Also the way to a book's own reader settings, which live in its folder
// (readerOverrides()).
//
// Nothing of the list is held in RAM. Each call opens the file and reads what it needs: the table a chunk at a time,
// a slot for each row it returns. Home asks for its newest few (newestBooks), the Library pages through the list
// (rowsBefore), and a change writes its entry, or its slot and its entry. The first call after an update from a
// firmware that kept the list in recent.json moves it over and deletes that file.
class RecentBooksStore {
  static RecentBooksStore instance;

 public:
  static constexpr uint8_t kMaxBooks = 100;

  static RecentBooksStore& getInstance() { return instance; }

  // The book opened now: listed first, added if it was not listed (making room by the one opened longest ago).
  void addBook(const std::string& path, const std::string& title, const std::string& author, const std::string& series,
               const std::string& coverBmpPath);
  // A listed book's title, author, series and cover; a book not listed stays so.
  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& series, const std::string& coverBmpPath);
  void removeBook(const std::string& path);
  // A book renamed or moved on the card (BookRelocation): its row follows it, keeping its place. Its cover thumbnail
  // and its own settings live in the book's cache folder, which moved too.
  void relocate(const std::string& oldPath, const std::string& newPath, const std::string& oldCacheDir,
                const std::string& newCacheDir);

  // Home: the newest books still on the card, at most `max`, newest first. A row whose metadata sidecar was added,
  // edited or removed since the book was opened takes title, author and series from it first (a sidecar written by the
  // metadata-editor plugin, or copied over USB, would otherwise reach Home only at the book's next open). A book gone
  // from the card is passed over and keeps its row.
  std::vector<RecentBook> newestBooks(size_t max);

  // The Library's pages: up to `max` books opened before `before` (the seq the previous page ended on; kNewest for the
  // first), newest first, appended to `out`. A book gone from the card leaves the list as the page meets it, and the
  // page reads on below it.
  struct Row {
    std::string path;
    uint32_t seq = 0;
  };
  static constexpr uint32_t kNewest = UINT32_MAX;
  void rowsBefore(uint32_t before, size_t max, std::vector<Row>& out);
  // How many books the list holds; a book gone from the card counts until a page meets it.
  size_t count();

  // The listed book at `path`, or one with an empty path.
  RecentBook getBookByPath(const std::string& path);
  // Many getBookByPath()s in a row, the table read once (the serial transfer's book listing).
  class Lookup {
   public:
    Lookup();
    ~Lookup();
    Lookup(const Lookup&) = delete;
    Lookup& operator=(const Lookup&) = delete;
    // The listed book at `path`, or false.
    bool find(const std::string& path, RecentBook& out);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };

  static bool isMissing(const RecentBook& book);

  // The EPUB's own reader settings, from its folder (BookReaderOverridesFile); none for another book or one without.
  BookReaderOverrides readerOverrides(const std::string& path) const;
  // Saves the EPUB's own reader settings in its folder. False when the save failed.
  bool setReaderOverrides(const std::string& path, const BookReaderOverrides& overrides);

  // Where the list lives, for a test: recent.bin and the recent.json it moves from.
  void useFilesForTest(std::string binPath, std::string legacyJsonPath);

 private:
  std::string binPath_;
  std::string legacyJsonPath_;
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
