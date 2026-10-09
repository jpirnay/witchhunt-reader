#include "BookRowResolver.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "../RenderLock.h"
#include "BookDetails.h"
#include "FileBrowserModel.h"
#include "components/BookProgressPresentation.h"

namespace {

std::string progressLabel(const int percent) {
  if (percent >= 100) return tr(STR_BOOK_FINISHED);
  if (percent > 0) return std::to_string(percent) + "%";
  return {};  // unread, or never opened
}

// A row's text from the book's details: its title, "Author · Series #3" (leaving out whichever part
// the book does not have), and those two parts on their own.
void setText(BookRowResolver::Row& row, BookDetails& d) {
  row.title = std::move(d.title);
  row.series = d.series;
  if (!row.series.empty() && !d.seriesIndex.empty()) row.series += " #" + d.seriesIndex;
  row.author = d.author;
  if (row.author.empty()) {
    row.subtitle = row.series;
  } else if (row.series.empty()) {
    row.subtitle = row.author;
  } else {
    row.subtitle = row.author + " · " + row.series;
  }
}

}  // namespace

const BookRowResolver::Row& BookRowResolver::row(const std::string& path, const uint32_t size) {
  for (const auto& existing : rows) {
    if (existing.path == path) return existing;
  }
  Row fresh;
  fresh.path = path;
  fresh.size = size;
  BookDetails details;
  if (BookDetailsLookup::cached(path, size, details)) {
    setText(fresh, details);
  } else {
    fresh.needsParse = true;
    pending = true;
  }
  const int percent = BookProgressPresentation::readPercent(path);
  fresh.percent = static_cast<int8_t>(std::max(-1, std::min(100, percent)));
  fresh.value = progressLabel(percent);

  return store(std::move(fresh));
}

const BookRowResolver::Row& BookRowResolver::folder(const std::string& path) {
  for (const auto& existing : rows) {
    if (existing.path == path) return existing;
  }
  Row fresh;
  fresh.path = path;
  // Counted earlier this session, and nothing on the card has changed since: the number is there
  // at once, with no "..." and no walk.
  const int known = FileBrowserModel::knownBooksBelow(path);
  if (known >= 0) {
    fresh.bookCount = static_cast<int16_t>(known);
  } else {
    fresh.needsCount = true;
    pending = true;
  }
  return store(std::move(fresh));
}

BookRowResolver::Row& BookRowResolver::store(Row&& fresh) {
  if (rows.capacity() < CAPACITY) rows.reserve(CAPACITY);
  if (rows.size() < CAPACITY) {
    rows.push_back(std::move(fresh));
    return rows.back();
  }
  Row& slot = rows[next];
  next = (next + 1) % CAPACITY;
  slot = std::move(fresh);
  return slot;
}

// One never-opened EPUB per call -- ~300 ms each -- so the caller's input is read between them.
// Not beside a frame being composed, which shares the core and the card with the parse.
bool BookRowResolver::resolveOne(const GfxRenderer& renderer, BuildArena* scratch) {
  if (!pending || renderer.isComposingFrame()) return false;
  std::string path;
  uint32_t size = 0;
  bool count = false;
  {
    RenderLock lock;
    auto it = std::find_if(rows.begin(), rows.end(), [](const Row& r) { return r.needsParse; });
    if (it == rows.end()) {
      it = std::find_if(rows.begin(), rows.end(), [](const Row& r) { return r.needsCount; });
      count = true;
    }
    if (it == rows.end()) {
      pending = false;
      return false;
    }
    path = it->path;
    size = it->size;
  }
  // Idle, the governor drops the clock to 10 MHz between presses -- and this is exactly the work
  // that runs between presses: a 200 ms parse took 2 s there.
  HalPowerManager::Lock fullSpeed;

  if (count) {
    const int books = FileBrowserModel::countBooksBelow(path);
    if (books < 0) return false;  // a button press: count again once it has been served
    RenderLock lock;
    for (auto& r : rows) {
      if (r.path != path || !r.needsCount) continue;
      r.bookCount = static_cast<int16_t>(books);
      r.needsCount = false;
    }
  } else {
    // Logged until the occasional first-visit reboot in Details is understood: what the heap held
    // going into a parse and coming out of it, and how long the parse took.
    const auto heapFree = [] { return static_cast<unsigned long>(esp_get_free_heap_size()); };
    const auto heapContig = [] {
      return static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
    };
    LOG_INF("BRR", "details parse %s: free=%lu contig=%lu minFree=%lu", path.c_str(), heapFree(), heapContig(),
            static_cast<unsigned long>(esp_get_minimum_free_heap_size()));
    const unsigned long parseStart = millis();
    BookDetails details;
    BookDetailsLookup::parse(path, size, details, scratch);
    LOG_INF("BRR", "details parse done in %lu ms (%s): free=%lu contig=%lu", millis() - parseStart,
            details.title.empty() ? "no title" : "ok", heapFree(), heapContig());
    RenderLock lock;
    for (auto& r : rows) {
      if (r.path != path || !r.needsParse) continue;
      setText(r, details);
      r.needsParse = false;
    }
  }

  RenderLock lock;
  if (std::any_of(rows.begin(), rows.end(), [](const Row& r) { return r.needsParse || r.needsCount; })) return false;
  pending = false;
  return true;
}

void BookRowResolver::clear() {
  rows.clear();
  next = 0;
  pending = false;
}

void BookRowResolver::release() {
  clear();
  rows.shrink_to_fit();
}
