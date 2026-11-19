#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>

// The pictures whose decode failed for the file's own fault in this reader session: a fixed ring of eight 32-bit
// hashes of the picture's path, no allocation (the reader holds one, EpubReaderActivity::imageDecodeFailures_).
// ImageBlock::render draws a picture found here as a frame without decoding it; without the list every pass of a
// foreground render decoded a cut file again (X4: 8 decodes, about 14 s, per visit of the page). Memory only, so the
// reader's onExit clears it and a wrong verdict lasts no longer than the open book. Eight is a window's worth (the
// page and the five after it) plus a spare; the ninth failure evicts the oldest, which is then decoded once more.
class DecodeFailures {
 public:
  static constexpr size_t kSlots = 8;

  static uint32_t hashOf(const std::string& path) {  // FNV-1a
    uint32_t h = 2166136261u;
    for (const char c : path) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return h;
  }

  bool has(const uint32_t hash) const {
    for (uint8_t i = 0; i < count_; ++i) {
      if (hashes_[i] == hash) return true;
    }
    return false;
  }

  void add(const uint32_t hash) {
    if (has(hash)) return;
    hashes_[next_] = hash;
    next_ = static_cast<uint8_t>((next_ + 1) % kSlots);
    if (count_ < kSlots) ++count_;
  }

  void clear() { count_ = next_ = 0; }

 private:
  uint32_t hashes_[kSlots] = {};
  uint8_t count_ = 0;
  uint8_t next_ = 0;
};
