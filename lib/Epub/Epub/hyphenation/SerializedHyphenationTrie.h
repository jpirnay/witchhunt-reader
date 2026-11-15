#pragma once

#include <cstddef>
#include <cstdint>

class PagedTrieReader;

// Lightweight descriptor of a serialized Liang hyphenation trie: in place at `data` (the image, a flash mapping,
// PSRAM), or, with `data` null, read through `reader` from its pack file on the card (PagedTrieReader.h), which must
// stay open as long as the descriptor is used.
struct SerializedHyphenationPatterns {
  size_t rootOffset;
  const std::uint8_t* data;
  size_t size;
  PagedTrieReader* reader = nullptr;

  // Whether there is a trie to walk: a descriptor with neither finds no break.
  bool available() const { return (data != nullptr || reader != nullptr) && size > 0; }
};
