#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "Bookmark.h"
#include "BookmarkFile.h"

// Stores starred/bookmarked pages for a single book.
// Persisted as a binary file on SD card within the book's cache directory.
class BookmarkStore {
 public:
  using Bookmark = ::Bookmark;

  // Load bookmarks from the cache directory (e.g. .crosspoint/epub_<hash>/). What a damaged file
  // holds before the damage stands. With too little heap the list stays empty and save() is refused:
  // the file may be fine, and is not to be written over from a part of it.
  void load(const std::string& cachePath) {
    using bookmark_file::ReadResult;

    basePath = cachePath;
    bookmarks.clear();
    dirty = false;
    writable = true;

    const std::string path = getFilePath();
    bookmark_file::recoverReplace(path);
    FsFile f;
    if (!Storage.openFileForRead("BKM", path, f)) {
      return;
    }

    uint8_t version = 0;
    if (!bookmark_file::readPod(f, version) || version < 1 || version > FILE_VERSION) {
      f.close();
      return;
    }

    uint16_t count = 0;
    if (!bookmark_file::readPod(f, count) || count > MAX_BOOKMARKS) {
      LOG_ERR("BKM", "Invalid bookmark count: %u", static_cast<unsigned>(count));
      f.close();
      return;
    }

    // A count the file cannot hold is damage, and what it can hold is all there is to read.
    const bool named = version >= 2;
    const size_t recordBytes = named ? bookmark_file::kMinRecordBytes : 2 * sizeof(uint16_t);
    const size_t fits = std::min<size_t>(count, static_cast<size_t>(f.available()) / recordBytes);
    if (!heapHasBlockFor(fits * sizeof(Bookmark))) {
      LOG_ERR("BKM", "Not enough memory for %u bookmarks", static_cast<unsigned>(fits));
      writable = false;
      f.close();
      return;
    }

    bookmarks.reserve(fits);
    for (size_t i = 0; i < fits; i++) {
      Bookmark bm;
      const ReadResult result = bookmark_file::readBookmark(f, bm, named);
      if (result == ReadResult::NoMemory) {
        LOG_ERR("BKM", "Not enough memory at entry %u", static_cast<unsigned>(i));
        std::vector<Bookmark>().swap(bookmarks);
        writable = false;
        f.close();
        return;
      }
      if (result == ReadResult::Damaged) {
        LOG_ERR("BKM", "Damaged bookmarks file at entry %u", static_cast<unsigned>(i));
        break;
      }
      bookmarks.push_back(std::move(bm));
    }

    f.close();
    LOG_DBG("BKM", "Loaded %u bookmarks", static_cast<unsigned>(bookmarks.size()));
  }

  // Save bookmarks to SD card (only if changed).
  void save() {
    if (!dirty || basePath.empty()) {
      return;
    }

    if (!writable) {
      LOG_ERR("BKM", "Not saving over a file that could not be read");
      return;
    }

    if (bookmarks.size() > MAX_BOOKMARKS) {
      LOG_ERR("BKM", "Too many bookmarks to save: %u", static_cast<unsigned>(bookmarks.size()));
      return;
    }

    const bool saved = bookmark_file::replaceFile("BKM", getFilePath(), [this](FsFile& f) {
      const auto count = static_cast<uint16_t>(bookmarks.size());
      bool ok = bookmark_file::writePod(f, FILE_VERSION) && bookmark_file::writePod(f, count);
      for (const auto& bm : bookmarks) ok = ok && bookmark_file::writeBookmark(f, bm);
      return ok;
    });
    if (!saved) {
      LOG_ERR("BKM", "Failed to save bookmarks");
      return;
    }
    dirty = false;
    LOG_DBG("BKM", "Saved %u bookmarks", static_cast<unsigned>(bookmarks.size()));
  }

  // Toggle bookmark for the given page. Returns true if now starred, false if removed.
  bool toggle(uint16_t spineIndex, uint16_t pageNumber) {
    auto it = find(spineIndex, pageNumber);
    if (it != bookmarks.end()) {
      bookmarks.erase(it);
      dirty = true;
      return false;
    }
    bookmarks.push_back({spineIndex, pageNumber});
    dirty = true;
    return true;
  }

  // Check if a page is starred.
  [[nodiscard]] bool has(uint16_t spineIndex, uint16_t pageNumber) const {
    return std::any_of(bookmarks.begin(), bookmarks.end(), [spineIndex, pageNumber](const Bookmark& bm) {
      return bm.spineIndex == spineIndex && bm.pageNumber == pageNumber;
    });
  }

  [[nodiscard]] const std::vector<Bookmark>& getAll() const { return bookmarks; }
  [[nodiscard]] bool isEmpty() const { return bookmarks.empty(); }
  void markDirty() { dirty = true; }

  // Set or clear the name for the bookmark at index. Empty name reverts to default label.
  void rename(size_t index, std::string name) {
    if (index >= bookmarks.size()) return;
    if (name.size() > MAX_NAME_LENGTH) name.resize(MAX_NAME_LENGTH);
    bookmarks[index].name = std::move(name);
    dirty = true;
  }

  void removeAt(size_t index) {
    if (index >= bookmarks.size()) return;
    bookmarks.erase(bookmarks.begin() + index);
    dirty = true;
  }

  static constexpr uint16_t MAX_NAME_LENGTH = bookmark_file::kMaxNameBytes;

 private:
  static constexpr uint8_t FILE_VERSION = 2;
  static constexpr uint16_t MAX_BOOKMARKS = 1000;

  std::vector<Bookmark> bookmarks;
  std::string basePath;
  bool dirty = false;
  bool writable = true;

  [[nodiscard]] std::string getFilePath() const { return basePath + "/bookmarks.bin"; }

  std::vector<Bookmark>::iterator find(uint16_t spineIndex, uint16_t pageNumber) {
    return std::find_if(bookmarks.begin(), bookmarks.end(), [spineIndex, pageNumber](const Bookmark& bm) {
      return bm.spineIndex == spineIndex && bm.pageNumber == pageNumber;
    });
  }
};
