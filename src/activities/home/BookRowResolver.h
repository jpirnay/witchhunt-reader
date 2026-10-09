#pragma once

#include <cstdint>
#include <string>
#include <vector>

class BuildArena;
class GfxRenderer;

// What a book list shows for the books on screen, worked out from their paths.
//
// The middle piece of a book list: something hands over the paths of the rows a screen is about to
// draw (today the file browser's folder listing; a library index could do the same), this answers
// what each one says -- title, "Author · Series #n", reading progress -- and whatever the screen
// does with a row when it is selected stays the screen's business. The Details list and the Covers
// grid of Browse Files both draw from it.
//
// Rows are resolved from the card without parsing anything where possible (BookDetailsLookup::
// cached, progress.bin). A never-opened EPUB comes back titled by its filename and marked
// needsParse; resolveOne(), called from the screen's loop(), parses those one at a time.
//
// The rows drawn lately are kept in a ring of CAPACITY entries keyed by path, reserved once and
// overwritten oldest first, so moving a selection redraws from memory and a long folder costs no
// more than a short one.
//
// Threading: row() runs on the render task while a frame is built, resolveOne() on the loop task.
// Both touch the ring only under the render lock -- row() because the render task holds it while
// it builds, resolveOne() by taking it -- and the parse itself runs outside the lock.
class BookRowResolver {
 public:
  struct Row {
    std::string path;
    uint32_t size = 0;
    std::string title;       // "" = the screen keeps the filename
    std::string subtitle;    // "Author · Series #3", or whichever part the book has
    std::string author;      // the subtitle's two parts, for a cover grid with a line for each
    std::string series;      // "Series #3"
    std::string value;       // "42%", Finished, or ""
    int8_t percent = -1;     // -1 unread / unknown, else 0..100
    int16_t bookCount = -1;  // a folder's books, all levels down; -1 not counted yet
    bool needsParse = false;
    bool needsCount = false;
  };

  static constexpr size_t CAPACITY = 16;

  // The row for a book on screen. Call with the render lock held. The reference stays valid until
  // the next row() call, which may reuse its slot: copy what is needed before asking for another.
  const Row& row(const std::string& path, uint32_t size);
  // The same for a folder on screen: its book count, counted by resolveOne() like a parse.
  const Row& folder(const std::string& path);

  // Parses one book still marked needsParse -- or, with none left, counts one folder marked
  // needsCount -- if no frame is being composed. True when that was the last one pending: the
  // caller redraws once, rather than once per row.
  // `scratch`: lent memory for the OPF parse's inflate ring (see BookDetailsLookup::parse).
  bool resolveOne(const GfxRenderer& renderer, BuildArena* scratch = nullptr);

  // Some row is still waiting for resolveOne().
  bool hasPending() const { return pending; }

  // Forget every row -- after a file changed, or a display option that changes what rows say.
  void clear();
  // clear() and give the ring's memory back, for a screen being left.
  void release();

 private:
  Row& store(Row&& fresh);

  std::vector<Row> rows;
  size_t next = 0;
  bool pending = false;  // some row is still waiting for its OPF parse
};
