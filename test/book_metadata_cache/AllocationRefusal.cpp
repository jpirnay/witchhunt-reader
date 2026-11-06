#include "AllocationRefusal.h"

#include <malloc.h>

#include <cstdint>
#include <cstdlib>
#include <new>

#ifdef _WIN32
#define malloc_usable_size _msize  // MinGW has no glibc call; _msize is the same size of a block
#endif

namespace {
// 0: nothing refused.
size_t refuseFrom = 0;
size_t refuseTo = SIZE_MAX;  // the largest size refused
size_t live = 0;
size_t peak = 0;
size_t allocations = 0;

bool refused(const size_t bytes) { return refuseFrom != 0 && bytes >= refuseFrom && bytes <= refuseTo; }

void* allocateOrNull(const size_t bytes) {
  if (refused(bytes)) return nullptr;
  void* p = std::malloc(bytes == 0 ? 1 : bytes);
  if (p != nullptr) {
    ++allocations;
    live += malloc_usable_size(p);
    if (live > peak) peak = live;
  }
  return p;
}

void* allocateOrThrow(const size_t bytes) {
  void* p = allocateOrNull(bytes);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

void release(void* p) {
  if (p == nullptr) return;
  live -= malloc_usable_size(p);
  std::free(p);
}
}  // namespace

AllocationRefusal::AllocationRefusal(const size_t bytes) {
  refuseFrom = bytes;
  refuseTo = SIZE_MAX;
}
AllocationRefusal::AllocationRefusal(const size_t bytes, const size_t maxBytes) {
  refuseFrom = bytes;
  refuseTo = maxBytes;
}
AllocationRefusal::~AllocationRefusal() {
  refuseFrom = 0;
  refuseTo = SIZE_MAX;
}

size_t heapLiveBytes() { return live; }
size_t heapPeakBytes() { return peak; }
void resetHeapPeak() { peak = live; }
size_t heapAllocations() { return allocations; }

// Every allocating form and every releasing form, so the live count sees each block both ways.
void* operator new(const size_t bytes) { return allocateOrThrow(bytes); }
void* operator new[](const size_t bytes) { return allocateOrThrow(bytes); }
void* operator new(const size_t bytes, const std::nothrow_t&) noexcept { return allocateOrNull(bytes); }
void* operator new[](const size_t bytes, const std::nothrow_t&) noexcept { return allocateOrNull(bytes); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, size_t) noexcept { release(p); }
void operator delete[](void* p, size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
