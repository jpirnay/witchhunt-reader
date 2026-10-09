#pragma once

#include <BuildArena.h>

#include <cstdint>
#include <string>

// Merges a new walk with the previous index (spec section 3.5, phase 2).
namespace LibraryJoin {

struct Input {
  std::string stagePath;     // library::StagedBook x stageCount, in walk order
  uint16_t stageCount = 0;
  std::string previousPath;  // the published index to carry from; missing or invalid = a first build
  bool resolveAll = false;   // Refresh library: carry no author, but keep firstSeen
  std::string recordsPath;   // out: library::BookRecord x stageCount, identity order
  std::string namesPath;     // out: the previous index's authors, so their names are not looked up again
};

struct Result {
  uint16_t pending = 0;   // books whose author has to be resolved
  uint32_t buildGen = 0;  // this build's generation: one up when any book is new
};

// The staged books are sorted by identity in `arena` (16 bytes each) and merged with the previous
// index's records, which are already in identity order, in one sequential pass. A book the index
// knows keeps when it was first seen, and its author too unless its metadata sidecar changed; a new
// book is first seen in this build. False when the arena is too small or a file fails.
bool join(const Input& in, BuildArena& arena, Result& out);

}  // namespace LibraryJoin
