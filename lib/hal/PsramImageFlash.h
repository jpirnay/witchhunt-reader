#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "NorFlash.h"

/// Where a PsramImageFlash keeps its image between boots: a file on the card on the device (CardImageFile), a
/// simulated one in host tests.
class AreaImageFile {
 public:
  /// The file's length; 0 when there is none.
  virtual uint32_t size() = 0;
  /// Copies `len` bytes at `offset`. False on an I/O error or a range past the end.
  virtual bool read(uint32_t offset, void* dst, uint32_t len) = 0;
  /// Writes `len` bytes at `offset`, which is never past the file's end: the file only grows at its end. The bytes may
  /// wait in a cache until sync().
  virtual bool write(uint32_t offset, const void* src, uint32_t len) = 0;
  /// Every byte written so far is on the card.
  virtual bool sync() = 0;
  /// Empties the file, making it when there is none.
  virtual bool truncate() = 0;

 protected:
  ~AreaImageFile() = default;
};

/// The flash area as an image in PSRAM, kept as a file on the card: backend (b) of FlashArea, for a board whose
/// partition table has no `spiffs` partition (docs/design/flash-area.md). The consumers keep their NOR rules:
/// program() only clears bits, erase() sets whole sectors to 0xFF. A mapping is a pointer into the image and costs no
/// MMU page.
///
/// The file is a header block (kHeaderBlockBytes) and then the image. A page (kMapPageBytes) is read from it the first
/// time anything reads, programs or maps it, unless the header's bitmap says the page is all 0xFF: then it is filled
/// in PSRAM and the card is not read (a glyph store maps its whole log region, mostly erased). sync() writes the
/// sectors changed since the last one, between a header that says dirty and one that says clean; the bitmap goes with
/// the clean header, recomputed for the pages that changed. A power cut loses what was not synced, and a cut inside
/// sync() leaves the header dirty, so the next boot starts erased. Each is a state a power cut on NOR flash could have
/// left (any instant between two calls is one), and every consumer is a cache of files on the card.
///
/// Thread-safe, unlike flash needs to be: a lazy load or a dirty bit must not interleave with a sync.
class PsramImageFlash final : public NorFlash {
 public:
  static constexpr uint32_t kHeaderBlockBytes = 4096;

  /// Over `bytes` of `image` (whole pages; the caller's, for this instance's life), kept in `file`.
  PsramImageFlash(uint8_t* image, const uint32_t bytes, AreaImageFile& file)
      : image_(image), bytes_(bytes), file_(file) {}

  /// Reads the file's header. A clean image of this size is used, each page loaded as it is first touched; anything
  /// else gives an erased image, which the next sync() writes out whole. False without memory for the two bitmaps
  /// (~0.1 KB for the default size) or for a size that is not whole pages.
  bool begin();

  uint32_t size() const override { return bytes_; }
  bool read(uint32_t offset, void* dst, uint32_t len) const override;
  bool program(uint32_t offset, const void* src, uint32_t len) override;
  bool erase(uint32_t offset, uint32_t len) override;
  const uint8_t* map(uint32_t offset, uint32_t len) override;
  void unmap(const uint8_t*) override {}
  /// Mapping costs no MMU page here: as many as the image has.
  uint32_t freeMapPages() const override { return bytes_ / kMapPageBytes; }
  /// Writes every sector changed since the last sync to the file. False when the card refused; they stay pending.
  bool sync() override;

 private:
  bool inRange(const uint32_t offset, const uint32_t len) const {
    return loaded_ != nullptr && offset <= bytes_ && len <= bytes_ - offset;
  }
  // Reads the pages [offset, offset + len) spans that are not loaded yet. With `erasing`, a page the range covers
  // whole is not read: it is about to be erased.
  bool load(uint32_t offset, uint32_t len, bool erasing) const;
  void markDirty(uint32_t offset, uint32_t len);
  void startErased();
  bool allLoaded() const;
  bool writeHeader(uint8_t state, bool wholeBlock);

  uint8_t* image_;
  uint32_t bytes_;
  AreaImageFile& file_;
  std::unique_ptr<uint32_t[]> loaded_;  // a bit per page: its bytes are in the image
  std::unique_ptr<uint32_t[]> erased_;  // a bit per page: all 0xFF in the file, so its load reads nothing
  std::unique_ptr<uint32_t[]> dirty_;   // a bit per sector: changed since the last sync
  bool fileWhole_ = false;              // the file holds a whole image of this size, so sync() writes in place
  mutable std::mutex lock_;
};
