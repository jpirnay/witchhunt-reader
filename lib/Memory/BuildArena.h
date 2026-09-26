#pragma once
// Bump arena for the section-build path (docs/compiled-book-pipeline-plan.md
// Phase 2). One up-front allocation sized to a named budget replaces the
// per-site free-heap gates: allocation inside the build becomes deterministic
// (either the arena fits the budget or the build refuses to start), so
// mid-build OOM/fragmentation surprises — the heap-recovery-restart class of
// bugs — cannot occur.
//
// Lifetime model (FreeInkBook-inspired):
//   - alloc() bump-allocates; there is NO per-object free, so fragmentation
//     cannot accumulate.
//   - reserveBlock()/release(block) scope transient work (inflate window,
//     parse chunks). A release can only restore the exact start of the newest
//     live block; arbitrary offsets and out-of-order releases are rejected.
//   - reset() discards everything at a build boundary.
//
// Diagnostics for host-side budget tests and device logs:
//   - highWater(): peak cursor ever reached (last successful state).
//   - failedAllocSize(): size of the last REFUSED allocation (0 = none) —
//     distinct from highWater(), which only records successes; together they
//     reproduce device OOM conditions exactly in host tests.
//   - beginLane()/laneHighWater(): the peak reached above the cursor at the last
//     beginLane(). A build calls beginLane() at each phase boundary so its
//     summary line can say how much each phase (extract ring, parse working set)
//     used on top of what was resident when it started -- the per-lane figures
//     the arena budget is derived from (memory audit 2026-09, R3).
//
// Heap discipline: the backing buffer comes from makeUniqueNoThrow (never a
// throwing new); valid() must be checked before use.
#include <Memory.h>

#include <cassert>
#include <cstddef>
#include <cstdint>

class BuildArena {
 public:
  class Block {
   public:
    Block() = default;
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    Block(Block&& other) noexcept { *this = static_cast<Block&&>(other); }
    Block& operator=(Block&& other) noexcept {
      if (this != &other) {
        // Overwriting a still-live token orphans its scope in the arena's active
        // chain (the cursor can no longer be rewound to it): a caller must release
        // or commit a block before reassigning the variable that holds it.
        assert(!valid() && "overwriting a live BuildArena::Block token");
        start_ = other.start_;
        id_ = other.id_;
        parentId_ = other.parentId_;
        owner_ = other.owner_;
        other.id_ = 0;
        other.owner_ = nullptr;
      }
      return *this;
    }
    bool valid() const { return id_ != 0; }
    size_t start() const { return start_; }

   private:
    friend class BuildArena;
    Block(BuildArena* owner, const size_t start, const uint32_t id, const uint32_t parentId)
        : start_(start), id_(id), parentId_(parentId), owner_(owner) {}

    size_t start_ = 0;
    uint32_t id_ = 0;
    uint32_t parentId_ = 0;
    BuildArena* owner_ = nullptr;
  };

  // Heap-backed arena. valid() is false when the buffer allocation failed —
  // callers treat that exactly like a refused build (retry released/smaller).
  // Members init in declaration order (owned_, base_, capacity_): owned_ allocates,
  // base_ views it, capacity_ collapses to 0 when the allocation failed.
  explicit BuildArena(const size_t capacity)
      : owned_(makeUniqueNoThrow<uint8_t[]>(capacity)), base_(owned_.get()), capacity_(base_ ? capacity : 0) {}

  // Caller-supplied buffer (e.g. a released framebuffer region). Not owned.
  BuildArena(uint8_t* buffer, const size_t capacity) : base_(buffer), capacity_(buffer ? capacity : 0) {}

  BuildArena(const BuildArena&) = delete;
  BuildArena& operator=(const BuildArena&) = delete;

  bool valid() const { return base_ != nullptr; }
  size_t capacity() const { return capacity_; }
  size_t used() const { return cursor_; }
  size_t highWater() const { return highWater_; }
  size_t failedAllocSize() const { return failedAllocSize_; }
  // Lane telemetry (diagnostic only): mark the cursor as a lane's floor, then read the peak
  // reached above it. Only alloc() moves the peak; releases never lower it.
  void beginLane() {
    laneStart_ = cursor_;
    laneHighWater_ = cursor_;
  }
  size_t laneHighWater() const { return laneHighWater_ > laneStart_ ? laneHighWater_ - laneStart_ : 0; }
  uint32_t releaseFailures() const { return releaseFailures_; }

  // Bump-allocate `bytes` aligned to `align` (power of two). Returns nullptr
  // when the request does not fit; the arena state is unchanged apart from
  // failedAllocSize() so a caller can log the exact shortfall.
  void* alloc(const size_t bytes, const size_t align = alignof(std::max_align_t)) {
    if (!base_ || align == 0 || (align & (align - 1)) != 0 || cursor_ > SIZE_MAX - (align - 1)) {
      failedAllocSize_ = bytes;
      return nullptr;
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(base_) + cursor_;
    if (address < reinterpret_cast<uintptr_t>(base_) || address > UINTPTR_MAX - (align - 1)) {
      failedAllocSize_ = bytes;
      return nullptr;
    }
    const size_t padding = static_cast<size_t>((0 - address) & (align - 1));
    const size_t aligned = cursor_ + padding;
    if (aligned > capacity_ || bytes > capacity_ - aligned || bytes > UINTPTR_MAX - (address + padding)) {
      failedAllocSize_ = bytes;
      return nullptr;
    }
    cursor_ = aligned + bytes;
    if (cursor_ > highWater_) highWater_ = cursor_;
    if (cursor_ > laneHighWater_) laneHighWater_ = cursor_;
    return base_ + aligned;
  }

  template <typename T>
  T* allocArray(const size_t count) {
    if (count != 0 && count > SIZE_MAX / sizeof(T)) {
      failedAllocSize_ = SIZE_MAX;
      return nullptr;
    }
    return static_cast<T*>(alloc(count * sizeof(T), alignof(T)));
  }

  // Start a transient allocation block at the current cursor. Blocks must be
  // released newest-first, which prevents one owner from rewinding through a
  // newer owner's reservation.
  Block reserveBlock() {
    uint32_t id = nextBlockId_++;
    if (id == 0) id = nextBlockId_++;
    Block block(this, cursor_, id, activeBlockId_);
    activeBlockId_ = id;
    return block;
  }

  // Reclaim exactly the allocations made since this block was reserved.
  // Returns false for stale, duplicate, or out-of-order releases.
  bool release(Block& block) {
    if (!block.valid() || block.owner_ != this || block.id_ != activeBlockId_) {
      ++releaseFailures_;
      return false;
    }
    cursor_ = block.start_;
    activeBlockId_ = block.parentId_;
    block.id_ = 0;
    block.owner_ = nullptr;
    return true;
  }

  // Keep this block's allocations while ending its transient scope. This lets
  // later blocks be reserved and released without retaining an unreachable
  // live token for permanent arena data.
  bool commit(Block& block) {
    if (!block.valid() || block.owner_ != this || block.id_ != activeBlockId_) {
      ++releaseFailures_;
      return false;
    }
    activeBlockId_ = block.parentId_;
    block.id_ = 0;
    block.owner_ = nullptr;
    return true;
  }

  // Discard everything (build boundary). highWater/failedAllocSize survive so
  // post-build diagnostics can still report the peak.
  void reset() {
    cursor_ = 0;
    activeBlockId_ = 0;
    laneStart_ = 0;
    laneHighWater_ = 0;
  }

 private:
  std::unique_ptr<uint8_t[]> owned_;
  uint8_t* base_ = nullptr;
  size_t capacity_ = 0;
  size_t cursor_ = 0;
  size_t highWater_ = 0;
  size_t laneStart_ = 0;
  size_t laneHighWater_ = 0;
  size_t failedAllocSize_ = 0;
  uint32_t activeBlockId_ = 0;
  uint32_t nextBlockId_ = 1;
  uint32_t releaseFailures_ = 0;
};
