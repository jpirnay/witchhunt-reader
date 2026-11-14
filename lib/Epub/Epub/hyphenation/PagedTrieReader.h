#pragma once

#include <cstdint>

#include "HyphenationPack.h"

// A hyphenation trie read in place from its pack file on the card, a page at a time: what the Liang walk reads on a
// board with nowhere to copy the trie to, a C3 whose partition table has no hyphenation region (HyphenationPacks.cpp).
// Pages are kPageBytes on kPageBytes offsets of the FILE, so each is one card sector; the trie starts `offset` bytes
// into the first. The least recently used page is read over.
//
// A read that fails, or an address past the trie, makes the reader fail: at() returns 0 from then on without reading
// the card, until clearFailure(). The walk checks failed() and drops the word's breaks, so a card error costs that
// word its hyphens, never a break taken from bytes that were not read.
//
// Header-only: every target that builds the Liang walk instantiates it over this class. Not thread-safe; section
// builds, the only users of the walk, run on the loop task.
class PagedTrieReader {
 public:
  static constexpr uint32_t kPageBytes = 512;
  static constexpr uint8_t kPages = 8;

  // `src` must outlive the reader. The trie is the `size` bytes from `offset` of the source (hyphpack::kHeaderBytes).
  PagedTrieReader(HyphenationPackSource& src, const uint32_t offset, const uint32_t size)
      : src_(src), offset_(offset), size_(size) {
    for (uint32_t& index : index_) index = kNoPage;
  }
  PagedTrieReader(const PagedTrieReader&) = delete;
  PagedTrieReader& operator=(const PagedTrieReader&) = delete;

  uint32_t size() const { return size_; }

  // The trie's byte at `addr`; 0 and failed() when it cannot be had.
  uint8_t at(const uint32_t addr) {
    if (failed_ || addr >= size_) {
      failed_ = true;
      return 0;
    }
    const uint32_t pos = offset_ + addr;
    // A node's bytes, and the next node's often, lie in the page the last byte came from.
    if (index_[last_] != pos / kPageBytes && !load(pos / kPageBytes)) return 0;
    return data_[last_][pos % kPageBytes];
  }

  bool failed() const { return failed_; }
  void clearFailure() { failed_ = false; }
  // Pages read from the source since the reader was made.
  uint32_t misses() const { return misses_; }

 private:
  static constexpr uint32_t kNoPage = UINT32_MAX;

  // Makes page `index` the last one, reading it over the least recently used when no page holds it.
  bool load(const uint32_t index) {
    uint8_t slot = 0;
    for (uint8_t i = 0; i < kPages; ++i) {
      if (index_[i] == index) {
        used_[i] = ++clock_;
        last_ = i;
        return true;
      }
      if (used_[i] < used_[slot]) slot = i;
    }
    // at() asks only for a page with trie bytes in it, so `start` lies before the trie's end.
    const uint32_t start = index * kPageBytes;
    const uint32_t end = offset_ + size_;
    const uint32_t bytes = end - start < kPageBytes ? end - start : kPageBytes;
    index_[slot] = kNoPage;  // a failed read leaves the page empty
    if (!src_.read(start, data_[slot], bytes)) {
      failed_ = true;
      return false;
    }
    ++misses_;
    index_[slot] = index;
    used_[slot] = ++clock_;
    last_ = slot;
    return true;
  }

  HyphenationPackSource& src_;
  uint32_t offset_;
  uint32_t size_;
  uint32_t index_[kPages];      // the page's file offset / kPageBytes; kNoPage: empty
  uint32_t used_[kPages] = {};  // the clock at the last use: the smallest is read over
  uint32_t clock_ = 0;
  uint32_t misses_ = 0;
  uint8_t last_ = 0;
  bool failed_ = false;
  uint8_t data_[kPages][kPageBytes] = {};
};
