#include "AllocationCounter.h"

#include <cstdlib>
#include <new>

namespace {
long gLiveAllocations = 0;
long gAllocations = 0;
}  // namespace

long liveAllocations() { return gLiveAllocations; }
long allocations() { return gAllocations; }

void* operator new(const std::size_t size) {
  if (void* p = std::malloc(size ? size : 1)) {
    ++gLiveAllocations;
    ++gAllocations;
    return p;
  }
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
  if (p != nullptr) --gLiveAllocations;
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
