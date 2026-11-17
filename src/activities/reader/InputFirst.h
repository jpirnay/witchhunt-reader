#pragma once

#include <cstdint>

// Input first: the reader's background work starts no unit while a press is on its way to becoming a turn (rule R0 of
// docs/future_work/reader-priority-scheme.md).
//
// The reader's loop hands every pass without an input event to the background work, and a press is no event until it
// is released and read: down, then released, then, with a double-press action set, waiting out its window. Each of
// those passes ran a slice of Background-B's look-ahead build (40 ms and one parse chunk), so the press was read three
// to four slices late. X4, 2026-11-16, B laying out the next chapter: press at 186.775 s, turn seen at 187.138 s,
// eleven pages laid out in between; without B, 111 ms. The build those slices fed was then thrown away by the turn.
//
// A slice of the chapter on screen (Background-C) is another matter: the page the press asks for may be one it has
// not laid out yet, so it goes on through a press and stops only for a turn that already waits for the render lock.
// Both run on the loop task today, which is the one that waits, so no slice can start then; the row states the rule
// for whoever moves either of them.
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
  LookAhead,       // nothing waits on it: a Background-B slice, the image lane, a glyph-store step, a bookmark parse
  CurrentChapter,  // a Background-C slice: the page the reader waits for may come from it
};

constexpr bool mayRunSlice(const Work work, const Press& press) {
  switch (work) {
    case Work::LookAhead:
      return !press.down && !press.gestureOpen && !press.turnWaiting;
    case Work::CurrentChapter:
      return !press.turnWaiting;
  }
  return true;
}

}  // namespace InputFirst
