#pragma once
#include <cstdint>
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
  // taken with (0 = none). EPUB only; see refreshSidecarMetadata().
  uint32_t metadataStamp = 0;
  // The book's own reader settings as a recent.json written before they moved to the book's folder carries them
  // (BookReaderOverridesFile). Moved there at the book's next open (moveReaderOverridesToBook), and never written to
  // recent.json again once they are: read them through readerOverrides(), not here.
  BookReaderOverrides legacyOverrides;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

// What JsonSettingsIO::loadRecentBooks made of the file's text: its list; a file it cannot read, which stands for an
// empty list; or too little memory to read it with, which stands for nothing (RecentBooksStore::loadFromFile).
enum class RecentBooksLoad : uint8_t { Loaded, Broken, ShortOfMemory };

class RecentBooksStore;
namespace JsonSettingsIO {
RecentBooksLoad loadRecentBooks(RecentBooksStore& store, const char* json);
}  // namespace JsonSettingsIO

// The list of books lately opened, newest first. Also the way to a book's own reader settings, which live in its
// folder (readerOverrides()).
//
// Kept in RAM only while something holds it (Hold), not for the whole session: loaded at boot it
// cost ~4.5 KB for good and, worse, split the heap's largest free block (61 KB -> 45 KB, X3), which
// is what the reader then opened every book with. The screens that show the list hold it while
// they are open; one-off calls (the reader adding its book, a per-book override saved, a book
// removed) hold it for the length of the call. Holds nest, so a call inside a held stretch reads
// the list already there.
class RecentBooksStore {
  // Static instance
  static RecentBooksStore instance;

  mutable std::vector<RecentBook> recentBooks;
  mutable bool loaded = false;
  // A load is under way (loadFromFile): whatever it calls that asks for the list gets the list as filled so far,
  // never a second load.
  mutable bool loading = false;
  uint8_t holds = 0;

  friend RecentBooksLoad JsonSettingsIO::loadRecentBooks(RecentBooksStore&, const char*);

  void ensureLoaded() const;

 public:
  // Keeps the list in RAM for as long as it lives: the first Hold loads it from the card, the last
  // one to go lets it go. Hold one across anything that keeps a reference from getBooks().
  class Hold {
   public:
    Hold();
    ~Hold();
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
  };

  ~RecentBooksStore() = default;

  // Get singleton instance
  static RecentBooksStore& getInstance() { return instance; }

  // Add a book to the recent list (moves to front if already exists)
  void addBook(const std::string& path, const std::string& title, const std::string& author, const std::string& series,
               const std::string& coverBmpPath);

  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& series, const std::string& coverBmpPath);

  // Re-read title, author and series for EPUB entries whose metadata sidecar was
  // added, edited or removed since they were stored. They are otherwise only
  // taken when the book is opened, so a sidecar written by the metadata-editor
  // plugin (or copied over USB) never reached the home screen. Covers the first
  // maxBooks entries still on the card, counted as the home screen counts them.
  // Call before copying entries out. Saves and returns true if any changed.
  bool refreshSidecarMetadata(size_t maxBooks);

  // Remove a book from the recent list by path
  void removeBook(const std::string& path);
  // A book renamed or moved on the card (BookRelocation): its row follows it, keeping its place. Its cover thumbnail
  // and its own settings live in the book's cache folder, which moved too.
  void relocate(const std::string& oldPath, const std::string& newPath, const std::string& oldCacheDir,
                const std::string& newCacheDir);

  // Get the list of recent books (most recent first). The reference is good while a Hold lives.
  // Called without one, the list is loaded and stays until the next Hold to end lets it go.
  const std::vector<RecentBook>& getBooks() const {
    ensureLoaded();
    return recentBooks;
  }

  // Get the count of recent books
  int getCount() const {
    ensureLoaded();
    return static_cast<int>(recentBooks.size());
  }

  // Returns true if the book's file is missing from storage
  static bool isMissing(const RecentBook& book);

  // Remove entries whose backing file is no longer on the SD card.
  // Returns true if any entry was removed. Does not persist — caller decides.
  bool pruneMissing();

  // Refuses while the list is not loaded: saving then would write an empty list over the file.
  bool saveToFile() const;

  bool loadFromFile();
  RecentBook getDataFromBook(std::string path) const;
  RecentBook getBookByPath(const std::string& path) const;
  // The EPUB's own reader settings: its overrides.bin, else what its row in this list still carries (a book not
  // opened since they moved to its folder), else none. Loads the list only for that second answer.
  BookReaderOverrides readerOverrides(const std::string& path) const;
  // readerOverrides(), with a copy the book's row still carries moved into its folder first and dropped from the row.
  // The row's copy wins over a file already there: only a firmware that kept them in this list writes one, so it is
  // the later of the two. The reader calls this as it opens a book.
  BookReaderOverrides moveReaderOverridesToBook(const std::string& path);
  // Saves the EPUB's own reader settings in its folder, and drops any copy its row still carries so that clearing the
  // book's cache cannot bring the old values back. False when the save failed.
  bool setReaderOverrides(const std::string& path, const BookReaderOverrides& overrides);
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
