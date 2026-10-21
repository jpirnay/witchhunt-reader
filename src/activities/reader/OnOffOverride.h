#pragma once

#include <cstdint>

// A per-book override (-1 = follow the global setting, 0 = off, 1 = on) shown as a plain Off / On row: the reader
// menu's Bionic reading. The row shows what the page does, so it cannot say "follow the global setting", and the
// value it shows is not the override the book holds.
//
// The menu used to hand back the shown value. A book without an override then came back 0 or 1 on every close, even a
// plain Back, and applyBookReaderOverrides() took that for a change: it pinned the book to the global value of the
// moment (later changes of the global setting no longer reached it), rewrote recent.json and dropped the section for a
// relayout. An action picked in that same visit found no section: X3, 2026-10-21, "Push progress from this device" on
// page 19 of 83 uploaded the first page of the chapter.
//
// So the row maps both ways. The mapping alone, apart from the menu, so the host can test it.
namespace OnOffOverride {

// What the page does, and so what the row shows.
constexpr bool isOn(const int8_t value, const bool globalOn) { return value >= 0 ? value > 0 : globalOn; }

// The override to hand back when the row shows `on`. Back on what the row showed at open is the override the book
// opened with, so a row flipped there and back changes nothing; anything else is the user's choice for this book.
constexpr int8_t forChoice(const bool on, const int8_t opened, const bool globalOn) {
  return on == isOn(opened, globalOn) ? opened : static_cast<int8_t>(on ? 1 : 0);
}

}  // namespace OnOffOverride
