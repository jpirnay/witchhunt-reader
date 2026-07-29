// G4 — pull-core latency + background-compile-throughput gate (on-device).
//
// Measures the TWO halves of the single-conclusive-source strategy on real hardware, against a real
// book from the SD card, so we have GO/NO-GO numbers BEFORE deleting section files or rewriting the
// reader (docs/remainder-plan-2026-07-29.md §4):
//
//   1. Background-compile throughput — Section::compileBookToContentBin: how long to compile the
//      whole book to content.bin, and the implied per-spine rate (does a spine commit before the
//      reader would reach it? the thing that makes fast-first-page work).
//   2. Live-read latency — compiled::layoutPage (forward) + layoutPageBackward (prev): ms/page and
//      the heap/contig headroom during a turn (the F failure was fragmentation, not exhaustion).
//
// Results print as `BENCH ...` lines over USB-CDC serial, then the chip halts.
//
// Build & flash: pio run -e bench_pagelayout -t upload
// Monitor:       pio device monitor -e bench_pagelayout
//
// Set BENCH_BOOK below to the SD path of the book to measure (King's Avatar / Small Gods for the
// heavy cases named in the plan). The book's cache dir is derived like the reader's.

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <memory>

#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <builtinFonts/all.h>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/content/BlockStreamReader.h"
#include "Epub/content/PageLayout.h"
#include "fontIds.h"

// The book to measure. Point this at a real book on the SD card.
#ifndef BENCH_BOOK
#define BENCH_BOOK "/books/kings-avatar.epub"
#endif

// Layout profile to measure at (mirrors the reader's defaults; keep in sync with a typical setting).
static constexpr int kFontId = BOOKERLY_14_FONT_ID;
static constexpr uint16_t kViewportW = 460;
static constexpr uint16_t kViewportH = 760;

// --- minimal font + renderer bringup, mirroring main.cpp::setupDisplayAndFonts ---

GfxRenderer renderer(display);
FontDecompressor fontDecompressor;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());

EpdFont bookerly14RegularFont(&bookerly_14_regular);
EpdFont bookerly14BoldFont(&bookerly_14_bold);
EpdFont bookerly14ItalicFont(&bookerly_14_italic);
EpdFont bookerly14BoldItalicFont(&bookerly_14_bolditalic);
EpdFontFamily bookerly14FontFamily(&bookerly14RegularFont, &bookerly14BoldFont, &bookerly14ItalicFont,
                                   &bookerly14BoldItalicFont);

static void setupRendererAndFonts() {
  display.begin(false);
  renderer.begin();
  fontDecompressor.init();
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(BOOKERLY_14_FONT_ID, bookerly14FontFamily);
}

static uint32_t freeHeap() { return esp_get_free_heap_size(); }
static uint32_t contigHeap() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT); }

static compiled::LayoutParams makeParams(const std::shared_ptr<Epub>& epub) {
  compiled::LayoutParams p;
  p.fontId = kFontId;
  p.viewportWidth = kViewportW;
  p.viewportHeight = kViewportH;
  p.embeddedStyle = true;
  p.epubFilePath = epub->getPath();
  return p;
}

// --- (1) compile the whole book to content.bin, timed ---

static bool benchCompile(const std::shared_ptr<Epub>& epub) {
  Section::BuildParams bp;
  bp.fontId = kFontId;
  bp.viewportWidth = kViewportW;
  bp.viewportHeight = kViewportH;
  bp.embeddedStyle = true;

  const int spineCount = epub->getSpineItemsCount();
  const uint32_t f0 = freeHeap(), c0 = contigHeap();
  const int64_t t0 = esp_timer_get_time();
  const bool ok = Section::compileBookToContentBin(epub, renderer, bp);
  const int64_t us = esp_timer_get_time() - t0;
  const uint32_t f1 = freeHeap(), c1 = contigHeap();

  Serial.printf("BENCH compile ok=%d spines=%d total=%lldms per_spine=%lldms free=%lu->%lu contig=%lu->%lu\n",
                ok ? 1 : 0, spineCount, us / 1000, spineCount > 0 ? (us / 1000) / spineCount : 0,
                (unsigned long)f0, (unsigned long)f1, (unsigned long)c0, (unsigned long)c1);
  return ok;
}

// --- (2) sweep pages forward then backward, timed per page ---

static void benchLayout(const std::shared_ptr<Epub>& epub) {
  const std::string binPath = epub->getCachePath() + "/content.bin";
  FsFile bin;
  if (!Storage.openFileForRead("BNC", binPath, bin)) {
    Serial.println("BENCH layout ERROR content.bin open failed");
    return;
  }
  compiled::BlockStreamReader reader;
  if (!reader.open(bin)) {
    Serial.println("BENCH layout ERROR content.bin read failed");
    bin.close();
    return;
  }
  const compiled::LayoutParams params = makeParams(epub);

  // FORWARD: chain layoutPage across the whole book (spine by spine), timing each page.
  int64_t fwdTotalUs = 0, fwdMaxUs = 0;
  int fwdPages = 0;
  uint32_t fwdMinContig = contigHeap();
  std::vector<compiled::PagePosition> lastSpineStarts;  // page starts of the LAST spine, for the backward sweep
  uint32_t lastSpine = 0;

  for (uint32_t si = 0; si < reader.spineCount(); ++si) {
    if (!reader.spineAvailable(si)) continue;
    compiled::PagePosition cursor;
    cursor.spineIndex = static_cast<uint16_t>(si);
    std::vector<compiled::PagePosition> spineStarts;
    for (int guard = 0; guard < 100000; ++guard) {
      const int64_t t = esp_timer_get_time();
      compiled::LaidOutPage lp = compiled::layoutPage(reader, renderer, params, cursor);
      const int64_t us = esp_timer_get_time() - t;
      if (!lp.ok || !lp.page) break;
      fwdTotalUs += us;
      if (us > fwdMaxUs) fwdMaxUs = us;
      ++fwdPages;
      const uint32_t cc = contigHeap();
      if (cc < fwdMinContig) fwdMinContig = cc;
      spineStarts.push_back(lp.start);
      if (lp.atSpineEnd) break;
      cursor = lp.end;
    }
    lastSpineStarts = std::move(spineStarts);
    lastSpine = si;
  }

  Serial.printf("BENCH layout_fwd pages=%d avg=%lldus max=%lldus min_contig=%lu\n", fwdPages,
                fwdPages > 0 ? fwdTotalUs / fwdPages : 0, fwdMaxUs, (unsigned long)fwdMinContig);

  // BACKWARD: prev-page across the last spine's page starts, timing each standalone layoutPageBackward.
  // (This is the O(pages) standalone path — the reader will use an O(1) cursor stack in G5; measuring
  // the worst case here bounds the post-jump reconstruction cost.)
  int64_t bwdTotalUs = 0, bwdMaxUs = 0;
  int bwdPages = 0;
  for (size_t k = lastSpineStarts.size(); k >= 2; --k) {
    const int64_t t = esp_timer_get_time();
    compiled::LaidOutPage bwd = compiled::layoutPageBackward(reader, renderer, params, lastSpineStarts[k - 1]);
    const int64_t us = esp_timer_get_time() - t;
    if (!bwd.ok || !bwd.page) break;
    bwdTotalUs += us;
    if (us > bwdMaxUs) bwdMaxUs = us;
    ++bwdPages;
  }
  Serial.printf("BENCH layout_bwd spine=%lu pages=%d avg=%lldus max=%lldus (standalone O(pages) worst case)\n",
                (unsigned long)lastSpine, bwdPages, bwdPages > 0 ? bwdTotalUs / bwdPages : 0, bwdMaxUs);

  bin.close();
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== PageLayout G4 benchmark (compile throughput + live-read latency) ===");
  Serial.printf("CPU %u MHz  free %lu  contig %lu\n", (unsigned)getCpuFrequencyMhz(), (unsigned long)freeHeap(),
                (unsigned long)contigHeap());

  if (!Storage.begin()) {
    Serial.println("BENCH ERROR SD begin failed");
    return;
  }
  setupRendererAndFonts();

  auto epub = std::make_shared<Epub>(BENCH_BOOK, "/.crosspoint");
  const int64_t lt = esp_timer_get_time();
  if (!epub->load(true, false)) {
    Serial.println("BENCH ERROR epub load failed: " BENCH_BOOK);
    return;
  }
  epub->loadImageManifest();
  Serial.printf("BENCH open book=%s spines=%d load=%lldms\n", BENCH_BOOK, epub->getSpineItemsCount(),
                (esp_timer_get_time() - lt) / 1000);

  if (benchCompile(epub)) benchLayout(epub);

  Serial.printf("\nfree after %lu  min-ever %lu\n", (unsigned long)freeHeap(),
                (unsigned long)esp_get_minimum_free_heap_size());
  Serial.println("=== done ===");
}

void loop() {}
