// CLI companion to the EpubPipeline gtest: compile one EPUB through the real
// pipeline and print the canonical layout dump plus benchmark lines (same
// "BENCHMARK <label> time=<us>us" format as EpubParserBenchmark).
//
// Usage: epub_pipeline_dump <book.epub> [cacheDir] [--bench] [--arena=BYTES] [--shaping-font=FILE] [--viewport=WxH]
// A missing cacheDir uses a fresh temp dir (cold build). Passing the same
// cacheDir twice exercises the warm path. --bench adds per-spine timing,
// whole-run peak heap, and the on-disk cache footprint to stderr, and per page
// the bytes of its lines' arenas (page_bytes), of their glyph forms' records
// (glyph_bytes), and what the same glyphs take as crosspoint-reader #3787's
// tokens (token_bytes: 8 B a glyph, 12 with an offset). --shaping-font names a
// font of test/fixtures/fonts, the shaping face of every font and style.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>  // dladdr, for symbolising alloc sites; MinGW has no dlfcn
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>

#include "Epub/Page.h"
#include "Epub/blocks/TextBlock.h"
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

// A page's text bytes: its lines' arenas, their glyph forms' records, and those glyphs as #3787's tokens.
struct PageBytes {
  size_t page = 0;
  size_t glyphs = 0;
  size_t tokens = 0;

  void add(const TextBlock& block) {
    page += block.arenaBytes();
    for (uint16_t w = 0; w < block.wordCount(); ++w) {
      const GlyphRun run = block.glyphForm(w);
      glyphs += run.bytes;
      forEachGlyph(run, 0, false,
                   [this](const GlyphRecord& record, int) { tokens += (record.dx != 0 || record.dy != 0) ? 12 : 8; });
    }
  }
};

static PageBytes pageBytes(const Page& page) {
  PageBytes bytes;
  for (const auto& element : page.elements) {
    if (element->getTag() == TAG_PageLine) {
      bytes.add(*static_cast<const PageLine&>(*element).getBlock());
    } else if (element->getTag() == TAG_PageTable) {
      for (const auto& row : static_cast<const PageTableFragment&>(*element).getRows()) {
        for (const auto& cell : row.cells) {
          for (const auto& line : cell.lines) bytes.add(*line);
        }
      }
    }
  }
  return bytes;
}

int main(const int argc, char** argv) {
  bool bench = false;
  size_t lentArenaBytes = 0;
  unsigned viewportW = 0, viewportH = 0;
  std::string epubPath, cacheDir, shapingFont;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--bench") == 0) {
      bench = true;
    } else if (std::strncmp(argv[i], "--arena=", 8) == 0) {
      lentArenaBytes = static_cast<size_t>(std::strtoul(argv[i] + 8, nullptr, 10));
    } else if (std::strncmp(argv[i], "--shaping-font=", 15) == 0) {
      shapingFont = argv[i] + 15;
    } else if (std::strncmp(argv[i], "--viewport=", 11) == 0) {
      // As epub_build_inventory's: a landscape X3 is about 770 px wide on the host's metrics.
      if (std::sscanf(argv[i] + 11, "%ux%u", &viewportW, &viewportH) != 2) viewportW = viewportH = 0;
    } else if (epubPath.empty()) {
      epubPath = argv[i];
    } else {
      cacheDir = argv[i];
    }
  }
  if (epubPath.empty()) {
    std::fprintf(stderr,
                 "usage: %s <book.epub> [cacheDir] [--bench] [--arena=BYTES] [--shaping-font=FILE] [--viewport=WxH]\n",
                 argv[0]);
    return 2;
  }
  if (cacheDir.empty()) {
    const auto dir = fs::temp_directory_path() / "epub_pipeline_dump";
    fs::remove_all(dir);
    cacheDir = dir.string();
  }
  fs::create_directories(cacheDir);

  pipeline_harness::SpineStatFn spineStat;
  pipeline_harness::PageStatFn pageStat;
  if (bench) {
    spineStat = [](const int spine, const uint16_t pages, const int64_t us) {
      std::fprintf(stderr, "BENCHMARK spine_%d pages=%u time=%lldus\n", spine, pages, static_cast<long long>(us));
    };
    pageStat = [](const int spine, const uint16_t index, const Page& page) {
      const PageBytes bytes = pageBytes(page);
      std::fprintf(stderr, "BENCHMARK spine_%d page_%u page_bytes=%zu glyph_bytes=%zu token_bytes=%zu\n", spine, index,
                   bytes.page, bytes.glyphs, bytes.tokens);
    };
  }

  if (bench) heapTrackBegin();
  const auto start = std::chrono::steady_clock::now();
  pipeline_harness::Profile profile;
  profile.lentArenaBytes = lentArenaBytes;
  if (!shapingFont.empty()) profile.shapingFontFile = shapingFont.c_str();
  if (viewportW != 0 && viewportH != 0) {
    profile.viewportWidth = static_cast<uint16_t>(viewportW);
    profile.viewportHeight = static_cast<uint16_t>(viewportH);
  }
  pipeline_harness::ArenaStatFn arenaStat;
  if (bench && lentArenaBytes > 0) {
    arenaStat = [](const int spine, const size_t highWater, const size_t capacity) {
      std::fprintf(stderr, "BENCHMARK spine_%d arena_highWater=%zu capacity=%zu\n", spine, highWater, capacity);
    };
  }
  const bool ok = pipeline_harness::runAndDump(epubPath, cacheDir, profile, std::cout, spineStat, arenaStat, pageStat);
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
    // Every populated slot, not a top-N: the peak on an image-heavy book turned out to be spread
    // across hundreds of small holders, which a 40-site cut hid entirely (attributed 3.5 KB of a
    // 179 KB peak). Static so the table does not land on the stack.
    static HeapTrackSite sites[4096];
    const int siteCount = heapTrackTopSites(sites, 4096);
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
#else
      // No dladdr here, so print the offset from the image base instead: addr2line on the
      // .exe resolves it. Without this the profiler's site list is a column of raw pointers.
      rel = sites[i].pc - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
#endif
      std::fprintf(stderr, "\nBENCHMARK alloc_site peakLive=%zu count=%zu bytes=%zu max=%zu off=0x%llx sym=%s",
                   sites[i].peakLive, sites[i].count, sites[i].bytes, sites[i].maxSize, rel, sym);
    }
  }
  std::fprintf(stderr, "\n");
  return ok ? 0 : 1;
}
