// malloc/free/calloc/realloc overrides with a size header word, tracking live
// and peak bytes. Adapted from test/epub_benchmark/EpubParserBenchmark.cpp
// (same technique, stripped to the Linux/glibc path plus the Windows branch).
#include "HeapTrack.h"

#include <atomic>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
// Weak fallbacks must have external linkage — inside the anonymous namespace
// they'd become (undefined) internal symbols instead of binding to glibc.
extern "C" void* __libc_malloc(size_t) __attribute__((weak));
extern "C" void __libc_free(void*) __attribute__((weak));
#endif

namespace {

std::atomic<size_t> g_liveBytes{0};
std::atomic<size_t> g_peakBytes{0};
std::atomic<size_t> g_allocCount{0};
constexpr int kSizeBucketCount = 12;
std::atomic<size_t> g_sizeBuckets[kSizeBucketCount];
std::atomic<bool> g_tracking{false};
thread_local bool g_inHook = false;

// Caller attribution. The size histogram says WHICH SIZE CLASSES dominate the count but not
// WHERE they come from, and on a no-compaction heap the "where" is what you have to change.
// Open-addressed fixed table because this runs inside the malloc hook: any allocation here
// would recurse. Collisions past the probe limit are dropped -- this is a profile, not a ledger.
constexpr int kSiteSlots = 4096;
std::atomic<uintptr_t> g_siteAddr[kSiteSlots];
std::atomic<size_t> g_siteCount[kSiteSlots];
std::atomic<size_t> g_siteBytes[kSiteSlots];

void trackSite(const uintptr_t pc, const size_t sz) {
  if (pc == 0) return;
  size_t h = (pc * 0x9E3779B97F4A7C15ull) >> 49;  // spread the low-entropy high bits of a code addr
  for (int probe = 0; probe < 8; ++probe) {
    const size_t slot = (h + probe) & (kSiteSlots - 1);
    uintptr_t cur = g_siteAddr[slot].load(std::memory_order_relaxed);
    if (cur == 0) {
      uintptr_t expected = 0;
      if (!g_siteAddr[slot].compare_exchange_strong(expected, pc, std::memory_order_relaxed)) {
        if (expected != pc) continue;  // lost the race to a different site; keep probing
      }
      cur = pc;
    }
    if (cur == pc) {
      g_siteCount[slot].fetch_add(1, std::memory_order_relaxed);
      g_siteBytes[slot].fetch_add(sz, std::memory_order_relaxed);
      return;
    }
  }
}

void trackAlloc(size_t sz) {
  if (!g_tracking || g_inHook) return;
  g_allocCount.fetch_add(1, std::memory_order_relaxed);
  // Size histogram: which allocations dominate the count is not obvious from the code — the
  // ones that fragment a no-compaction heap are not necessarily the ones you notice reading it.
  // Buckets are powers of two: [0]=<=16B, [1]=<=32B, ... [11]=>16KB.
  size_t bucket = 0;
  for (size_t limit = 16; bucket < 11 && sz > limit; limit <<= 1) bucket++;
  g_sizeBuckets[bucket].fetch_add(1, std::memory_order_relaxed);
  const size_t live = g_liveBytes.fetch_add(sz) + sz;
  size_t peak = g_peakBytes.load(std::memory_order_relaxed);
  while (live > peak && !g_peakBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
  }
  // Walk out to the first frame above the allocator itself -- the line of application code that
  // wanted the memory, which is the level a fix has to act on. __builtin_return_address(n>0) is
  // documented as unreliable without frame pointers and segfaulted here; backtrace() uses unwind
  // info instead. It can allocate internally, so the hook guard must be held across the call.
  g_inHook = true;
  void* frames[6] = {};
  const int depth = backtrace(frames, 6);
  // 0 = trackAlloc, 1 = the malloc/new override, 2 = libstdc++ operator new for C++ allocations.
  // Take the first frame that lies outside this translation unit's address range by preferring
  // the deepest available, which is the application for both C and C++ paths.
  const int pick = depth > 3 ? 3 : depth - 1;
  const uintptr_t pc = pick >= 0 ? reinterpret_cast<uintptr_t>(frames[pick]) : 0;
  g_inHook = false;
  trackSite(pc, sz);
}
void trackFree(size_t sz) {
  if (!g_tracking || g_inHook) return;
  g_liveBytes.fetch_sub(sz, std::memory_order_relaxed);
}

constexpr size_t kAlign = alignof(std::max_align_t);
constexpr size_t kHeaderSize = (sizeof(size_t) + kAlign - 1) & ~(kAlign - 1);

void* rawAlloc(size_t bytes) {
#if defined(_WIN32)
  return HeapAlloc(GetProcessHeap(), 0, bytes);
#else
  using malloc_fn_t = void* (*)(size_t);
  static malloc_fn_t realMalloc = nullptr;
  if (!realMalloc) {
    realMalloc = reinterpret_cast<malloc_fn_t>(dlsym(RTLD_NEXT, "malloc"));
    if (!realMalloc) realMalloc = __libc_malloc;
  }
  return realMalloc ? realMalloc(bytes) : nullptr;
#endif
}
void rawFree(void* p) {
#if defined(_WIN32)
  HeapFree(GetProcessHeap(), 0, p);
#else
  using free_fn_t = void (*)(void*);
  static free_fn_t realFree = nullptr;
  if (!realFree) {
    realFree = reinterpret_cast<free_fn_t>(dlsym(RTLD_NEXT, "free"));
    if (!realFree) realFree = __libc_free;
  }
  if (realFree) realFree(p);
#endif
}

}  // namespace

extern "C" {

void* malloc(size_t size) {
  if (g_inHook) return rawAlloc(kHeaderSize + size);
  g_inHook = true;
  void* raw = rawAlloc(kHeaderSize + size);
  g_inHook = false;
  if (!raw) return nullptr;
  *static_cast<size_t*>(raw) = size;
  trackAlloc(size);
  return static_cast<char*>(raw) + kHeaderSize;
}

void free(void* ptr) {
  if (!ptr) return;
  if (g_inHook) {
    rawFree(ptr);
    return;
  }
  void* raw = static_cast<char*>(ptr) - kHeaderSize;
  const size_t size = *static_cast<size_t*>(raw);
  trackFree(size);
  g_inHook = true;
  rawFree(raw);
  g_inHook = false;
}

void* calloc(size_t nmemb, size_t size) {
  const size_t total = nmemb * size;
  void* p = malloc(total);
  if (p) memset(p, 0, total);
  return p;
}

void* realloc(void* ptr, size_t size) {
  if (!ptr) return malloc(size);
  void* raw = static_cast<char*>(ptr) - kHeaderSize;
  const size_t oldSize = *static_cast<size_t*>(raw);
  void* np = malloc(size);
  if (!np) return nullptr;
  memcpy(np, ptr, oldSize < size ? oldSize : size);
  free(ptr);
  return np;
}

}  // extern "C"

void heapTrackBegin() {
  // backtrace() lazily initialises (and allocates) on first use; do it before the
  // hook is live so that initialisation is not itself profiled or recursed into.
  void* warm[4];
  (void)backtrace(warm, 4);
  g_liveBytes.store(0);
  g_peakBytes.store(0);
  g_allocCount.store(0);
  for (auto& b : g_sizeBuckets) b.store(0);
  g_tracking.store(true);
}

size_t heapTrackEnd() {
  g_tracking.store(false);
  return g_peakBytes.load();
}

size_t heapTrackAllocCount() { return g_allocCount.load(); }

void heapTrackSizeHistogram(size_t* out, const int count) {
  for (int i = 0; i < count && i < kSizeBucketCount; i++) out[i] = g_sizeBuckets[i].load();
}

void heapTrackPause() { g_tracking.store(false); }
void heapTrackResume() { g_tracking.store(true); }

int heapTrackTopSites(HeapTrackSite* out, const int count) {
  if (out == nullptr || count <= 0) return 0;
  int n = 0;
  for (int slot = 0; slot < kSiteSlots && n < count; ++slot) {
    const size_t c = g_siteCount[slot].load(std::memory_order_relaxed);
    if (c == 0) continue;
    out[n].pc = g_siteAddr[slot].load(std::memory_order_relaxed);
    out[n].count = c;
    out[n].bytes = g_siteBytes[slot].load(std::memory_order_relaxed);
    ++n;
  }
  // Descending by count; n is a few hundred at most, so an insertion sort is ample.
  for (int i = 1; i < n; ++i) {
    HeapTrackSite key = out[i];
    int j = i - 1;
    while (j >= 0 && out[j].count < key.count) {
      out[j + 1] = out[j];
      --j;
    }
    out[j + 1] = key;
  }
  return n;
}
