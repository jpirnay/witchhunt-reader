// Device bench for the TTF rasterizer (Stage 2b): every glyph of Bookerly Regular at 14 pt (the body size) and
// at 40 pt (a large heading). The font is embedded in the image (board_build.embed_files), so it is read
// through the flash cache, as the glyph store's mapped styles will be. Prints timings, the scratch block, the
// heap and the loop task's stack, and a hash of the output that must equal the host's
// (bench/ttf_raster_golden.h). Then every glyph emboldened (synthetic bold, bold_14pt_pool4k and bold_40pt_pool4k),
// and the same for the kanji at 12 pt (<name>_kanji_12pt_pool8k_bold). Then every glyph slanted (synthetic italic,
// italic_14pt_pool4k and italic_40pt_pool4k), emboldened then slanted (bolditalic_14pt_pool4k), and the kanji slanted
// (<name>_kanji_12pt_pool8k_italic). Then the kana and kanji of a Japanese subset at
// 12 pt, as CFF with and without subroutines and as TrueType (test/fixtures/fonts/README.md): what decoding a
// charstring costs on a store miss. Ends with "=== done ===".
//
// Build & flash: pio run -e bench_ttf_raster -t upload
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <ttf/TtfFamily.h>
#include <ttf/TtfRasterizer.h>

#include "ttf_raster_golden.h"

extern const uint8_t kFontStart[] asm("_binary_lib_EpdFont_builtinFonts_source_Bookerly_Bookerly_Regular_ttf_start");
extern const uint8_t kFontEnd[] asm("_binary_lib_EpdFont_builtinFonts_source_Bookerly_Bookerly_Regular_ttf_end");
extern const uint8_t kCffStart[] asm("_binary_test_fixtures_fonts_NotoSansJP_Cff_otf_start");
extern const uint8_t kCffEnd[] asm("_binary_test_fixtures_fonts_NotoSansJP_Cff_otf_end");
extern const uint8_t kCffFlatStart[] asm("_binary_test_fixtures_fonts_NotoSansJP_CffFlat_otf_start");
extern const uint8_t kCffFlatEnd[] asm("_binary_test_fixtures_fonts_NotoSansJP_CffFlat_otf_end");
extern const uint8_t kTrueTypeStart[] asm("_binary_test_fixtures_fonts_NotoSansJP_TrueType_ttf_start");
extern const uint8_t kTrueTypeEnd[] asm("_binary_test_fixtures_fonts_NotoSansJP_TrueType_ttf_end");

using namespace ttf;

namespace {

struct Run {
  uint32_t ok = 0;
  uint32_t failed = 0;
  int64_t totalUs = 0;
  int64_t maxUs = 0;
  uint64_t pixels = 0;
  uint32_t hash = ttf_golden::kSeed;
};

// The glyphs of `glyphs` (count of them), or every glyph of the face when it is null. `bold`: each is emboldened
// between load and rasterize, the three timed together as a store miss for a synthetic bold style. `slant`: each is
// sheared after the emboldening, timed with the rest, for a synthetic italic or bold italic style.
Run renderGlyphs(Rasterizer& rasterizer, FontBytes& bytes, const Face& face, const uint16_t* glyphs, uint32_t count,
                 const uint32_t ppem26_6, const bool bold = false, const bool slant = false) {
  Run run;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t g = glyphs ? glyphs[i] : i;
    EpdGlyph metrics{};
    const int64_t start = esp_timer_get_time();
    Status status = rasterizer.load(bytes, face, static_cast<uint16_t>(g), ppem26_6, &metrics);
    if (status == Status::Ok && bold) status = rasterizer.embolden(emboldenStrength(ppem26_6), &metrics);
    if (status == Status::Ok && slant) status = rasterizer.slant(&metrics);
    if (status == Status::Ok) status = rasterizer.rasterize(rasterizer.output());
    const int64_t us = esp_timer_get_time() - start;
    if (status != Status::Ok) {
      ++run.failed;
      continue;
    }
    ++run.ok;
    run.totalUs += us;
    run.maxUs = us > run.maxUs ? us : run.maxUs;
    run.pixels += static_cast<uint64_t>(metrics.width) * metrics.height;
    run.hash = ttf_golden::hashGlyph(run.hash, static_cast<uint16_t>(g), metrics, rasterizer.output());
  }
  return run;
}

Run renderAll(Rasterizer& rasterizer, FontBytes& bytes, const Face& face, const uint32_t ppem26_6,
              const bool bold = false, const bool slant = false) {
  return renderGlyphs(rasterizer, bytes, face, nullptr, face.numGlyphs, ppem26_6, bold, slant);
}

void report(const char* name, const Run& run) {
  Serial.printf("BENCH %-20s glyphs=%u failed=%u avg=%lldus max=%lldus px/glyph=%u total=%lldms hash=%08x\n", name,
                static_cast<unsigned>(run.ok), static_cast<unsigned>(run.failed), run.ok ? run.totalUs / run.ok : 0LL,
                run.maxUs, run.ok ? static_cast<unsigned>(run.pixels / run.ok) : 0u, run.totalUs / 1000,
                static_cast<unsigned>(run.hash));
}

unsigned freeHeap() { return static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)); }
unsigned largestBlock() { return static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)); }

// The Japanese subset's kana (hiragana and katakana) and kanji (test/fixtures/fonts/README.md).
constexpr char32_t kKanji[] = U"日本語文字書読鬱慶議驚龍鑑響漢点線曲円角";
constexpr uint32_t kKanjiCount = sizeof(kKanji) / sizeof(kKanji[0]) - 1;
constexpr uint32_t kKanaCount = (0x3094 - 0x3041) + (0x30F4 - 0x30A1);

// The glyphs of `glyphs` drawn twice each, the second timed: the cost with the code and the font already cached.
Run renderWarm(Rasterizer& rasterizer, FontBytes& bytes, const Face& face, const uint16_t* glyphs, const uint32_t count,
               const uint32_t ppem26_6) {
  Run run;
  for (uint32_t i = 0; i < count; ++i) {
    EpdGlyph metrics{};
    rasterizer.render(bytes, face, glyphs[i], ppem26_6, &metrics);
    const int64_t start = esp_timer_get_time();
    const Status status = rasterizer.render(bytes, face, glyphs[i], ppem26_6, &metrics);
    const int64_t us = esp_timer_get_time() - start;
    if (status != Status::Ok) {
      ++run.failed;
      continue;
    }
    ++run.ok;
    run.totalUs += us;
    run.maxUs = us > run.maxUs ? us : run.maxUs;
    run.pixels += static_cast<uint64_t>(metrics.width) * metrics.height;
    run.hash = ttf_golden::hashGlyph(run.hash, glyphs[i], metrics, rasterizer.output());
  }
  return run;
}

// What another component does to the flash cache between two glyphs (the layout, the store, SdFat): `bytes` of flash
// read, a line at a time. Bookerly's file is flash the Japanese runs never read.
volatile uint32_t gPolluteSink = 0;
void polluteCache(const uint32_t bytes) {
  uint32_t sum = 0;
  for (uint32_t at = 0; at < bytes && kFontStart + at < kFontEnd; at += 32) sum += kFontStart[at];
  gPolluteSink = sum;
}

Run renderPolluted(Rasterizer& rasterizer, FontBytes& bytes, const Face& face, const uint16_t* glyphs,
                   const uint32_t count, const uint32_t ppem26_6, const uint32_t pollute) {
  Run run;
  for (uint32_t i = 0; i < count; ++i) {
    polluteCache(pollute);
    EpdGlyph metrics{};
    const int64_t start = esp_timer_get_time();
    const Status status = rasterizer.render(bytes, face, glyphs[i], ppem26_6, &metrics);
    const int64_t us = esp_timer_get_time() - start;
    if (status != Status::Ok) {
      ++run.failed;
      continue;
    }
    ++run.ok;
    run.totalUs += us;
    run.maxUs = us > run.maxUs ? us : run.maxUs;
    run.pixels += static_cast<uint64_t>(metrics.width) * metrics.height;
    run.hash = ttf_golden::hashGlyph(run.hash, glyphs[i], metrics, rasterizer.output());
  }
  return run;
}

// SD mode without a card: the style's block made from the file (SdBlock) and copied to the heap, the outlines read
// through FontBytes' far range and an SdPageCache from the file in flash, as the card would hand them over (the card's
// own time aside). What the engine spends per glyph reading outlines in SD mode.
struct FlashFile {
  const uint8_t* data;
  uint32_t size;
};
bool flashRead(void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
  const auto* f = static_cast<const FlashFile*>(ctx);
  if (offset > f->size || len > f->size - offset) return false;
  memcpy(dst, f->data + offset, len);
  return true;
}

// One font of the subset at 12 pt: kana and kanji together, the kanji alone (the dense ones), and the kanji
// with the SD mode's 8 KB pool. Then kana and kanji: drawn twice each (warm), with 8 KB of other
// flash read before each glyph (polluted), from a copy of the file in RAM (no flash reads of the font), and in SD mode.
void benchJapanese(const char* name, const uint8_t* start, const uint8_t* end) {
  FontBytes bytes(start, static_cast<uint32_t>(end - start));
  Face face;
  if (parseFace(bytes, &face) != FaceStatus::Ok) {
    Serial.printf("%s: parse FAILED\n", name);
    return;
  }
  uint16_t glyphs[kKanaCount + kKanjiCount];
  uint32_t count = 0;
  for (uint32_t cp = 0x3041; cp < 0x3094; ++cp) glyphs[count++] = glyphIndex(bytes, face, cp);
  for (uint32_t cp = 0x30A1; cp < 0x30F4; ++cp) glyphs[count++] = glyphIndex(bytes, face, cp);
  for (uint32_t i = 0; i < kKanjiCount; ++i) glyphs[count++] = glyphIndex(bytes, face, kKanji[i]);
  const uint16_t* kanji = glyphs + kKanaCount;

  const uint32_t ppem = ppemFromPoints(12);
  Rasterizer rasterizer;
  if (!rasterizer.allocate(planScratch(face, ppem, kPoolBytesFlash))) {
    Serial.printf("%s: FATAL: scratch block\n", name);
    return;
  }
  Serial.printf("%s: font %u B, %u glyphs, scratch %u B\n", name, static_cast<unsigned>(bytes.size()),
                static_cast<unsigned>(face.numGlyphs), static_cast<unsigned>(rasterizer.scratchBytes()));
  char label[40];
  snprintf(label, sizeof(label), "%s_kana+kanji_12pt", name);
  report(label, renderGlyphs(rasterizer, bytes, face, glyphs, count, ppem));
  snprintf(label, sizeof(label), "%s_kanji_12pt", name);
  report(label, renderGlyphs(rasterizer, bytes, face, kanji, kKanjiCount, ppem));
  snprintf(label, sizeof(label), "%s_warm_12pt", name);
  report(label, renderWarm(rasterizer, bytes, face, glyphs, count, ppem));
  snprintf(label, sizeof(label), "%s_polluted8k_12pt", name);
  report(label, renderPolluted(rasterizer, bytes, face, glyphs, count, ppem, 8192));
  {
    const uint32_t size = static_cast<uint32_t>(end - start);
    auto* copy = static_cast<uint8_t*>(malloc(size));
    if (copy) {
      memcpy(copy, start, size);
      FontBytes ram(copy, size);
      Face ramFace;
      if (parseFace(ram, &ramFace) == FaceStatus::Ok) {
        snprintf(label, sizeof(label), "%s_ram_12pt", name);
        report(label, renderGlyphs(rasterizer, ram, ramFace, glyphs, count, ppem));
      }
      free(copy);
    } else {
      Serial.printf("%s: no %u B for the RAM copy\n", name, static_cast<unsigned>(size));
    }
  }
  rasterizer.allocate(planScratch(face, ppem, kPoolBytesSd, true, true));
  snprintf(label, sizeof(label), "%s_kanji_12pt_pool8k", name);
  report(label, renderGlyphs(rasterizer, bytes, face, kanji, kKanjiCount, ppem));
  snprintf(label, sizeof(label), "%s_kanji_12pt_pool8k_bold", name);
  report(label, renderGlyphs(rasterizer, bytes, face, kanji, kKanjiCount, ppem, true));
  snprintf(label, sizeof(label), "%s_kanji_12pt_pool8k_italic", name);
  report(label, renderGlyphs(rasterizer, bytes, face, kanji, kKanjiCount, ppem, false, true));
  snprintf(label, sizeof(label), "%s_kana+kanji_12pt_pool8k", name);
  report(label, renderGlyphs(rasterizer, bytes, face, glyphs, count, ppem));
  {
    FlashFile file{start, static_cast<uint32_t>(end - start)};
    const FileSource source{file.size, flashRead, &file, 1};
    SdBlock* block = new SdBlock();
    SdPageCache* pages = new SdPageCache();
    uint8_t* blockData = nullptr;
    if (block->plan(source) == SdPlan::Ok && (blockData = static_cast<uint8_t*>(malloc(block->bytes())))) {
      FileSource blockSource = block->source();
      const glyphstore::SdOutlines outlines = block->outlines();
      const SdRange range{source, outlines.fontOffset, outlines.fileOffset, outlines.length, pages};
      FontBytes sd(blockData, block->bytes(), &range);
      Face sdFace;
      if (blockSource.read(blockSource.ctx, 0, blockData, block->bytes()) && parseFace(sd, &sdFace) == FaceStatus::Ok) {
        snprintf(label, sizeof(label), "%s_sd_12pt_pool8k", name);
        report(label, renderGlyphs(rasterizer, sd, sdFace, glyphs, count, ppem));
        Serial.printf("%s: SD block %u B, %u page reads\n", name, static_cast<unsigned>(block->bytes()),
                      static_cast<unsigned>(pages->misses()));
      }
    } else {
      Serial.printf("%s: no SD block\n", name);
    }
    free(blockData);
    delete pages;
    delete block;
  }
  rasterizer.release();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== TTF rasterizer device bench (Stage 2b) ===");

  FontBytes bytes(kFontStart, static_cast<uint32_t>(kFontEnd - kFontStart));
  Face face;
  const FaceStatus parsed = parseFace(bytes, &face);
  Serial.printf("font %u B, parse %s, %u glyphs, %u units per em, maxp %u points %u contours\n",
                static_cast<unsigned>(bytes.size()), parsed == FaceStatus::Ok ? "ok" : "FAILED",
                static_cast<unsigned>(face.numGlyphs), static_cast<unsigned>(face.unitsPerEm),
                static_cast<unsigned>(face.maxPoints), static_cast<unsigned>(face.maxContours));
  if (parsed != FaceStatus::Ok) {
    Serial.println("=== done ===");
    return;
  }

  const uint32_t body = ppemFromPoints(14);
  const uint32_t heading = ppemFromPoints(40);
  Rasterizer rasterizer;
  const unsigned heapBefore = freeHeap();
  if (!rasterizer.allocate(planScratch(face, heading, kPoolBytesFlash, true, true))) {
    Serial.println("FATAL: scratch block");
    Serial.println("=== done ===");
    return;
  }
  Serial.printf("scratch %u B (heap %u -> %u B, largest block %u B); output region %u B\n",
                static_cast<unsigned>(rasterizer.scratchBytes()), heapBefore, freeHeap(), largestBlock(),
                static_cast<unsigned>(rasterizer.plan().outputBytes));

  const unsigned stackBefore = static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr));
  const Run text = renderAll(rasterizer, bytes, face, body);
  report("render_14pt_pool4k", text);
  Serial.printf("output hash %08x: %s the host's %08x\n", static_cast<unsigned>(text.hash),
                text.hash == ttf_golden::kBookerlyRegular14pt ? "MATCHES" : "DIFFERS FROM",
                static_cast<unsigned>(ttf_golden::kBookerlyRegular14pt));
  report("render_40pt_pool4k", renderAll(rasterizer, bytes, face, heading));
  report("bold_14pt_pool4k", renderAll(rasterizer, bytes, face, body, true));
  report("bold_40pt_pool4k", renderAll(rasterizer, bytes, face, heading, true));
  report("italic_14pt_pool4k", renderAll(rasterizer, bytes, face, body, false, true));
  report("italic_40pt_pool4k", renderAll(rasterizer, bytes, face, heading, false, true));
  report("bolditalic_14pt_pool4k", renderAll(rasterizer, bytes, face, body, true, true));
  Serial.printf("loop task stack: lowest free %u B before rendering, %u B after\n", stackBefore,
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

  // The pool is a speed dial (spec 2): the same glyphs with 2 KB and with the SD mode's 8 KB.
  rasterizer.allocate(planScratch(face, heading, 2048));
  report("render_14pt_pool2k", renderAll(rasterizer, bytes, face, body));
  rasterizer.allocate(planScratch(face, heading, kPoolBytesSd));
  report("render_14pt_pool8k", renderAll(rasterizer, bytes, face, body));
  report("render_40pt_pool8k", renderAll(rasterizer, bytes, face, heading));
  rasterizer.release();
  Serial.printf("after release: heap %u B\n", freeHeap());

  benchJapanese("truetype", kTrueTypeStart, kTrueTypeEnd);
  benchJapanese("cff_flat", kCffFlatStart, kCffFlatEnd);
  benchJapanese("cff_subrs", kCffStart, kCffEnd);
  Serial.printf("after the Japanese runs: heap %u B\n", freeHeap());
  Serial.println("=== done ===");
}

void loop() { delay(30000); }
