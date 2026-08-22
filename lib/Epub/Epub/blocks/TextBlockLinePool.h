#pragma once
// Bump pool for TextBlock word-data allocations made while laying out table rows.
//
// Why this exists. A device heap trace of appendix-b of "Through the Brazilian Wilderness"
// (docs/memory-allocation-strategy.md 8.4a) showed table pages taking 130-215 MORE heap blocks
// than the prose pages either side, and giving every byte back afterwards -- yet the largest free
// block fell from 26612 to 20468 and stayed there for the remaining thirty pages. The cost is
// allocation COUNT, not bytes: a prose page holds ~22 lines, one TextBlock each, while a table
// page holds the same text cut into cells (rows x columns x wrapped lines), so it churns three to
// five times as many variable-sized allocations through the general heap.
//
// Only the word-data allocation is pooled, not the shared_ptr control block. That is deliberate
// and is the half worth moving: the word arena is variable-sized, which is what carves the heap
// into unusable gaps, while control blocks are uniform and recycle cleanly.
//
// LIFETIME -- the property that makes this safe. A pooled line's storage must outlive the line,
// and table lines have an awkward shape: flushTableFragment() calls emitPage() while the packer
// still holds laid-out rows, so "reset the pool when the page is emitted" would free storage that
// live TextBlocks still point at. Rather than reason about every emit path, the pool counts live
// lines and rewinds ONLY at zero. A dangling pooled pointer is then not a bug to avoid but a state
// that cannot be represented.
//
// A request that does not fit falls back to the heap (the caller allocates normally and the line
// is not counted), so the worst case is exactly the unpooled behaviour.
#include <BuildArena.h>
#include <Logging.h>

#include <cstddef>
#include <cstdint>

class TextBlockLinePool {
 public:
  explicit TextBlockLinePool(const size_t capacity) : arena_(capacity) {}

  bool valid() const { return arena_.valid(); }
  size_t highWater() const { return arena_.highWater(); }
  size_t failedAllocSize() const { return arena_.failedAllocSize(); }
  uint32_t liveLines() const { return live_; }
  uint32_t fallbacks() const { return fallbacks_; }

  // Bump-allocate storage for one line. Returns nullptr when it does not fit, and the caller
  // falls back to the heap -- the pool must not be told about that line, so no count is taken.
  uint8_t* allocate(const size_t bytes) {
    auto* p = static_cast<uint8_t*>(arena_.alloc(bytes));
    if (p == nullptr) {
      ++fallbacks_;
      return nullptr;
    }
    ++live_;
    return p;
  }

  // One pooled line has been destroyed. At zero the cursor rewinds: nothing points into the
  // arena, so every byte is reclaimable and no later allocation can alias a live line.
  void releaseOne() {
    if (live_ == 0) {
      LOG_ERR("TXB", "line pool released below zero");  // a double free would corrupt the rewind
      return;
    }
    if (--live_ == 0) arena_.reset();
  }

 private:
  BuildArena arena_;
  uint32_t live_ = 0;
  uint32_t fallbacks_ = 0;
};

// Frees TextBlock word storage the way it was allocated. pool == nullptr means plain heap
// storage; otherwise the bytes belong to that pool and are reclaimed by its live-line count
// reaching zero, never by this deleter.
//
// Namespace scope, not nested in TextBlock: a nested deleter's default member initializer is not
// parsed until the enclosing class is complete, so std::unique_ptr's default constructor sees
// is_default_constructible as false at the point the member is declared.
struct TextBlockArenaDeleter {
  TextBlockLinePool* pool = nullptr;
  void operator()(uint8_t* p) const {
    if (p == nullptr) return;
    if (pool != nullptr) {
      pool->releaseOne();
      return;
    }
    delete[] p;
  }
};
