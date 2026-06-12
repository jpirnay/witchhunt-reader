#include <dlfcn.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../../lib/Epub/Epub/css/CssParser.h"
#include "../../lib/ZipFile/ZipFile.h"
#include "Arduino.h"
#include "HalStorage.h"

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Heap instrumentation: malloc/free/calloc/realloc are interposed and every
// allocation gets a hidden header in front of the user pointer. The header
// carries a magic value (so free/realloc can recognize pointers that did NOT
// come from this malloc — e.g. aligned_alloc, posix_memalign, or allocations
// made inside the hooks themselves — and pass them through untouched) and the
// measurement epoch the block was allocated in (so freeing a block from an
// earlier measurement window can never underflow the live-byte counter of the
// current window).
// ---------------------------------------------------------------------------

static std::atomic<size_t> g_liveBytes{0};
static std::atomic<size_t> g_peakBytes{0};
static std::atomic<uint32_t> g_epoch{0};  // current measurement window; 0 = not measuring
static std::atomic<bool> g_tracking{false};
static thread_local bool g_inHook = false;

static void trackAlloc(size_t sz) {
  size_t live = g_liveBytes.fetch_add(sz) + sz;
  size_t peak = g_peakBytes.load(std::memory_order_relaxed);
  while (live > peak && !g_peakBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
  }
}

static void* rawAlloc(size_t bytes) {
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
}

static void rawFree(void* p) {
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
}

static void* rawRealloc(void* p, size_t bytes) {
  using realloc_fn_t = void* (*)(void*, size_t);
  static realloc_fn_t realRealloc = nullptr;
  if (!realRealloc) {
    realRealloc = reinterpret_cast<realloc_fn_t>(dlsym(RTLD_NEXT, "realloc"));
  }
  return realRealloc ? realRealloc(p, bytes) : nullptr;
}

struct AllocHeader {
  size_t size;
  uint32_t magic;
  uint32_t epoch;  // measurement window this block was allocated in; 0 = untracked
};

static constexpr uint32_t kHeaderMagic = 0xC55AFE17u;
static constexpr size_t kAlign = alignof(std::max_align_t);
static constexpr size_t kHeaderSize = (sizeof(AllocHeader) + kAlign - 1) & ~(kAlign - 1);

// Reading 16 bytes in front of a foreign pointer is technically out of bounds,
// but every allocator we can encounter here keeps its own metadata there, so
// the load is safe in practice; the magic check then rejects the pointer.
static AllocHeader* headerOf(void* ptr) {
  return reinterpret_cast<AllocHeader*>(static_cast<char*>(ptr) - kHeaderSize);
}

extern "C" {

void* malloc(size_t size) {
  if (g_inHook) return rawAlloc(size);  // header-less; free() detects it via the magic check
  g_inHook = true;
  void* raw = rawAlloc(kHeaderSize + size);
  g_inHook = false;
  if (!raw) return nullptr;
  auto* header = static_cast<AllocHeader*>(raw);
  header->size = size;
  header->magic = kHeaderMagic;
  header->epoch = g_tracking ? g_epoch.load(std::memory_order_relaxed) : 0;
  if (header->epoch != 0) trackAlloc(size);
  return static_cast<char*>(raw) + kHeaderSize;
}

void free(void* ptr) {
  if (!ptr) return;
  AllocHeader* header = headerOf(ptr);
  if (header->magic != kHeaderMagic) {
    rawFree(ptr);
    return;
  }
  // Only blocks allocated in the *current* window may shrink its counter.
  if (g_tracking && header->epoch == g_epoch.load(std::memory_order_relaxed)) {
    g_liveBytes.fetch_sub(header->size, std::memory_order_relaxed);
  }
  header->magic = 0;  // poison so a double free is routed to rawFree and crashes loudly there
  g_inHook = true;
  rawFree(header);
  g_inHook = false;
}

void* calloc(size_t nmemb, size_t size) {
  if (size != 0 && nmemb > SIZE_MAX / size) return nullptr;
  const size_t total = nmemb * size;
  void* p = malloc(total);
  if (p) memset(p, 0, total);
  return p;
}

void* realloc(void* ptr, size_t size) {
  if (!ptr) return malloc(size);
  AllocHeader* header = headerOf(ptr);
  if (header->magic != kHeaderMagic) {
    return rawRealloc(ptr, size);
  }
  const size_t oldSize = header->size;
  void* np = malloc(size);
  if (!np) return nullptr;
  memcpy(np, ptr, oldSize < size ? oldSize : size);
  free(ptr);
  return np;
}

}  // extern "C"

static long long elapsedUs(const Clock::time_point& start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

static void beginMeasurement() {
  g_liveBytes.store(0);
  g_peakBytes.store(0);
  g_epoch.fetch_add(1);
  g_tracking.store(true);
}

static void endMeasurement() { g_tracking.store(false); }

// Peak heap while fn runs (allocations minus frees, high-water mark).
static size_t measurePeakBytes(const std::function<void()>& fn) {
  beginMeasurement();
  fn();
  endMeasurement();
  return g_peakBytes.load();
}

// Bytes still allocated when fn returns — the retained footprint of whatever fn built.
static size_t measureLiveBytes(const std::function<void()>& fn) {
  beginMeasurement();
  fn();
  endMeasurement();
  return g_liveBytes.load();
}

static std::vector<uint8_t> readZipEntry(const std::string& epubPath, const char* entryName) {
  ZipFile zip(epubPath);
  size_t size = 0;
  uint8_t* raw = zip.readFileToMemory(entryName, &size, false);
  if (!raw) {
    return {};
  }
  std::vector<uint8_t> buffer(raw, raw + size);
  free(raw);
  return buffer;
}

static std::string makeTempDir() {
  const char* tmpl = "/tmp/cssperf-XXXXXX";
  std::string path(tmpl);
  if (!mkdtemp(&path[0])) {
    return {};
  }
  return path;
}

static bool writeTempCssFile(const std::vector<uint8_t>& data, std::string& outPath) {
  char tmpl[] = "/tmp/cssperf-XXXXXX.css";
  int fd = mkstemps(tmpl, 4);
  if (fd < 0) return false;
  outPath = tmpl;
  std::ofstream out(outPath, std::ios::binary);
  if (!out) {
    close(fd);
    return false;
  }
  out.write(reinterpret_cast<const char*>(data.data()), data.size());
  close(fd);
  return out.good();
}

static bool removePath(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  return !ec;
}

// Restores the stubbed ESP free-heap value even when an ASSERT_* returns early,
// so a low-heap simulation can't leak into tests that run later in this binary.
struct FreeHeapGuard {
  uint32_t saved = ESP.getFreeHeap();
  ~FreeHeapGuard() { ESP.setFreeHeap(saved); }
};

// Must match the defaults in scripts/generate_large_css_epub.py, which produced
// test/fixtures/test_large_css.epub.
constexpr size_t kFixtureRuleCount = 1500;

TEST(CssParserPerf, ParseLargeCssEpub) {
  const std::string epubPath = FIXTURE_EPUB;
  const char* cssEntry = "OEBPS/styles/large.css";

  const std::vector<uint8_t> cssData = readZipEntry(epubPath, cssEntry);
  ASSERT_FALSE(cssData.empty()) << "Failed to read CSS entry from EPUB";

  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(cssData, cssPath));

  size_t parseTimeUs = 0;
  bool parseOk = false;
  size_t ruleCount = 0;

  const size_t heapPeak = measurePeakBytes([&] {
    std::unique_ptr<CssParser> parser = std::make_unique<CssParser>("");
    FsFile cssFile;
    if (!Storage.openFileForRead("CSS", cssPath.c_str(), cssFile)) {
      return;
    }
    const auto t0 = Clock::now();
    if (!parser->loadFromStream(cssFile)) {
      return;
    }
    parseTimeUs = static_cast<size_t>(elapsedUs(t0));
    ruleCount = parser->ruleCount();
    parseOk = true;
  });

  ASSERT_TRUE(parseOk);
  ASSERT_EQ(ruleCount, kFixtureRuleCount);

  printf("BENCHMARK parse_time=%zu us heap_peak=%zu B rule_count=%zu css_bytes=%zu\n", parseTimeUs, heapPeak, ruleCount,
         cssData.size());

  CssParser parser("");
  FsFile cssFile;
  ASSERT_TRUE(Storage.openFileForRead("CSS", cssPath.c_str(), cssFile));
  const size_t parseLiveBytes = measureLiveBytes([&] { ASSERT_TRUE(parser.loadFromStream(cssFile)); });
  printf("PARSE_LIVE_BYTES=%zu\n", parseLiveBytes);

  const std::vector<std::string> testClasses = {"rule0_0", "rule1_0", "rule4_0", "rule7_0"};
  for (const auto& cls : testClasses) {
    const std::string classAttr = cls;
    const CssStyle style = parser.resolveStyle("p", classAttr);
    if (cls == "rule0_0") {
      ASSERT_TRUE(style.hasTextDecoration());
      ASSERT_EQ(style.textDecoration, CssTextDecoration::Underline);
    }
  }

  const auto stats = parser.getResolveStats();
  printf("LOOKUP_STATS resolveCalls=%u mapHits=%u hotHits=%u diskHits=%u misses=%u\n", stats.resolveCalls,
         stats.mapHits, stats.hotHits, stats.diskHits, stats.misses);

  std::filesystem::remove(cssPath);
}

TEST(CssParserPerf, CacheSaveLoadAndLowHeapLookup) {
  const std::string epubPath = FIXTURE_EPUB;
  const char* cssEntry = "OEBPS/styles/large.css";
  const std::string cacheDir = makeTempDir();
  const std::string cacheFileRoot = cacheDir;

  const std::vector<uint8_t> cssData = readZipEntry(epubPath, cssEntry);
  ASSERT_FALSE(cssData.empty()) << "Failed to read CSS entry from EPUB";

  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(cssData, cssPath));

  ASSERT_FALSE(cacheDir.empty());
  CssParser parser(cacheFileRoot);
  {
    FsFile cssFile;
    ASSERT_TRUE(Storage.openFileForRead("CSS", cssPath.c_str(), cssFile));
    ASSERT_TRUE(parser.loadFromStream(cssFile));
  }
  ASSERT_TRUE(parser.saveToCache());
  const std::filesystem::path cacheFilePath = std::filesystem::path(cacheDir) / "css_rules.cache";
  EXPECT_TRUE(std::filesystem::exists(cacheFilePath));
  EXPECT_EQ(parser.ruleCount(), kFixtureRuleCount);

  parser.clear();
  const size_t loadedCacheLiveBytes = measureLiveBytes([&] { ASSERT_TRUE(parser.loadFromCache()); });
  EXPECT_EQ(parser.ruleCount(), kFixtureRuleCount);
  printf("CACHE_LOAD_LIVE_BYTES=%zu\n", loadedCacheLiveBytes);
  // The retained footprint after a cache load is the selector index: one
  // 8-byte (hash, offset) entry per rule. Assert the order of magnitude so a
  // regression back to in-RAM rule storage (~275 KB for this fixture) fails loudly.
  EXPECT_LE(loadedCacheLiveBytes, kFixtureRuleCount * 16);

  {
    CssStyle style = parser.resolveStyle("p", "rule0_0");
    ASSERT_TRUE(style.hasTextDecoration());
    ASSERT_EQ(style.textDecoration, CssTextDecoration::Underline);
  }

  // Force low-heap mode: should bypass disk lookup and not crash.
  FreeHeapGuard heapGuard;
  ESP.setFreeHeap(10 * 1024);
  parser.clear();
  ASSERT_TRUE(parser.loadFromCache());

  const CssStyle lowHeapStyle = parser.resolveStyle("p", "rule0_0");
  EXPECT_FALSE(lowHeapStyle.hasTextDecoration());
  const auto lowHeapStats = parser.getResolveStats();
  printf("LOW_HEAP_STATS resolveCalls=%u mapHits=%u hotHits=%u diskHits=%u lowHeapDiskBypasses=%u misses=%u\n",
         lowHeapStats.resolveCalls, lowHeapStats.mapHits, lowHeapStats.hotHits, lowHeapStats.diskHits,
         lowHeapStats.lowHeapDiskBypasses, lowHeapStats.misses);

  // resolveStyle("p", "rule0_0") probes three selectors — "p", ".rule0_0", and
  // "p.rule0_0" — and each probe must skip its disk lookup in low-heap mode.
  EXPECT_EQ(lowHeapStats.lowHeapDiskBypasses, 3u);
  EXPECT_EQ(lowHeapStats.diskHits, 0u);

  removePath(cacheDir);
  std::filesystem::remove(cssPath);
}
