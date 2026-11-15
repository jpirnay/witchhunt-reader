#pragma once

#include <HalStorage.h>
#include <HeapFit.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Bookmark.h"
#include "BookmarkFile.h"

// Stores starred/bookmarked pages for a single book.
// Persisted as a binary file on SD card within the book's cache directory.
//
// Each bookmark has a StarAnchor at the same index (getAnchors()): what finds an EPUB page's text again after a
// relayout (StarAnchoring.h). The two lists change together, through this class only.
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
    anchors.clear();
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
    const bool anchored = version >= 3;
    const size_t recordBytes = (named ? bookmark_file::kMinRecordBytes : 2 * sizeof(uint16_t)) +
                               (anchored ? bookmark_file::kMinAnchorBytes : 0);
    const size_t fits = std::min<size_t>(count, static_cast<size_t>(f.available()) / recordBytes);
    const auto refuse = [this, &f] {
      std::vector<Bookmark>().swap(bookmarks);
      std::vector<StarAnchor>().swap(anchors);
      writable = false;
      f.close();
    };
    if (!heapHasBlockFor(fits * sizeof(Bookmark))) {
      LOG_ERR("BKM", "Not enough memory for %u bookmarks", static_cast<unsigned>(fits));
      return refuse();
    }
    bookmarks.reserve(fits);
    if (!heapHasBlockFor(fits * sizeof(StarAnchor))) {
      LOG_ERR("BKM", "Not enough memory for %u anchors", static_cast<unsigned>(fits));
      return refuse();
    }
    anchors.reserve(fits);

    for (size_t i = 0; i < fits; i++) {
      Bookmark bm;
      StarAnchor anchor;
      ReadResult result = bookmark_file::readBookmark(f, bm, named);
      if (result == ReadResult::Ok && anchored) result = bookmark_file::readStarAnchor(f, anchor);
      if (result == ReadResult::NoMemory) {
        LOG_ERR("BKM", "Not enough memory at entry %u", static_cast<unsigned>(i));
        return refuse();
      }
      if (result == ReadResult::Damaged) {
        LOG_ERR("BKM", "Damaged bookmarks file at entry %u", static_cast<unsigned>(i));
        break;
      }
      bookmarks.push_back(std::move(bm));
      anchors.push_back(std::move(anchor));
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
      for (size_t i = 0; ok && i < bookmarks.size(); i++) {
        ok = bookmark_file::writeBookmark(f, bookmarks[i]) && bookmark_file::writeStarAnchor(f, anchors[i]);
      }
      return ok;
    });
    if (!saved) {
      LOG_ERR("BKM", "Failed to save bookmarks");
      return;
    }
    dirty = false;
    LOG_DBG("BKM", "Saved %u bookmarks", static_cast<unsigned>(bookmarks.size()));
  }

  // Toggle bookmark for the given page. Returns true if now starred, false if removed. A TXT page: its
  // number is all there is to find it by.
  bool toggle(uint16_t spineIndex, uint16_t pageNumber) { return toggle(spineIndex, pageNumber, StarAnchor{}); }

  // An EPUB page: a new star keeps `anchor`, what finds its text after a relayout (StarAnchoring::forPage).
  // Unstarring takes every star on the page: stars made under other layouts can come to rest on one page.
  // Returns true if the page is now starred; false when it was unstarred, or the heap had no room for one more.
  bool toggle(uint16_t spineIndex, uint16_t pageNumber, StarAnchor anchor) {
    const size_t before = bookmarks.size();
    for (size_t i = before; i-- > 0;) {
      if (bookmarks[i].spineIndex == spineIndex && bookmarks[i].pageNumber == pageNumber) eraseAt(i);
    }
    if (bookmarks.size() != before) {
      dirty = true;
      return false;
    }
    // The count is the reader's own doing, but the growth asks through the throwing operator new.
    if (!heapReserveFor(bookmarks, 1) || !heapReserveFor(anchors, 1)) {
      LOG_ERR("BKM", "Not enough memory for bookmark %u", static_cast<unsigned>(before + 1));
      return false;
    }
    bookmarks.push_back(Bookmark{spineIndex, pageNumber, {}});
    anchors.push_back(std::move(anchor));
    dirty = true;
    return true;
  }

  // Check if a page is starred.
  [[nodiscard]] bool has(uint16_t spineIndex, uint16_t pageNumber) const {
    return std::any_of(bookmarks.begin(), bookmarks.end(), [spineIndex, pageNumber](const Bookmark& bm) {
      return bm.spineIndex == spineIndex && bm.pageNumber == pageNumber;
    });
  }

  // The first star on the page, if any.
  [[nodiscard]] std::optional<size_t> indexOf(uint16_t spineIndex, uint16_t pageNumber) const {
    for (size_t i = 0; i < bookmarks.size(); i++) {
      if (bookmarks[i].spineIndex == spineIndex && bookmarks[i].pageNumber == pageNumber) return i;
    }
    return std::nullopt;
  }

  [[nodiscard]] const std::vector<Bookmark>& getAll() const { return bookmarks; }
  // Parallel to getAll().
  [[nodiscard]] const std::vector<StarAnchor>& getAnchors() const { return anchors; }
  [[nodiscard]] bool isEmpty() const { return bookmarks.empty(); }
  void markDirty() { dirty = true; }

  // The star's text is on `pageNumber` of the layout `layout`, which lays its chapter out in `pageCount` pages.
  void movePage(size_t index, uint16_t pageNumber, uint32_t layout, uint16_t pageCount) {
    if (index >= bookmarks.size()) return;
    bookmarks[index].pageNumber = pageNumber;
    anchors[index].layout = layout;
    anchors[index].pageCount = pageCount;
    dirty = true;
  }

  void setXPath(size_t index, std::string xpath) {
    if (index >= anchors.size()) return;
    anchors[index].xpath = std::move(xpath);
    dirty = true;
  }

  // Session-only state (StarAnchor): nothing to save.
  void noteOffset(size_t index, uint32_t offset) {
    if (index < anchors.size()) anchors[index].offset = offset;
  }
  void noteMappingFailed(size_t index) {
    if (index < anchors.size()) anchors[index].mappingFailed = true;
  }

  // Set or clear the name for the bookmark at index. Empty name reverts to default label.
  void rename(size_t index, std::string name) {
    if (index >= bookmarks.size()) return;
    if (name.size() > MAX_NAME_LENGTH) name.resize(MAX_NAME_LENGTH);
    bookmarks[index].name = std::move(name);
    dirty = true;
  }

  void removeAt(size_t index) {
    if (index >= bookmarks.size()) return;
    eraseAt(index);
    dirty = true;
  }

  static constexpr uint16_t MAX_NAME_LENGTH = bookmark_file::kMaxNameBytes;

 private:
  // 3: a StarAnchor after each bookmark (layout, page count, XPath). 2: names. 1: spine and page.
  static constexpr uint8_t FILE_VERSION = 3;
  static constexpr uint16_t MAX_BOOKMARKS = 1000;

  std::vector<Bookmark> bookmarks;
  std::vector<StarAnchor> anchors;
  std::string basePath;
  bool dirty = false;
  bool writable = true;

  [[nodiscard]] std::string getFilePath() const { return basePath + "/bookmarks.bin"; }

  void eraseAt(size_t index) {
    bookmarks.erase(bookmarks.begin() + static_cast<std::ptrdiff_t>(index));
    anchors.erase(anchors.begin() + static_cast<std::ptrdiff_t>(index));
  }
};
