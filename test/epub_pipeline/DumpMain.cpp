// CLI companion to the EpubPipeline gtest: compile one EPUB through the real
// pipeline and print the canonical layout dump plus benchmark lines (same
// "BENCHMARK <label> time=<us>us" format as EpubParserBenchmark).
//
// Usage: epub_pipeline_dump <book.epub> [cacheDir] [--bench]
// A missing cacheDir uses a fresh temp dir (cold build). Passing the same
// cacheDir twice exercises the warm path. --bench adds per-spine timing,
// whole-run peak heap, and the on-disk cache footprint to stderr.
#if !defined(_WIN32)
#include <dlfcn.h>  // dladdr, for symbolising alloc sites; MinGW has no dlfcn
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>

#include "HeapTrack.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

static uintmax_t dirBytes(const std::string& dir) {
  uintmax_t total = 0;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
    if (e.is_regular_file(ec)) total += e.file_size(ec);
  }
  return total;
}

int main(const int argc, char** argv) {
  bool bench = false;
  std::string epubPath, cacheDir;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--bench") == 0) {
      bench = true;
    } else if (epubPath.empty()) {
      epubPath = argv[i];
    } else {
      cacheDir = argv[i];
    }
  }
  if (epubPath.empty()) {
    std::fprintf(stderr, "usage: %s <book.epub> [cacheDir] [--bench]\n", argv[0]);
    return 2;
  }
  if (cacheDir.empty()) {
    const auto dir = fs::temp_directory_path() / "epub_pipeline_dump";
    fs::remove_all(dir);
    cacheDir = dir.string();
  }
  fs::create_directories(cacheDir);

  pipeline_harness::SpineStatFn spineStat;
  if (bench) {
    spineStat = [](const int spine, const uint16_t pages, const int64_t us) {
      std::fprintf(stderr, "BENCHMARK spine_%d pages=%u time=%lldus\n", spine, pages, static_cast<long long>(us));
    };
  }

  if (bench) heapTrackBegin();
  const auto start = std::chrono::steady_clock::now();
  const bool ok = pipeline_harness::runAndDump(epubPath, cacheDir, pipeline_harness::Profile{}, std::cout, spineStat);
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
  std::fprintf(stderr, "BENCHMARK pipeline_%s time=%lldus", ok ? "ok" : "FAILED", static_cast<long long>(us.count()));
  if (bench) {
    const size_t peak = heapTrackEnd();
    std::fprintf(stderr, " heap_peak=%zuB (%zuKB) allocs=%zu cache_bytes=%ju", peak, peak / 1024, heapTrackAllocCount(),
                 static_cast<uintmax_t>(dirBytes(cacheDir)));
    size_t buckets[12] = {};
    heapTrackSizeHistogram(buckets, 12);
    std::fprintf(stderr, "\nBENCHMARK alloc_sizes");
    const char* labels[12] = {"<=16", "<=32", "<=64", "<=128", "<=256", "<=512",
                              "<=1K", "<=2K", "<=4K", "<=8K",  "<=16K", ">16K"};
    for (int i = 0; i < 12; i++) {
      if (buckets[i]) std::fprintf(stderr, " %s=%zu", labels[i], buckets[i]);
    }
    // Where the allocations come from, not just how big they are. Raw addresses; pipe through
    // addr2line against this binary to get file:line.
    HeapTrackSite sites[40];
    const int siteCount = heapTrackTopSites(sites, 40);
    for (int i = 0; i < siteCount; i++) {
      // Resolve in-process: the binary is position-independent, so a raw runtime address means
      // nothing to addr2line without the load base. dladdr gives both the symbol and the base,
      // and the base-relative offset is what addr2line can turn into file:line.
      const char* sym = "?";
      unsigned long long rel = sites[i].pc;
#if !defined(_WIN32)
      Dl_info info{};
      if (dladdr(reinterpret_cast<void*>(static_cast<uintptr_t>(sites[i].pc)), &info) != 0) {
        if (info.dli_sname != nullptr) sym = info.dli_sname;
        if (info.dli_fbase != nullptr) rel = sites[i].pc - reinterpret_cast<uintptr_t>(info.dli_fbase);
      }
#endif
      std::fprintf(stderr, "\nBENCHMARK alloc_site count=%zu bytes=%zu off=0x%llx sym=%s", sites[i].count,
                   sites[i].bytes, rel, sym);
    }
  }
  std::fprintf(stderr, "\n");
  return ok ? 0 : 1;
}
