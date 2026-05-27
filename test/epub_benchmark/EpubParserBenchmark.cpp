// Benchmark: measures wall-clock time and peak heap for the XML parsers that
// can run standalone on the host (ContainerParser, TocNcxParser).
// Also measures raw expat parser lifecycle heap (create/free N times).
//
// Run with the moby-dick fixture files extracted from test/fixtures/moby-dick/.
// Output format:
//   BENCHMARK <label> time=<ms>ms heap_peak=<KB>KB [extra]

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// Heap tracking via malloc/free wrappers.
// We wrap malloc/free on Linux/Mac; on Windows use _malloc_dbg or a simple
// atomic counter via operator new override.
#include <atomic>
#include <new>

static std::atomic<size_t> g_currentHeap{0};
static std::atomic<size_t> g_peakHeap{0};
static std::atomic<bool> g_trackingEnabled{false};

// Reset peak counters before a benchmark run.
static void heapResetPeak() { g_peakHeap.store(g_currentHeap.load()); }

static void heapStartTracking() {
  g_trackingEnabled = true;
  heapResetPeak();
}

static size_t heapStopTracking() {
  g_trackingEnabled = false;
  return g_peakHeap.load();
}

#if defined(_WIN32)
// On Windows, override operator new/delete for tracking.
// This is coarse (catches all allocations in the process) but sufficient for
// relative comparison between expat and yxml runs.
void* operator new(size_t sz) {
  void* p = malloc(sz);
  if (!p) throw std::bad_alloc{};
  if (g_trackingEnabled) {
    size_t cur = g_currentHeap.fetch_add(sz) + sz;
    size_t peak = g_peakHeap.load();
    while (cur > peak && !g_peakHeap.compare_exchange_weak(peak, cur)) {
    }
  }
  return p;
}
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t sz) noexcept {
  if (g_trackingEnabled && sz > 0) g_currentHeap.fetch_sub(sz);
  free(p);
}
#endif

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

static long long elapsedMs(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
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

// Raw SaxParser lifecycle: measures heap used to create and destroy a SaxParser
// (with a no-op start/end handler), isolating the parser engine overhead.
TEST(ExpatBaseline, RawLifecycleHeap) {
  constexpr int REPS = 10;
  static void (*noop)(void*, const char*, const char**) = [](void*, const char*, const char**) {};
  static void (*noopEnd)(void*, const char*) = [](void*, const char*) {};
  heapStartTracking();
  auto t0 = Clock::now();
  for (int i = 0; i < REPS; ++i) {
    SaxParser p;
    p.init(nullptr, noop, noopEnd);
    // destructor frees the parser
  }
  long long ms = elapsedMs(t0);
  size_t peak = heapStopTracking();
  printf("BENCHMARK expat_raw_lifecycle time=%lldms heap_peak=%zuKB reps=%d\n", ms, peak / 1024, REPS);
  SUCCEED();
}

// ContainerParser: parse container.xml
TEST(ExpatBaseline, ContainerParser) {
  auto data = readFile(fixtureDir + "/container.xml");
  ASSERT_FALSE(data.empty());

  heapStartTracking();
  auto t0 = Clock::now();

  ContainerParser parser(data.size());
  ASSERT_TRUE(parser.setup());
  parser.write(data.data(), data.size());

  long long ms = elapsedMs(t0);
  size_t peak = heapStopTracking();

  printf("BENCHMARK expat_container_parser time=%lldms heap_peak=%zuKB fullPath=%s\n", ms, peak / 1024,
         parser.fullPath.c_str());
  EXPECT_FALSE(parser.fullPath.empty());
}

// TocNcxParser: parse the Moby Dick NCX (32 KB, 138 chapters)
TEST(ExpatBaseline, TocNcxParser) {
  auto data = readFile(fixtureDir + "/toc.ncx");
  ASSERT_FALSE(data.empty());

  heapStartTracking();
  auto t0 = Clock::now();

  const std::string base = "OEBPS/";
  TocNcxParser parser(base, data.size(), nullptr, nullptr);
  ASSERT_TRUE(parser.setup());
  parser.write(data.data(), data.size());

  long long ms = elapsedMs(t0);
  size_t peak = heapStopTracking();

  printf("BENCHMARK expat_toc_ncx_parser time=%lldms heap_peak=%zuKB input=%zuKB\n", ms, peak / 1024,
         data.size() / 1024);
  SUCCEED();
}

// TocNcxParser with large chapter XHTML as input (stress test XML tokeniser):
// Re-uses TocNcxParser on an HTML file — it will parse without crashing even
// though the HTML isn't NCX-shaped (callbacks see unfamiliar tags, just no-op).
// Gives us a timing baseline for a 77 KB XML parse under expat.
TEST(ExpatBaseline, LargeXhtmlThroughNcxParser) {
  auto data = readFile(fixtureDir + "/chapter-large.xhtml");
  ASSERT_FALSE(data.empty());

  heapStartTracking();
  auto t0 = Clock::now();

  const std::string base = "OEBPS/";
  TocNcxParser parser(base, data.size(), nullptr, nullptr);
  ASSERT_TRUE(parser.setup());
  parser.write(data.data(), data.size());

  long long ms = elapsedMs(t0);
  size_t peak = heapStopTracking();

  printf("BENCHMARK expat_large_xhtml_parse time=%lldms heap_peak=%zuKB input=%zuKB\n", ms, peak / 1024,
         data.size() / 1024);
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
    // Assume executable lives in build/ under test/epub_benchmark; go up two
    // levels and look for test/fixtures/moby-dick.
    fixtureDir = std::string(argv[0]);
    // Strip executable name
    auto pos = fixtureDir.find_last_of("/\\");
    if (pos != std::string::npos) fixtureDir = fixtureDir.substr(0, pos);
    fixtureDir += "/../../test/fixtures/moby-dick";
  }

  printf("Fixture directory: %s\n", fixtureDir.c_str());
  return RUN_ALL_TESTS();
}
