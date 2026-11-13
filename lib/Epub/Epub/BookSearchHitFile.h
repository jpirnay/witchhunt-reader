#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "BookSearch.h"

namespace BookSearch {

// A search's hits on the card, in the order they were found: fixed records, so the results list reads a window of
// them and the reader one hit by its index, and none of them stays in RAM. A search writes the file afresh; it lives
// in the book's cache folder and goes when the search is closed.
//
// A record: spine (u16), place in the chapter (u16, per mille), visible-text offset (u32), snippet length (u8), then
// the snippet's kSnippetBytes. Little-endian as the device writes it: the file never leaves the device.
class HitFile final : public HitSink {
 public:
  static constexpr size_t kRecordBytes = 2 + 2 + 4 + 1 + Hit::kSnippetBytes;

  // Starts the file afresh at `path`. False when it cannot be written.
  bool create(const std::string& path);
  // Appends a hit. A failed write is logged once and latched (failed()).
  void add(const Hit& hit) override;
  // Puts what was written on the card, so a reader of the file sees it (the list, between chapters).
  void flush();
  // Closes the file; false when any write failed.
  bool close();
  bool failed() const { return failed_; }
  // Hits written since create(): with failed(), the ones the file holds.
  size_t written() const { return written_; }

  // Reads up to `n` hits from `index` on into `out`; how many it read (fewer at the file's end, 0 when it cannot be
  // read).
  static size_t read(const std::string& path, size_t index, Hit* out, size_t n);

 private:
  HalFile file_;
  bool open_ = false;
  bool failed_ = false;
  size_t written_ = 0;
};

}  // namespace BookSearch
