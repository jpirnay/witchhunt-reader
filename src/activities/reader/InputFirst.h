#pragma once

#include <cstdint>

// Input first: the reader's background work starts no unit while a press is on its way to becoming a turn (rule R0 of
// docs/design/reader-priority-scheme.md).
//
// The reader's loop hands every pass without an input event to the background work, and a press is no event until it
// is released and read: down, then released, then, with a double-press action set, waiting out its window. Each of
// those passes ran a slice of Background-B's look-ahead build (40 ms and one parse chunk), so the press was read three
// to four slices late. X4, 2026-11-16, B laying out the next chapter: press at 186.775 s, turn seen at 187.138 s,
// eleven pages laid out in between; without B, 111 ms. The build those slices fed was then thrown away by the turn.
//
// A slice of the chapter on screen (Background-C) is another matter while the reader waits for a page of it: the
// page asked for may be one it has not laid out yet, and a key held down must not keep it from coming. Then it goes
// on through a press and stops only for a turn that already waits for the render lock. Once the page asked for is
// drawn, the slices lay out the pages after it, and a press holds them as it holds the look-ahead; a key held on a
// page pauses the layout behind it until it is let go.
//
// X3, 2026-11-17, the test API's press 0.6 s after a cold open's first page: 316 to 368 ms from the command to the
// turn, five loop passes (command read, key down, edge taken in, key up, edge taken in), each with a slice of 41 to
// 99 ms; about 110 ms with nothing building. No unit of the build is that long: a slice is its 40 ms and the 1 KB
// parse chunk that crosses them, 58 ms on average, 3 to 6 in 105 over 100 ms, 184 ms the longest. With the slices
// held: 240 to 245 ms, of which 150 ms are the two slices that still run before the key's edge reaches the loop.
//
// All of it runs on the loop task today, which is the one that waits, so no slice can start then; the rows state the
// rule for whoever moves a lane.
//
// The decision alone, apart from the reader, so the host can test it.
namespace InputFirst {

// A press between the button going down and the turn it becomes.
struct Press {
  bool down = false;         // an edge the loop has not taken in yet (the image decoders' abort signal)
  bool gestureOpen = false;  // a button held, a release waiting out the double-press window, an event not read yet
  bool turnWaiting = false;  // read as a turn, which waits for the render lock
};

enum class Work : uint8_t {
  LookAhead,          // nothing waits on it: a Background-B slice, the image lane, a glyph-store step, a bookmark parse
  ChapterForPage,     // Background-C while the page the reader asked for is not drawn yet: the reader waits on it
  ChapterBehindPage,  // Background-C once that page is drawn: the pages after it, the page's gray, a picture on it
};

// Which of the two a step of the chapter on screen is. Drawn: handed to the panel, its refresh may still run.
constexpr Work chapterWork(const bool askedPageDrawn) {
  return askedPageDrawn ? Work::ChapterBehindPage : Work::ChapterForPage;
}

constexpr bool mayRunSlice(const Work work, const Press& press) {
  switch (work) {
    case Work::LookAhead:
    case Work::ChapterBehindPage:
      return !press.down && !press.gestureOpen && !press.turnWaiting;
    case Work::ChapterForPage:
      return !press.turnWaiting;
  }
  return true;
}

}  // namespace InputFirst
