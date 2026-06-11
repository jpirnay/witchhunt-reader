// Benchmark: measures wall-clock time and peak heap for the XML parsers that
// can run standalone on the host (ContainerParser, TocNcxParser).
// Also measures raw expat parser lifecycle heap (create/free N times).
//
// Run with the moby-dick fixture files extracted from test/fixtures/moby-dick/.
// Output format:
//   BENCHMARK <label> time=<us>us heap_peak=<bytes>B (<KB>KB) [extra]
//
// Timing: microseconds via std::chrono::steady_clock.
// Heap:   Two complementary strategies:
//
//   (A) Cold peak — heapMeasurePeak(fn): takes a heap snapshot BEFORE calling
//       fn, calls fn once, takes another snapshot after, and returns the
//       maximum live bytes above baseline observed. This captures what expat
//       actually holds at peak — including its internal struct, hash tables,
//       and parse buffer — with no CRT pool pre-warming. This is the number
//       that matters for ESP32 capacity planning.
//
//   (B) Timing loop — warm-up + N reps measured together, reporting avg µs.
//       Warm-up runs outside heapMeasurePeak so it does not distort (A).
//
// Windows: uses HeapWalk on the default process heap (catches malloc/expat C
//          allocations as well as C++ new). Falls back to "n/a" on non-Windows.

#include <gtest/gtest.h>

#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// malloc/free interception via weak-symbol override (GCC/Clang, Windows+Linux)
//
// We replace malloc, calloc, realloc, and free with wrappers that track net
// live bytes and peak live bytes during a guarded window. This catches every
// C heap allocation — including expat's internal malloc calls — regardless of
// whether operator new routes through it.
//
// The real allocators are accessed via the underlying __real_malloc etc. on
// Linux (--wrap linker flag) or via a dlopen/GetProcAddress trick. For
// simplicity here we use a re-entrant flag and the platform's underlying
// allocator directly. On Windows with UCRT (MinGW) the CRT malloc is
// replaceable by providing our own definitions; on Linux glibc provides
// __libc_malloc as the real allocator.
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstdlib>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static std::atomic<size_t> g_liveBytes{0};
static std::atomic<size_t> g_peakBytes{0};
static std::atomic<bool> g_tracking{false};
// Re-entrancy guard: our malloc calls HeapAlloc internally on Windows, which
// could recurse. Use a thread-local flag to break the cycle.
static thread_local bool g_inHook = false;

static void trackAlloc(size_t sz) {
  if (!g_tracking || g_inHook) return;
  size_t live = g_liveBytes.fetch_add(sz) + sz;
  size_t peak = g_peakBytes.load(std::memory_order_relaxed);
  while (live > peak && !g_peakBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
  }
}
static void trackFree(size_t sz) {
  if (!g_tracking || g_inHook) return;
  g_liveBytes.fetch_sub(sz, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// On Windows (MinGW/UCRT) malloc is replaceable. We supply our own, call the
// underlying UCRT functions via their internal names.
// On Linux, define them the same way; glibc's __libc_malloc is the real one,
// but for simplicity on both platforms we use a size-header approach so free()
// knows the size without needing _msize().
// ---------------------------------------------------------------------------

// Store the allocation size in a header word prepended to the block.
// The header is padded to max_align_t so the returned pointer is always
// suitably aligned for any type.
static constexpr size_t kAlign = alignof(std::max_align_t);
static constexpr size_t kHeaderSize = (sizeof(size_t) + kAlign - 1) & ~(kAlign - 1);

// Raw allocator that bypasses our wrappers. On Windows (MinGW) HeapAlloc is
// always available and does not recurse through our malloc.
static void* rawAlloc(size_t bytes) {
#if defined(_WIN32)
  return HeapAlloc(GetProcessHeap(), 0, bytes);
#else
  using malloc_fn_t = void* (*)(size_t);
  static malloc_fn_t realMalloc = nullptr;
  if (!realMalloc) {
    realMalloc = reinterpret_cast<malloc_fn_t>(dlsym(RTLD_NEXT, "malloc"));
    if (!realMalloc) {
      extern void* __libc_malloc(size_t) __attribute__((weak));
      realMalloc = __libc_malloc;
    }
  }
  return realMalloc ? realMalloc(bytes) : nullptr;
#endif
}
static void rawFree(void* p) {
#if defined(_WIN32)
  HeapFree(GetProcessHeap(), 0, p);
#else
  using free_fn_t = void (*)(void*);
  static free_fn_t realFree = nullptr;
  if (!realFree) {
    realFree = reinterpret_cast<free_fn_t>(dlsym(RTLD_NEXT, "free"));
    if (!realFree) {
      extern void __libc_free(void*) __attribute__((weak));
      realFree = __libc_free;
    }
  }
  if (realFree) realFree(p);
#endif
}

extern "C" {

void* malloc(size_t size) {
  if (g_inHook) return rawAlloc(kHeaderSize + size);  // re-entrant: skip header
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
  size_t size = *static_cast<size_t*>(raw);
  trackFree(size);
  g_inHook = true;
  rawFree(raw);
  g_inHook = false;
}

void* calloc(size_t nmemb, size_t size) {
  size_t total = nmemb * size;
  void* p = malloc(total);
  if (p) memset(p, 0, total);
  return p;
}

void* realloc(void* ptr, size_t size) {
  if (!ptr) return malloc(size);
  void* raw = static_cast<char*>(ptr) - kHeaderSize;
  size_t oldSize = *static_cast<size_t*>(raw);
  void* np = malloc(size);
  if (!np) return nullptr;
  memcpy(np, ptr, oldSize < size ? oldSize : size);
  free(ptr);
  return np;
}

}  // extern "C"

// operator new/delete route through our malloc/free above automatically.

static size_t heapMeasureLivePeak(const std::function<void()>& createFn, const std::function<void()>& destroyFn) {
  g_liveBytes.store(0);
  g_peakBytes.store(0);
  g_tracking.store(true);
  createFn();
  g_tracking.store(false);
  size_t peak = g_peakBytes.load();
  destroyFn();
  return peak;
}

static void printHeapBytes(size_t peak) {
  if (peak == SIZE_MAX) {
    printf("n/a");
  } else {
    printf("%zuB (%zuKB)", peak, peak / 1024);
  }
}

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

static std::string fixtureDir;

static std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    fprintf(stderr, "Cannot open fixture: %s\n", path.c_str());
    return {};
  }
  return {std::istreambuf_iterator<char>(f), {}};
}

// ---------------------------------------------------------------------------
// Timing helper
// ---------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;

static long long elapsedUs(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

// ---------------------------------------------------------------------------
// Parser includes
// ---------------------------------------------------------------------------

#include <SaxParser/SaxParser.h>

#include "ContainerParser.h"
#include "FsHelpers.h"
#include "TocNcxParser.h"

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

// Raw SaxParser lifecycle: peak live heap while a freshly-created parser exists
// (before destructor), isolating the expat engine's own fixed allocation footprint.
TEST(ExpatBaseline, RawLifecycleHeap) {
  static void (*noop)(void*, const char*, const char**) = [](void*, const char*, const char**) {};
  static void (*noopEnd)(void*, const char*) = [](void*, const char*) {};

  SaxParser* p = nullptr;
  size_t coldPeak = heapMeasureLivePeak(
      [&] {
        p = new SaxParser;
        p->init(nullptr, noop, noopEnd);
      },
      [&] {
        delete p;
        p = nullptr;
      });

  constexpr int REPS = 1000;
  {
    SaxParser w;
    w.init(nullptr, noop, noopEnd);
  }
  auto t0 = Clock::now();
  for (int i = 0; i < REPS; ++i) {
    SaxParser q;
    q.init(nullptr, noop, noopEnd);
  }
  long long us = elapsedUs(t0);

  printf("BENCHMARK expat_raw_lifecycle heap_peak=");
  printHeapBytes(coldPeak);
  printf(" time=%lldus avg_per_iter=%lldus reps=%d\n", us, us / REPS, REPS);
  SUCCEED();
}

// ContainerParser: peak live heap while parser holds all state after a full parse.
TEST(ExpatBaseline, ContainerParser) {
  auto data = readFile(fixtureDir + "/container.xml");
  ASSERT_FALSE(data.empty());

  std::string fullPath;
  ContainerParser* parser = nullptr;
  size_t coldPeak = heapMeasureLivePeak(
      [&] {
        parser = new ContainerParser(data.size());
        parser->setup();
        parser->write(data.data(), data.size());
        fullPath = parser->fullPath;
        // parser is alive — heap snapshot taken here captures peak live bytes
      },
      [&] {
        delete parser;
        parser = nullptr;
      });

  constexpr int REPS = 500;
  {
    ContainerParser w(data.size());
    w.setup();
    w.write(data.data(), data.size());
  }
  auto t0 = Clock::now();
  for (int i = 0; i < REPS; ++i) {
    ContainerParser p(data.size());
    p.setup();
    p.write(data.data(), data.size());
  }
  long long us = elapsedUs(t0);

  printf("BENCHMARK expat_container_parser heap_peak=");
  printHeapBytes(coldPeak);
  printf(" time=%lldus avg_per_iter=%lldus fullPath=%s reps=%d\n", us, us / REPS, fullPath.c_str(), REPS);
  EXPECT_FALSE(fullPath.empty());
}

// TocNcxParser: parse the Moby Dick NCX (32 KB, 138 chapters).
TEST(ExpatBaseline, TocNcxParser) {
  auto data = readFile(fixtureDir + "/toc.ncx");
  ASSERT_FALSE(data.empty());
  const std::string base = "OEBPS/";

  TocNcxParser* parser = nullptr;
  size_t coldPeak = heapMeasureLivePeak(
      [&] {
        parser = new TocNcxParser(base, data.size(), nullptr, nullptr);
        parser->setup();
        parser->write(data.data(), data.size());
      },
      [&] {
        delete parser;
        parser = nullptr;
      });

  constexpr int REPS = 100;
  {
    TocNcxParser w(base, data.size(), nullptr, nullptr);
    w.setup();
    w.write(data.data(), data.size());
  }
  auto t0 = Clock::now();
  for (int i = 0; i < REPS; ++i) {
    TocNcxParser p(base, data.size(), nullptr, nullptr);
    p.setup();
    p.write(data.data(), data.size());
  }
  long long us = elapsedUs(t0);

  printf("BENCHMARK expat_toc_ncx_parser heap_peak=");
  printHeapBytes(coldPeak);
  printf(" time=%lldus avg_per_iter=%lldus input=%zuKB reps=%d\n", us, us / REPS, data.size() / 1024, REPS);
  SUCCEED();
}

// TocNcxParser on a large XHTML chapter (76 KB): stress test for the parse
// buffer growth path. Gives a timing and memory baseline for a large XML parse.
TEST(ExpatBaseline, LargeXhtmlThroughNcxParser) {
  auto data = readFile(fixtureDir + "/chapter-large.xhtml");
  ASSERT_FALSE(data.empty());
  const std::string base = "OEBPS/";

  TocNcxParser* parser = nullptr;
  size_t coldPeak = heapMeasureLivePeak(
      [&] {
        parser = new TocNcxParser(base, data.size(), nullptr, nullptr);
        parser->setup();
        parser->write(data.data(), data.size());
      },
      [&] {
        delete parser;
        parser = nullptr;
      });

  constexpr int REPS = 100;
  {
    TocNcxParser w(base, data.size(), nullptr, nullptr);
    w.setup();
    w.write(data.data(), data.size());
  }
  auto t0 = Clock::now();
  for (int i = 0; i < REPS; ++i) {
    TocNcxParser p(base, data.size(), nullptr, nullptr);
    p.setup();
    p.write(data.data(), data.size());
  }
  long long us = elapsedUs(t0);

  printf("BENCHMARK expat_large_xhtml_parse heap_peak=");
  printHeapBytes(coldPeak);
  printf(" time=%lldus avg_per_iter=%lldus input=%zuKB reps=%d\n", us, us / REPS, data.size() / 1024, REPS);
  SUCCEED();
}

// ---------------------------------------------------------------------------
// main — locate fixture directory relative to executable or FIXTURE_DIR env
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);

  const char* envDir = getenv("FIXTURE_DIR");
  if (envDir) {
    fixtureDir = envDir;
  } else {
    fixtureDir = std::string(argv[0]);
    auto pos = fixtureDir.find_last_of("/\\");
    if (pos != std::string::npos) fixtureDir = fixtureDir.substr(0, pos);
    fixtureDir += "/../../test/fixtures/moby-dick";
  }

  printf("Fixture directory: %s\n", fixtureDir.c_str());
  return RUN_ALL_TESTS();
}
