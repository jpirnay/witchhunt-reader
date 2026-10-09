#pragma once

#include <cstdint>

// The Library screen's tabs, in bar order: Books (the folders), Recent, New (the book index's newest)
// and Authors. Stored as APP_STATE.libraryTab, so the order is part of the state file.
enum class LibraryTab : uint8_t { Books, Recent, New, Authors };
constexpr uint8_t LIBRARY_TAB_COUNT = 4;

// A stored tab, or Books when the value is out of range (a state file from another build, or damaged).
constexpr LibraryTab libraryTabFrom(const uint8_t stored) {
  return stored < LIBRARY_TAB_COUNT ? static_cast<LibraryTab>(stored) : LibraryTab::Books;
}

// The tab `direction` steps from `tab`, wrapping at both ends.
constexpr LibraryTab libraryTabStep(const LibraryTab tab, const int direction) {
  const int count = LIBRARY_TAB_COUNT;
  return static_cast<LibraryTab>(((static_cast<int>(tab) + direction) % count + count) % count);
}
