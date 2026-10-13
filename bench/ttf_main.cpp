// THROWAWAY: TTF step 0 benchmark. Never merged. Spec: docs/superpowers/specs/2026-10-13-ttf-fonts-design.md
// (section 9.1). Measures what the TTF design's gates depend on, plus one page of prose drawn with a
// built-in font, with a TTF on first contact and with a TTF whose glyphs are already stored. Prints
// integers only, then idles.
//
// Build & flash: pio run -e bench_ttf -t upload
// It ERASES the first 1 MB of the spiffs partition (the .cpfont flash cache; the reader re-caches it).

#include <Arduino.h>
#include <BoardConfig.h>
#include <EpdFont.h>
#include <EpdFontData.h>
#include <FontDecompressor.h>
#include <Gpos.h>
#include <HalGPIO.h>
#include <HalI2cBus.h>
#include <HalPowerManager.h>
#include <HalSpiBus.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <Logging.h>  // redirects Serial to a deliberately undefined proxy; this sketch prints through logSerial
#include <builtinFonts/bookerly_14_regular.h>
#include <builtinFonts/notosans_14_regular.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <spi_flash_mmap.h>

#include <algorithm>
#include <cstring>

#include "ttf/gen/bookerly.h"
#include "ttf/gen/cjk_subset.h"
#include "ttf/gen/notosans.h"
#include "ttf/ittf.h"

// Every timed loop folds its work into this, and it is printed once at the end, so -O2 cannot
// delete a loop whose result is otherwise unused.
static uint32_t sink = 0;
static int64_t nowUs() { return esp_timer_get_time(); }
static uint32_t lcg(uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  return s;
}

// ---- rendering ------------------------------------------------------------------------------

static ITtf g_font;
static ITtfOutline g_outline;  // ~17 KB in .bss; production sizes this from maxp (spec 5.3)
static uint8_t g_out[4096];

static uint32_t g_latin[189];
static uint32_t g_latinCount = 0;
static uint32_t g_cjk[520];
static uint32_t g_cjkCount = 0;

static void buildCodepointSets() {
  for (uint32_t c = 0x21; c <= 0x7E; ++c) g_latin[g_latinCount++] = c;
  for (uint32_t c = 0xA1; c <= 0xFF; ++c) g_latin[g_latinCount++] = c;
  for (uint32_t c = 0x4E00; c <= 0x9FA5; c += 41) g_cjk[g_cjkCount++] = c;  // same stride as make_cjk_subset.py
}

struct RenderResult {
  uint32_t coldNs = 0;  // first pass: flash cache cold for this font's outlines
  uint32_t warmNs = 0;  // mean of the next 5 passes
  uint32_t rendered = 0;
  uint32_t maxBytes = 0;
};

static RenderResult timeRender(const uint8_t* data, uint32_t len, int px, const uint32_t* cps, uint32_t n) {
  RenderResult r;
  if (ittf_init(&g_font, data, len) != 0) {
    logSerial.println("    ittf_init FAILED");
    return r;
  }
  constexpr int kWarmPasses = 5;
  uint64_t warmTotalNs = 0;
  for (int pass = 0; pass <= kWarmPasses; ++pass) {
    uint32_t count = 0;
    const int64_t t0 = nowUs();
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t g = ittf_glyph(&g_font, cps[i]);
      if (!g) continue;
      ITtfBitmap bm;
      const int rc = ittf_render_2bit(&g_font, g, px << 6, &g_outline, g_out, sizeof g_out, &bm);
      if (rc < 0) continue;
      sink += static_cast<uint32_t>(bm.width) + (rc > 0 ? g_out[0] : 0u);
      r.maxBytes = std::max(r.maxBytes, static_cast<uint32_t>(bm.width * bm.height + 3) / 4);
      ++count;
    }
    const uint64_t ns = static_cast<uint64_t>(nowUs() - t0) * 1000u / (count ? count : 1);
    if (pass == 0) {
      r.coldNs = static_cast<uint32_t>(ns);
    } else {
      warmTotalNs += ns;
    }
    r.rendered = count;
  }
  r.warmNs = static_cast<uint32_t>(warmTotalNs / kWarmPasses);
  return r;
}

static void benchRender() {
  struct Case {
    const char* name;
    const uint8_t* data;
    uint32_t len;
    const uint32_t* cps;
    uint32_t n;
  };
  const Case cases[] = {
      {"Bookerly", kBookerly, kBookerlyLen, g_latin, g_latinCount},
      {"NotoSans", kNotoSans, kNotoSansLen, g_latin, g_latinCount},
      {"CJK", kCjkSubset, kCjkSubsetLen, g_cjk, g_cjkCount},
  };
  const unsigned stackBefore = static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr));
  for (const Case& c : cases) {
    for (const int px : {29, 54}) {  // 14 pt and 26 pt at 150 DPI
      const RenderResult r = timeRender(c.data, c.len, px, c.cps, c.n);
      logSerial.printf("  render %-8s %2d px/em: cold %6u ns/glyph, warm %6u ns/glyph (%u glyphs, max %u B)\n", c.name,
                    px, (unsigned)r.coldNs, (unsigned)r.warmNs, (unsigned)r.rendered, (unsigned)r.maxBytes);
    }
  }
  const unsigned stackAfter = static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr));
  logSerial.printf("  loop-task stack high-water: %u B free before rendering, %u B after\n", stackBefore, stackAfter);
}

// ---- kerning --------------------------------------------------------------------------------

static uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

static uint32_t findTable(const uint8_t* d, uint32_t len, const char* tag, uint32_t* tableLen) {
  const uint32_t n = (uint32_t(d[4]) << 8) | d[5];
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t r = 12 + 16 * i;
    if (r + 16 > len) break;
    if (memcmp(d + r, tag, 4) == 0) {
      *tableLen = be32(d + r + 12);
      return be32(d + r + 8);
    }
  }
  return 0;
}

static void benchKerning(const char* name, const uint8_t* d, uint32_t len) {
  uint32_t gposLen = 0;
  const uint32_t gpos = findTable(d, len, "GPOS", &gposLen);
  if (!gpos) {
    logSerial.printf("  kerning %-8s: no GPOS table\n", name);
    return;
  }
  if (ittf_init(&g_font, d, len) != 0) return;
  static const char kChars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,;:'-()";
  constexpr size_t kN = sizeof kChars - 1;
  uint32_t gids[kN];
  for (size_t i = 0; i < kN; ++i) gids[i] = ittf_glyph(&g_font, static_cast<unsigned char>(kChars[i]));
  uint32_t nonzero = 0;
  const int64_t t0 = nowUs();
  for (size_t a = 0; a < kN; ++a) {
    for (size_t b = 0; b < kN; ++b) {
      const int32_t v = freeink::font::gpos::PairKernAdjustment(d + gpos, gposLen, gids[a], gids[b]);
      sink += static_cast<uint32_t>(v);
      nonzero += v != 0;
    }
  }
  const int64_t us = nowUs() - t0;
  logSerial.printf("  kerning %-8s: %lld ns/pair (GPOS %u B), %u of %u pairs non-zero\n", name,
                (long long)(us * 1000 / (int64_t)(kN * kN)), (unsigned)gposLen, (unsigned)nonzero,
                (unsigned)(kN * kN));
}

// ---- flash: erase, program, index probe ------------------------------------------------------

constexpr uint32_t kIndexOffset = 512 * 1024;  // inside the erased first 1 MB
constexpr uint32_t kIndexSlots = 4096;         // 32 KB of 8-byte slots
constexpr uint32_t kKeys = 2000;               // ~49% load

static uint32_t hashSlot(uint32_t key) { return (key * 2654435761u) >> (32 - 12); }

static uint32_t makeKey(uint32_t& s) {  // spec 4.3: cp | style << 21 | sizeIndex << 23
  const uint32_t cp = 0x20 + lcg(s) % (0x9FFF - 0x20);
  const uint32_t style = (lcg(s) >> 8) & 3;
  const uint32_t size = (lcg(s) >> 8) & 3;
  return cp | (style << 21) | (size << 23);
}

static const esp_partition_t* spiffsPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
}

static void benchFlash() {
  const esp_partition_t* p = spiffsPartition();
  if (!p || p->size < (1u << 20) + 0x10000) {
    logSerial.println("  spiffs partition missing or too small");
    return;
  }
  logSerial.printf("  partition at 0x%06x, %u B; erasing its first 1 MB\n", (unsigned)p->address, (unsigned)p->size);

  int64_t t0 = nowUs();
  esp_partition_erase_range(p, 0, 0x10000);
  logSerial.printf("  erase 64 KB: %lld us\n", (long long)(nowUs() - t0));
  t0 = nowUs();
  esp_partition_erase_range(p, 0, 1u << 20);
  logSerial.printf("  erase 1 MB: %lld us\n", (long long)(nowUs() - t0));

  static uint8_t rec[252];
  for (size_t i = 0; i < sizeof rec; ++i) rec[i] = static_cast<uint8_t>(i * 37 + 1);
  uint32_t off = 0;
  for (const uint32_t size : {100u, 252u}) {
    t0 = nowUs();
    for (int i = 0; i < 200; ++i) {
      esp_partition_write(p, off, rec, size);
      off += size;
    }
    logSerial.printf("  program a %u-byte record: %lld us\n", (unsigned)size, (long long)((nowUs() - t0) / 200));
  }

  static uint8_t occupied[kIndexSlots / 8];
  memset(occupied, 0, sizeof occupied);
  static uint32_t keys[kKeys];
  uint32_t seed = 12345;
  t0 = nowUs();
  for (uint32_t k = 0; k < kKeys; ++k) {
    const uint32_t key = makeKey(seed);
    keys[k] = key;
    uint32_t slot = hashSlot(key);
    while (occupied[slot >> 3] & (1u << (slot & 7))) slot = (slot + 1) & (kIndexSlots - 1);
    occupied[slot >> 3] |= static_cast<uint8_t>(1u << (slot & 7));
    const uint32_t entry[2] = {key, k * 128u};
    esp_partition_write(p, kIndexOffset + slot * 8, entry, sizeof entry);
  }
  logSerial.printf("  program an 8-byte index slot: %lld us\n", (long long)((nowUs() - t0) / kKeys));

  const void* mapped = nullptr;
  esp_partition_mmap_handle_t handle;
  if (esp_partition_mmap(p, kIndexOffset, 0x10000, ESP_PARTITION_MMAP_DATA, &mapped, &handle) != ESP_OK) {
    logSerial.println("  mmap of the index failed");
    return;
  }
  const uint32_t* slots = static_cast<const uint32_t*>(mapped);
  uint32_t missSeed = 999;
  uint32_t hits = 0;
  constexpr int kProbes = 10000;
  t0 = nowUs();
  for (int i = 0; i < kProbes; ++i) {
    // Odd i: a stored key. Even i: bit 27 is never set by makeKey(), so a guaranteed miss.
    const uint32_t key = (i & 1) ? keys[(i * 7) % kKeys] : (makeKey(missSeed) | (1u << 27));
    uint32_t slot = hashSlot(key);
    for (;;) {
      const uint32_t k = slots[slot * 2];
      if (k == key) {
        ++hits;
        sink += slots[slot * 2 + 1];
        break;
      }
      if (k == 0xFFFFFFFFu) break;
      slot = (slot + 1) & (kIndexSlots - 1);
    }
  }
  const int64_t probeUs = nowUs() - t0;
  esp_partition_munmap(handle);
  logSerial.printf("  index probe (half hits): %lld ns per lookup, %u hits of %d\n",
                (long long)(probeUs * 1000 / kProbes), (unsigned)hits, kProbes);
  logSerial.printf("  free MMU pages (this bench image, nothing mapped): %u\n",
                (unsigned)spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA));
}

// ---- one page: built-in font vs TTF on first contact vs TTF with a warm store -----------------
//
// The same page of prose, laid out with each font's own advances into a 1-bit 480x800 buffer
// (2-bit coverage >= 2 is ink, as the BW pass draws it). Only glyph acquisition differs:
//   built-in   FontDecompressor prewarm of the page, then getGlyph + getBitmap per character;
//   TTF cold   an empty store: every distinct glyph is rendered and written (record + index slot);
//   TTF warm   the same page again: every glyph is a hash probe into the mapped index and a record
//              read straight out of the mapped log.

static const char kPage[] =
    "It is a truth universally acknowledged, that a single man in possession of a good fortune, must "
    "be in want of a wife. However little known the feelings or views of such a man may be on his first "
    "entering a neighbourhood, this truth is so well fixed in the minds of the surrounding families, that "
    "he is considered the rightful property of some one or other of their daughters. \"My dear Mr. "
    "Bennet,\" said his lady to him one day, \"have you heard that Netherfield Park is let at last?\" Mr. "
    "Bennet replied that he had not. \"But it is,\" returned she; \"for Mrs. Long has just been here, and "
    "she told me all about it.\" Mr. Bennet made no answer. \"Do you not want to know who has taken it?\" "
    "cried his wife impatiently. \"You want to tell me, and I have no objection to hearing it.\" This was "
    "invitation enough. \"Why, my dear, you must know, Mrs. Long says that Netherfield is taken by a young "
    "man of large fortune from the north of England; that he came down on Monday in a chaise and four to "
    "see the place, and was so much delighted with it, that he agreed with Mr. Morris immediately; that he "
    "is to take possession before Michaelmas, and some of his servants are to be in the house by the end "
    "of next week.\"";

constexpr int kPageW = 480;
constexpr int kPageH = 800;
constexpr int kMargin = 10;
static uint8_t g_fb[kPageW * kPageH / 8];

static void blit2bit(const uint8_t* bm, int w, int h, int x0, int y0) {
  if (!bm) return;
  for (int y = 0; y < h; ++y) {
    const int Y = y0 + y;
    if (Y < 0 || Y >= kPageH) continue;
    for (int x = 0; x < w; ++x) {
      const int X = x0 + x;
      if (X < 0 || X >= kPageW) continue;
      const int p = y * w + x;
      if (((bm[p >> 2] >> (6 - 2 * (p & 3))) & 3) >= 2) g_fb[(Y * kPageW + X) >> 3] |= static_cast<uint8_t>(0x80 >> (X & 7));
    }
  }
}

struct Pen {
  int x = kMargin;
  int baseline = 0;
  int lineHeight = 0;
  void advance(int px) {
    x += px;
  }
  void wrapFor(int px) {
    if (x + px > kPageW - kMargin) {
      x = kMargin;
      baseline += lineHeight;
    }
  }
};

static uint32_t distinctChars() {
  bool seen[128] = {};
  uint32_t n = 0;
  for (const char* p = kPage; *p; ++p) {
    const auto c = static_cast<unsigned char>(*p);
    if (c < 128 && !seen[c]) {
      seen[c] = true;
      ++n;
    }
  }
  return n;
}

static FontDecompressor g_decompressor;

static void pageBuiltIn(const char* name, const EpdFontData* data) {
  const EpdFont font(data);
  for (int run = 0; run < 2; ++run) {
    memset(g_fb, 0, sizeof g_fb);
    g_decompressor.clearCache();
    const int64_t t0 = nowUs();
    g_decompressor.prewarmCache(data, kPage);
    const int64_t t1 = nowUs();
    Pen pen;
    pen.lineHeight = data->advanceY;
    pen.baseline = kMargin + data->ascender;
    for (const char* p = kPage; *p; ++p) {
      const EpdGlyphRef g = font.getGlyph(static_cast<unsigned char>(*p));
      if (!g) continue;
      const int adv = g.advanceX >> 4;
      pen.wrapFor(adv);
      blit2bit(g_decompressor.getBitmap(data, g, g.index), g.width, g.height, pen.x + g.left, pen.baseline - g.top);
      pen.advance(adv);
    }
    const int64_t t2 = nowUs();
    sink += g_fb[1000] + static_cast<uint32_t>(pen.baseline);
    logSerial.printf("  page built-in %-8s run %d: %lld us (prewarm %lld + draw %lld)\n", name, run + 1,
                  (long long)(t2 - t0), (long long)(t1 - t0), (long long)(t2 - t1));
  }
  g_decompressor.clearCache();
}

// A minimal store laid out as spec 4.3/4.4 describe: index slot {key, logOffset}; log record
// [u32 key][EpdGlyph][2-bit bitmap], padded to 4. Log at partition offset 0, index after it.
constexpr uint32_t kLogBytes = 256 * 1024;
constexpr uint32_t kStoreIndexOffset = kLogBytes;
constexpr uint32_t kStoreSlots = 4096;
constexpr uint32_t kMissing = 0xFFFFFFFEu;

struct PageStore {
  const esp_partition_t* part = nullptr;
  const uint8_t* log = nullptr;
  const uint32_t* index = nullptr;
  esp_partition_mmap_handle_t logHandle = 0;
  esp_partition_mmap_handle_t indexHandle = 0;
  uint32_t logEnd = 0;
  uint8_t occupied[kStoreSlots / 8] = {};
};

static bool storeOpen(PageStore& s) {
  s.part = spiffsPartition();
  if (!s.part) return false;
  esp_partition_erase_range(s.part, 0, kLogBytes + 0x10000);
  s.logEnd = 0;
  memset(s.occupied, 0, sizeof s.occupied);
  const void* p = nullptr;
  if (esp_partition_mmap(s.part, 0, kLogBytes, ESP_PARTITION_MMAP_DATA, &p, &s.logHandle) != ESP_OK) return false;
  s.log = static_cast<const uint8_t*>(p);
  if (esp_partition_mmap(s.part, kStoreIndexOffset, 0x10000, ESP_PARTITION_MMAP_DATA, &p, &s.indexHandle) != ESP_OK) {
    return false;
  }
  s.index = static_cast<const uint32_t*>(p);
  return true;
}

static void storeClose(PageStore& s) {
  if (s.indexHandle) esp_partition_munmap(s.indexHandle);
  if (s.logHandle) esp_partition_munmap(s.logHandle);
  s.indexHandle = s.logHandle = 0;
}

// Returns the slot's logOffset, or 0xFFFFFFFF when the key is not stored.
static uint32_t storeFind(const PageStore& s, uint32_t key, uint32_t* slotOut) {
  uint32_t slot = (key * 2654435761u) >> (32 - 12);
  for (;;) {
    const uint32_t k = s.index[slot * 2];
    if (k == key) return s.index[slot * 2 + 1];
    if (k == 0xFFFFFFFFu) {
      *slotOut = slot;
      return 0xFFFFFFFFu;
    }
    slot = (slot + 1) & (kStoreSlots - 1);
  }
}

struct ColdCounters {
  int64_t renderUs = 0;
  int64_t writeUs = 0;
  uint32_t rendered = 0;
};

// The record for `cp`: from the store on a hit; rendered, written and then read back on a miss.
static const EpdGlyph* storeGlyph(PageStore& s, uint32_t cp, int32_t ppem26_6, ColdCounters& c) {
  uint32_t slot = 0;
  uint32_t off = storeFind(s, cp, &slot);
  if (off == kMissing) return nullptr;
  if (off != 0xFFFFFFFFu) return reinterpret_cast<const EpdGlyph*>(s.log + off + 4);

  const int64_t t0 = nowUs();
  const uint32_t g = ittf_glyph(&g_font, cp);
  ITtfBitmap bm;
  const int rc = g ? ittf_render_2bit(&g_font, g, ppem26_6, &g_outline, g_out, sizeof g_out, &bm) : -1;
  const int64_t t1 = nowUs();
  c.renderUs += t1 - t0;
  uint32_t entry[2] = {cp, kMissing};
  if (rc >= 0) {
    const uint32_t bytes = rc > 0 ? static_cast<uint32_t>(bm.width * bm.height + 3) / 4 : 0;
    struct {
      uint32_t key;
      EpdGlyph glyph;
    } head{};
    head.key = cp;
    head.glyph.width = static_cast<uint8_t>(bm.width);
    head.glyph.height = static_cast<uint8_t>(bm.height);
    head.glyph.advanceX = static_cast<uint16_t>(bm.advance26_6 >> 2);  // 26.6 -> 12.4
    head.glyph.left = static_cast<int16_t>(bm.left);
    head.glyph.top = static_cast<int16_t>(bm.top);
    head.glyph.dataLength = static_cast<uint16_t>(bytes);
    head.glyph.dataOffset = s.logEnd + sizeof head;
    esp_partition_write(s.part, s.logEnd, &head, sizeof head);
    if (bytes) esp_partition_write(s.part, s.logEnd + sizeof head, g_out, (bytes + 3) & ~3u);
    entry[1] = s.logEnd;
    s.logEnd += sizeof head + ((bytes + 3) & ~3u);
    ++c.rendered;
  }
  esp_partition_write(s.part, kStoreIndexOffset + slot * 8, entry, sizeof entry);
  c.writeUs += nowUs() - t1;
  return entry[1] == kMissing ? nullptr : reinterpret_cast<const EpdGlyph*>(s.log + entry[1] + 4);
}

static void pageTtf(const char* name, const uint8_t* data, uint32_t len) {
  constexpr int32_t kPpem14pt = (14 * 150 * 64 + 36) / 72;  // 14 pt at 150 DPI in 26.6, as fontconvert sizes it
  if (ittf_init(&g_font, data, len) != 0) {
    logSerial.printf("  page TTF %-8s: ittf_init FAILED\n", name);
    return;
  }
  PageStore store;
  if (!storeOpen(store)) {
    logSerial.printf("  page TTF %-8s: store setup FAILED\n", name);
    storeClose(store);
    return;
  }
  const int32_t asc = (g_font.ascender * kPpem14pt / g_font.upem + 32) >> 6;
  const int32_t lineH = ((g_font.ascender - g_font.descender) * kPpem14pt / g_font.upem + 63) >> 6;
  for (int run = 0; run < 2; ++run) {
    memset(g_fb, 0, sizeof g_fb);
    ColdCounters counters;
    const int64_t t0 = nowUs();
    Pen pen;
    pen.lineHeight = lineH;
    pen.baseline = kMargin + asc;
    for (const char* p = kPage; *p; ++p) {
      const EpdGlyph* g = storeGlyph(store, static_cast<unsigned char>(*p), kPpem14pt, counters);
      if (!g) continue;
      const int adv = g->advanceX >> 4;
      pen.wrapFor(adv);
      blit2bit(g->dataLength ? store.log + g->dataOffset : nullptr, g->width, g->height, pen.x + g->left,
               pen.baseline - g->top);
      pen.advance(adv);
    }
    const int64_t t1 = nowUs();
    sink += g_fb[1000] + static_cast<uint32_t>(pen.baseline);
    logSerial.printf("  page TTF %-8s %s: %lld us (render %lld + store writes %lld; %u glyphs rendered, log %u B)\n",
                  name, run == 0 ? "first contact" : "warm store   ", (long long)(t1 - t0),
                  (long long)counters.renderUs, (long long)counters.writeUs, (unsigned)counters.rendered,
                  (unsigned)store.logEnd);
  }
  storeClose(store);
}

static void benchPage() {
  logSerial.printf("  page: %u characters, %u distinct\n", (unsigned)strlen(kPage), (unsigned)distinctChars());
  if (!g_decompressor.init()) {
    logSerial.println("  FontDecompressor::init FAILED");
    return;
  }
  pageBuiltIn("Bookerly", &bookerly_14_regular);
  pageTtf("Bookerly", kBookerly, kBookerlyLen);
  pageBuiltIn("NotoSans", &notosans_14_regular);
  pageTtf("NotoSans", kNotoSans, kNotoSansLen);
}

// ---- SD -------------------------------------------------------------------------------------

static bool g_sdReady = false;

static void bringUpSd() {
  // The part of src/main.cpp setup() that SD access depends on, in the same order.
  HalSystem::begin();
  HalSpiBus::begin();
  HalI2cBus::begin();
  gpio.begin();
  HalI2cBus::ensureBusStarted();
  powerManager.begin();
  g_sdReady = Storage.begin();
  logSerial.printf("  SD card: %s\n", g_sdReady ? "mounted" : "NOT mounted");
}

// The largest regular file in `dir` and its immediate subdirectories, written into `path`.
static uint32_t largestFileIn(const char* dir, char* path, size_t pathLen) {
  uint32_t best = 0;
  HalFile d = Storage.open(dir);
  if (!d || !d.isDirectory()) return 0;
  char name[96];
  for (HalFile e = d.openNextFile(); e; e = d.openNextFile()) {
    e.getName(name, sizeof name);
    char child[160];
    snprintf(child, sizeof child, "%s/%s", dir, name);
    if (e.isDirectory()) {
      e.close();
      char sub[160];
      const uint32_t s = largestFileIn(child, sub, sizeof sub);
      if (s > best) {
        best = s;
        snprintf(path, pathLen, "%s", sub);
      }
      continue;
    }
    const uint32_t s = static_cast<uint32_t>(e.fileSize());
    if (s > best) {
      best = s;
      snprintf(path, pathLen, "%s", child);
    }
  }
  return best;
}

static void benchSd() {
  if (!g_sdReady) {
    logSerial.println("  SD 512 B read: SKIPPED (no card)");
    return;
  }
  char path[160] = "/bench/cjk.ttf";
  HalFile f = Storage.open(path);
  if (!f || f.fileSize() < 1024 * 1024) {
    if (f) f.close();
    if (largestFileIn("/fonts", path, sizeof path) < 64 * 1024) {
      logSerial.println("  SD 512 B read: SKIPPED (no /bench/cjk.ttf and no file over 64 KB in /fonts)");
      return;
    }
    f = Storage.open(path);
  }
  if (!f) {
    logSerial.printf("  SD 512 B read: SKIPPED (cannot open %s)\n", path);
    return;
  }
  const uint32_t size = static_cast<uint32_t>(f.fileSize());
  constexpr int kReads = 500;
  static uint32_t offsets[kReads];
  uint32_t seed = 4242;
  for (int i = 0; i < kReads; ++i) offsets[i] = (lcg(seed) % (size - 512)) & ~511u;
  static uint8_t buf[512];
  for (int sorted = 0; sorted < 2; ++sorted) {
    if (sorted) std::sort(offsets, offsets + kReads);
    const int64_t t0 = nowUs();
    for (int i = 0; i < kReads; ++i) {
      f.seekSet(offsets[i]);
      sink += static_cast<uint32_t>(f.read(buf, sizeof buf)) + buf[7];
    }
    logSerial.printf("  SD 512 B read, %s offsets: %lld us each (%s, %u B)\n", sorted ? "sorted" : "random",
                  (long long)((nowUs() - t0) / kReads), path, (unsigned)size);
  }
  f.close();
}

// ---- main ---------------------------------------------------------------------------------------

void setup() {
  BoardConfig::holdPowerRails();
  delay(250);
  logSerial.begin(115200);
  delay(8000);  // time for capture.py to attach after `pio run -t upload` resets the chip
  logSerial.println();
  logSerial.println("=== TTF step 0 benchmark ===");
  buildCodepointSets();
  logSerial.println("[render]");
  benchRender();
  logSerial.println("[kerning]");
  benchKerning("Bookerly", kBookerly, kBookerlyLen);
  benchKerning("NotoSans", kNotoSans, kNotoSansLen);
  logSerial.println("[flash]");
  benchFlash();
  logSerial.println("[page]");
  benchPage();
  logSerial.println("[sd]");
  bringUpSd();
  benchSd();
  logSerial.printf("(sink=%lu, printed so the optimiser keeps the work above)\n", (unsigned long)sink);
  logSerial.println("=== done ===");
  logSerial.println("Reflash the reader: pio run -e default -t upload");
}

void loop() {
  delay(30000);
  logSerial.println("[idle] TTF benchmark finished; reflash the reader with: pio run -e default -t upload");
}
