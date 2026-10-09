#pragma once

#include <BuildArena.h>

#include <cstdint>
#include <string>

// Assembles the book index from a build's working files (docs/design/library-index.md, "Building": Publish).
namespace LibraryPublish {

struct Input {
  std::string recordsPath;  // library::BookRecord x bookCount, identity order; pathOff into pathsPath
  std::string pathsPath;    // the books' paths, blob strings
  std::string namesPath;    // author names: uint32_t hash, uint8_t flags, name, sort key (see loadNames)
  uint16_t bookCount = 0;
  uint32_t buildGen = 0;
  uint8_t acceptRules = 0;
  bool partial = false;
};

// Writes the index to outPath + ".tmp" and renames it into place. The author table is sorted in
// `arena` (the lent framebuffer on the device), at most ~22 bytes per book. False, with the previous
// index left as it was, when the arena is too small or a read or write fails.
bool publish(const Input& in, BuildArena& arena, const std::string& outPath);

}  // namespace LibraryPublish
