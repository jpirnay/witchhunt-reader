#include "JpegToFramebufferConverter.h"

#include <AdaptiveTone.h>
#include <BitmapHelpers.h>
#include <BuildArena.h>
#include <CooperativeAbort.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HalSystem.h>  // feedWatchdog()
#include <Logging.h>
#include <Memory.h>
#include <ProgressiveJpeg.h>
#include <ProgressiveJpegDc.h>
#include <ZipFile.h>
#include <tjpgd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

#include "../blocks/ImageBlock.h"  // image_scratch: pass-wide decode arena
#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
#include <esp_heap_caps.h>
#endif

namespace {

#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
// Heap-corruption tripwire for the cover-JPEG decode path (the buildSection warm pass
// where the multi_heap poisoning crash surfaces). Walks the whole heap and names the
// checkpoint; the symbolized crash shows the poison is only DETECTED at cache.abort()'s
// SD-buffer free, so these checks before/after jd_decomp() convict the writer:
//   jpg_decode_entry corrupt   -> corruption is UPSTREAM of the JPEG path (CSS/build)
//   jpg_before_decode corrupt  -> the ditherer/cache-stream setup allocs trashed it
//   jpg_after_decode corrupt   -> the decode itself (band-cache / pixel writes) is the writer
// Same caveat as EpubReaderActivity::checkHeapIntegrity: only for diagnostic builds.
void jpgCheckHeap(const char* checkpoint) {
  if (heap_caps_check_integrity_all(true)) return;
  LOG_ERR("JPG", "HEAP CORRUPT at checkpoint: %s", checkpoint);
}
#else
inline void jpgCheckHeap(const char*) {}
#endif

// Partial sums for area-averaged downscaling across decode-block edges. A destination pixel
// belongs to the block its footprint ENDS in (the ownership emitGrayBlock's dst ranges already
// encode), so the blocks its footprint starts in hand over what they saw of it:
//   - rowSum/rowWeight: the pixel straddling the previous block's right edge, one per dst row
//     of the current MCU row. Blocks of an MCU row arrive left to right, so one column at a time.
//   - pending[]: the dst row straddling an MCU-row boundary, per column, as (mean, weight >> 8).
//     Two slots, because an MCU row consumes the row its predecessor left pending while it
//     fills its own.
// Without the handover the straddling pixel only averaged its own block's part: a 1-px line
// in the last column or row of an MCU was mostly dropped, i.e. every 8th source line could
// vanish, which is the nearest-neighbour defect again at a coarser pitch.
struct AreaCarry {
  static constexpr int MAX_ROWS = 20;  // dst rows per MCU row: <= 16 * fine + 1 with fine <= 1
  uint32_t rowSum[MAX_ROWS]{};
  uint32_t rowWeight[MAX_ROWS]{};
  int rowCol{-1};  // dst column the row carry refers to; -1 = none

  struct PendingRow {
    std::unique_ptr<uint16_t[]> weight;
    std::unique_ptr<uint8_t[]> mean;
    int row{-1};
  };
  PendingRow pending[2];
  int width{0};

  bool allocate(const int dstWidth) {
    width = dstWidth;
    for (PendingRow& slot : pending) {
      slot.weight = makeUniqueNoThrow<uint16_t[]>(static_cast<size_t>(dstWidth));
      slot.mean = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(dstWidth));
      if (!slot.weight || !slot.mean) return false;
      slot.row = -1;
    }
    return true;
  }

  PendingRow* slotFor(const int row) {
    for (PendingRow& slot : pending) {
      if (slot.row == row) return &slot;
    }
    return nullptr;
  }

  // Start collecting `row`, reusing whichever slot is not the one `consuming` still reads.
  void open(const int row, const int consuming) {
    if (slotFor(row)) return;
    PendingRow& slot = (pending[0].row != consuming) ? pending[0] : pending[1];
    memset(slot.weight.get(), 0, static_cast<size_t>(width) * sizeof(uint16_t));
    memset(slot.mean.get(), 0, static_cast<size_t>(width));
    slot.row = row;
  }

  // The mean comes from the exact sum/weight ratio before the weight is narrowed: dividing
  // sum >> 8 by weight >> 8 instead overshoots 255 on near-white pixels whenever the shift
  // drops weight bits, and the uint8 wrap turned white margins into a dotted grid.
  static void add(PendingRow& slot, const int col, const uint32_t sum, const uint32_t weight) {
    const uint32_t w = weight >> 8;
    if (w == 0) return;
    const uint32_t mean = (sum + weight / 2) / weight;
    const uint32_t oldW = slot.weight[col];
    const uint32_t newW = oldW + w;
    slot.mean[col] = static_cast<uint8_t>((slot.mean[col] * oldW + mean * w + newW / 2) / newW);
    slot.weight[col] = static_cast<uint16_t>(newW);
  }
};

// Context struct passed through the TJpgDec callbacks (via the TJpgSession in
// jd->device) to avoid global mutable state. The output callback uses it to resample
// and dither each block; the input callback uses the session's FsFile* to read.
struct JpegContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};
  ImageDitherMode effectiveDitherMode{ImageDitherMode::Bayer};

  // Source dimensions after the built-in DCT scaling
  int scaledSrcWidth{0};
  int scaledSrcHeight{0};

  // Final output dimensions
  int dstWidth{0};
  int dstHeight{0};

  // Fine scale in 16.16 fixed-point (ESP32-C3 has no FPU).
  // X and Y use separate scale factors because dstWidth/dstHeight may differ from
  // scaledSrcWidth/scaledSrcHeight in aspect ratio (integer rounding of displayHeight),
  // so a single X-derived factor would map Y rows incorrectly and crop visible content.
  int32_t fineScaleFPX{1 << 16};  // src -> dst mapping (X axis)
  int32_t invScaleFPX{1 << 16};   // dst -> src mapping (X axis)
  int32_t fineScaleFPY{1 << 16};  // src -> dst mapping (Y axis)
  int32_t invScaleFPY{1 << 16};   // dst -> src mapping (Y axis)

  PixelCache cache;
  bool caching{false};

  // The other dither variant of the same decode (RenderConfig::companionCachePath), cache only.
  // The reader wants both .pxc per image; without this each one cost a full decode -- 2.35 s
  // apiece for a progressive diagram on the X3. Written from flushDitherBand, the one place
  // pixels pass in raster order, so it exists only when the primary uses the dither band (the
  // reader's BW-primary decode). Otherwise the caller's second decode covers it, as before.
  struct Companion {
    std::unique_ptr<Atkinson1BitDitherer> atkinson1Bit;  // null => stateless 4-level Bayer
    PixelCache cache;
    int row{-1};
  };
  std::unique_ptr<Companion> companion;

  // Downscale by coverage-weighted averaging (see emitGrayBlock). Decided once per decode;
  // areaCarry is null when its ~3 bytes per column could not be allocated, in which case
  // each block averages only the part of a straddling pixel it holds.
  bool areaAverage{false};
  std::unique_ptr<AreaCarry> areaCarry;

  // See PngContext for the rationale: monochromeOutput requests a 1-bit Atkinson dither
  // emitting only 0/3 so the BW DirectPixelWriter (`pixelValue < 3` rule) maps cleanly.
  int oneBitDitherRow{-1};
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;

  // TJpgDec delivers pixels one MCU block at a time, left-to-right across a row of
  // MCUs before advancing (see tjpgd.c jd_decomp: outer loop over MCU rows, inner
  // loop over MCU columns). The error-diffusion ditherers below need true
  // left-to-right, top-to-bottom pixel order across a FULL row before advancing —
  // otherwise their carried diffusion error resets every MCU-column boundary,
  // producing a visible grid of seams (observed on-device, X4 cover images).
  // When a stateful ditherer is active, incoming blocks are buffered here (in
  // destination pixel coordinates) for one MCU row at a time; ditherGray() only
  // runs once the whole row band is complete, via flushDitherBand(). Non-null
  // implies banding is active; sized dstWidth * ditherBandCapacityRows.
  std::unique_ptr<uint8_t[]> ditherBand;
  int ditherBandCapacityRows{0};
  int ditherBandTop{-1};      // dst Y of band row 0; -1 = no band currently open
  int ditherBandUsedRows{0};  // rows actually spanned by the open band (<= capacity)
  int ditherBandXStart{0};    // union of visible dst-X ranges written into the band
  int ditherBandXEnd{0};

#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  int currentDitherRow{-1};
  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<DiffusedBayerDitherer> diffusedBayerDitherer;
#endif
};

// Advance the 1-bit Atkinson ditherer to the requested destination row.
// Handles non-monotonic row walks (block-based JPEG decode) by reset+replay.
void prepareOneBitDitherRow(JpegContext& ctx, int dstY) {
  if (!ctx.atkinson1BitDitherer) return;

  if (ctx.oneBitDitherRow == -1 || dstY < ctx.oneBitDitherRow) {
    ctx.atkinson1BitDitherer->reset();
    ctx.oneBitDitherRow = dstY;
    return;
  }

  while (ctx.oneBitDitherRow < dstY) {
    ctx.atkinson1BitDitherer->nextRow();
    ctx.oneBitDitherRow++;
  }
}

#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
void prepareDitherRow(JpegContext& ctx, int dstY) {
  if (!ctx.config || !ctx.config->useDithering) return;

  if (ctx.currentDitherRow == -1 || dstY < ctx.currentDitherRow) {
    if (ctx.atkinsonDitherer) ctx.atkinsonDitherer->reset();
    if (ctx.diffusedBayerDitherer) ctx.diffusedBayerDitherer->reset();
    ctx.currentDitherRow = dstY;
    return;
  }

  while (ctx.currentDitherRow < dstY) {
    if (ctx.atkinsonDitherer) ctx.atkinsonDitherer->nextRow();
    if (ctx.diffusedBayerDitherer) ctx.diffusedBayerDitherer->nextRow();
    ctx.currentDitherRow++;
  }
}

uint8_t ditherGray(JpegContext& ctx, uint8_t gray, int localX, int outX, int outY) {
  // Level-correct before dithering, so the ditherer sees the stretched range.
  // Identity when the caller supplied no points. Applied here rather than at the
  // sample sites because both the banded and non-banded emit paths funnel through
  // this function — see flushDitherBand and emitGrayBlock's sinkPixel.
  if (ctx.config) gray = adaptive_tone::apply(ctx.config->adaptiveTone, gray);
  if (ctx.atkinson1BitDitherer) {
    return ctx.atkinson1BitDitherer->processPixel(gray, localX) ? 3 : 0;
  }

  if (!ctx.config || !ctx.config->useDithering) {
    return quantizeGray4Level(gray);
  }

  switch (ctx.effectiveDitherMode) {
    case ImageDitherMode::Atkinson:
      if (ctx.atkinsonDitherer) {
        return ctx.atkinsonDitherer->processPixel(gray, localX);
      }
      break;
    case ImageDitherMode::DiffusedBayer:
      if (ctx.diffusedBayerDitherer) {
        return ctx.diffusedBayerDitherer->processPixel(gray, localX, outX, outY);
      }
      break;
    case ImageDitherMode::Bayer:
    case ImageDitherMode::COUNT:
    default:
      break;
  }

  return applyBayerDither4Level(gray, outX, outY);
}
#else
uint8_t ditherGray(JpegContext& ctx, uint8_t gray, int localX, int outX, int outY) {
  // See the extension-enabled variant above: one tone-mapping seam for both emit paths.
  if (ctx.config) gray = adaptive_tone::apply(ctx.config->adaptiveTone, gray);
  if (ctx.atkinson1BitDitherer) {
    return ctx.atkinson1BitDitherer->processPixel(gray, localX) ? 3 : 0;
  }
  (void)localX;
  return applyBayerDither4Level(gray, outX, outY);
}
#endif

// Dither, write and (if enabled) cache one complete MCU row band: ctx.ditherBand rows
// [0, ditherBandUsedRows) x dst columns [ditherBandXStart, ditherBandXEnd). Called once
// per MCU row, after the last block in that row has been buffered — see the banding
// rationale on JpegContext::ditherBand. Runs the row-based ditherers in true
// left-to-right, top-to-bottom order, which is what they require.
void flushDitherBand(JpegContext& ctx) {
  const int bandTop = ctx.ditherBandTop;
  ctx.ditherBandTop = -1;  // band is consumed either way
  if (bandTop < 0) return;

  const int bandRows = ctx.ditherBandUsedRows;
  const int xStart = ctx.ditherBandXStart;
  const int xEnd = ctx.ditherBandXEnd;
  if (bandRows <= 0 || xStart >= xEnd) return;

  const int cfgX = ctx.config->x;
  const int cfgY = ctx.config->y;

  DirectPixelWriter pw;
  pw.init(*ctx.renderer);

  bool caching = ctx.caching;
  DirectCacheWriter cw;
  if (caching) {
    if (!ctx.cache.advanceTo(bandTop)) {
      caching = false;
      ctx.caching = false;
    } else {
      cw.init(ctx.cache.buffer, ctx.cache.bytesPerRow, ctx.cache.originX, ctx.config->y + ctx.cache.bandStart,
              ctx.cache.width, ctx.cache.bandRows);
    }
  }

  JpegContext::Companion* companion = ctx.companion.get();
  DirectCacheWriter companionWriter;
  if (companion) {
    if (!companion->cache.advanceTo(bandTop)) {
      ctx.companion.reset();  // a failed flush drops the partial file; the caller decodes again
      companion = nullptr;
    } else {
      companionWriter.init(companion->cache.buffer, companion->cache.bytesPerRow, companion->cache.originX,
                           cfgY + companion->cache.bandStart, companion->cache.width, companion->cache.bandRows);
    }
  }

  for (int dstY = bandTop; dstY < bandTop + bandRows; dstY++) {
    const int outY = cfgY + dstY;
    prepareOneBitDitherRow(ctx, dstY);
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
    prepareDitherRow(ctx, dstY);
#endif
    pw.beginRow(outY);
    if (caching) cw.beginRow(outY);
    if (companion) {
      if (companion->atkinson1Bit && companion->row >= 0) companion->atkinson1Bit->nextRow();
      companion->row = dstY;
      companionWriter.beginRow(outY);
    }
    const uint8_t* rowBuf = &ctx.ditherBand[static_cast<size_t>(dstY - bandTop) * static_cast<size_t>(ctx.dstWidth)];
    for (int dstX = xStart; dstX < xEnd; dstX++) {
      const int outX = cfgX + dstX;
      uint8_t dithered = ditherGray(ctx, rowBuf[dstX], dstX, outX, outY);
      pw.writePixel(outX, dithered);
      if (caching) cw.writePixel(outX, dithered);
      if (companion) {
        // Same tone step ditherGray applies, so both variants dither one grey stream.
        const uint8_t gray = adaptive_tone::apply(ctx.config->adaptiveTone, rowBuf[dstX]);
        const uint8_t value = companion->atkinson1Bit ? (companion->atkinson1Bit->processPixel(gray, dstX) ? 3 : 0)
                                                      : applyBayerDither4Level(gray, outX, outY);
        companionWriter.writePixel(outX, value);
      }
    }
  }
}

// TJpgDec session passed through jd->device to the I/O and output callbacks, giving
// access to the open source file and the shared decode context (no global state).
struct TJpgSession {
  FsFile* file{nullptr};
  JpegContext* ctx{nullptr};
};

// TJpgDec stream input. When buff is non-null, read ndata bytes into it; when null,
// remove (skip) ndata bytes from the stream. Returns the number of bytes consumed.
size_t tjpgInput(JDEC* jd, uint8_t* buff, size_t ndata) {
  FsFile* f = static_cast<TJpgSession*>(jd->device)->file;
  if (!f) return 0;
  if (buff) {
    const int n = f->read(buff, ndata);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
  if (!f->seek(f->position() + static_cast<uint32_t>(ndata))) return 0;
  return ndata;
}

// TJpgDec work area. With JD_FASTDECODE=2 the huffman LUTs (~6 KB for a colour JPEG)
// are carved from this pool on top of the ~3 KB base tables/buffers; 12 KB leaves
// headroom and still beats JPEGDEC's ~20 KB struct. (jd_prepare returns JDR_MEM1 and
// we fail gracefully if an unusual image needs more.)
constexpr size_t TJPG_WORK_POOL_SIZE = 12 * 1024;

// A decoder's working block (TJpgDec's work pool, or the progressive decoder's workspace), drawn
// from the pass scratch arena when one is installed (see image_scratch) and otherwise from the
// heap. At 12-20 KB it is the largest per-decode block on the JPEG path, and a warm pass
// allocates it once per decode AND once per tone analysis — the churn rule 4 in
// docs/memory-allocation-strategy.md exists to stop.
//
// RAII so every early return below releases the arena scope; the arena release is newest-first,
// which holds because the pool is the only thing this scope allocates. `heapFloor`: free heap
// that must remain after a heap fallback, or none is attempted (the arena is always tried).
class JpegWorkPool {
 public:
  explicit JpegWorkPool(const size_t size, const size_t heapFloor = 0) {
    if (BuildArena* arena = image_scratch::get(); arena && arena->valid()) {
      block_ = arena->reserveBlock();
      if (block_.valid()) {
        if (auto* p = static_cast<uint8_t*>(arena->alloc(size))) {
          ptr_ = p;
          arena_ = arena;
          return;
        }
        arena->release(block_);  // too small for this pass — fall back to the heap
      }
    }
    if (heapFloor > 0 && ESP.getFreeHeap() < size + heapFloor) return;
    heap_ = makeUniqueNoThrow<uint8_t[]>(size);
    ptr_ = heap_.get();
  }
  ~JpegWorkPool() {
    heap_.reset();
    if (block_.valid()) arena_->release(block_);
  }
  JpegWorkPool(const JpegWorkPool&) = delete;
  JpegWorkPool& operator=(const JpegWorkPool&) = delete;

  uint8_t* get() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }

 private:
  uint8_t* ptr_ = nullptr;
  std::unique_ptr<uint8_t[]> heap_;
  BuildArena* arena_ = nullptr;
  BuildArena::Block block_;
};
// Minimum free heap to attempt a decode: the work pool plus headroom for the
// streaming cache band and the ditherer rows allocated further below.
constexpr size_t MIN_FREE_HEAP_FOR_JPEG = TJPG_WORK_POOL_SIZE + 16 * 1024;
constexpr size_t MIN_FREE_HEAP_FOR_PROGRESSIVE_JPEG = 16 * 1024;

// The work-pool term of the floor above is only a HEAP cost when no pass arena is installed --
// JpegWorkPool draws from image_scratch first. Charging it either way makes the floor demand
// 12 KB of heap for a block the heap never sees, which is what refuses decodes under a borrowed
// framebuffer (free heap is ~52 KB lower there than on the released path, but the arena is
// serving the largest block). See docs/memory-allocation-strategy.md §9.3.
size_t minFreeHeapForJpeg() {
  return image_scratch::canServe(TJPG_WORK_POOL_SIZE) ? MIN_FREE_HEAP_FOR_JPEG - TJPG_WORK_POOL_SIZE
                                                      : MIN_FREE_HEAP_FOR_JPEG;
}

// Optional memory-behavior knobs for embedded targets.
#ifndef JPEG_ENABLE_FIRST_RENDER_NO_CACHE
#define JPEG_ENABLE_FIRST_RENDER_NO_CACHE 1
#endif

// Heap the decode still takes AFTER the cache gate, cache or not: the ditherer band (<= 8 KB, one
// row for progressive) and error rows (~6 B per pixel column), and for progressive the decoder's
// tables and row buffers (~6 KB). The work pool / probe state are allocated BEFORE the gate, so
// the free-heap figure there already reflects them.
#ifndef JPEG_CACHE_POST_GATE_BYTES
#define JPEG_CACHE_POST_GATE_BYTES (12 * 1024)
#endif
// What must remain for everything else once the decode's whole working set is taken.
#ifndef JPEG_CACHE_HEAP_FLOOR
#define JPEG_CACHE_HEAP_FLOOR (8 * 1024)
#endif

// Heap that must remain when the progressive workspace cannot come from the scratch arena: the
// rest of the decode's working set (JPEG_CACHE_POST_GATE_BYTES) plus the floor for everything
// else. Below it the workspace steps down a scale, and finally the DC-only preview runs.
constexpr size_t PROGRESSIVE_WORKSPACE_HEAP_FLOOR = JPEG_CACHE_POST_GATE_BYTES + JPEG_CACHE_HEAP_FLOOR;

// Arena-aware (via minFreeHeapForJpeg): otherwise a borrowed-framebuffer decode
// silently drops from Atkinson to Bayer dithering on every image — a visible quality regression
// caused by charging heap for a block the arena is serving.
#ifndef JPEG_DITHER_LOW_MEM_MIN_FREE_HEAP
#define JPEG_DITHER_LOW_MEM_MIN_FREE_HEAP (minFreeHeapForJpeg() + 8 * 1024)
#endif

// Whether to stream this decode into its .pxc. Charged with what caching actually adds -- one
// streaming band (PixelCache::bandBytesFor: 2-4 KB for a typical page image) -- on top of the
// decode's remaining working set and a floor.
//
// It used to demand minFreeHeapForJpeg() + 24 KB: a margin sized in 603005c3a for the old
// full-image cache buffer, never revisited when the streaming band replaced it, and charging the
// decoder floor again for a pool that is already allocated here (and that progressive never
// takes). With both framebuffers resident a reading page has ~29-38 KB free against the 41-53 KB
// that asked for, so no JPEG was ever cached: every visit re-decoded, and a large image that
// Confirm had loaded fell back to its placeholder on the next visit (X3 2026-09-25, "Skipping
// cache: free heap 29060 < 40960" on every decode).
bool shouldEnableJpegCache(const RenderConfig& config, const int width, const int height, const int maxBlockDstRows) {
  if (config.cachePath.empty()) return false;

#if JPEG_ENABLE_FIRST_RENDER_NO_CACHE
  if (!Storage.exists(config.cachePath.c_str())) {
    LOG_TRC("JPG", "No existing JPEG cache file on first render; enabling cache write: %s", config.cachePath.c_str());
  }
#endif

  const size_t bandBytes = PixelCache::bandBytesFor(width, height, maxBlockDstRows);
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t minFreeForCaching = bandBytes + JPEG_CACHE_POST_GATE_BYTES + JPEG_CACHE_HEAP_FLOOR;

  if (freeHeap < minFreeForCaching) {
    LOG_DBG("JPG", "Skipping cache: free heap %u < %u (band %u bytes)", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(minFreeForCaching), static_cast<unsigned>(bandBytes));
    return false;
  }

  // Don't pre-check maxAlloc: let PixelCache::begin() attempt its malloc and fall back to the
  // no-cache path on failure.
  return true;
}

bool shouldForceBayerDither(const RenderConfig& config) {
  if (!config.useDithering) return false;
  if (config.ditherMode == ImageDitherMode::Bayer) return false;

  // Use getFreeHeap() only — getMaxAllocHeap() walks the TLSF free-block chain
  // and crashes if the heap is corrupt (possible after failed image decodes under
  // pressure). The free-heap threshold is conservative enough as a sole guard here.
  const size_t freeHeap = ESP.getFreeHeap();
  return freeHeap < JPEG_DITHER_LOW_MEM_MIN_FREE_HEAP;
}

// Classify a SOF marker byte into the coding mode used for engine selection.
// Only SOF0 is true baseline (the sole mode TJpgDec accepts); SOF2 is progressive;
// everything else (extended-sequential SOF1, arithmetic, lossless, …) is Other.
// Only Baseline can be decoded; the rest fall back to a placeholder.
JpegToFramebufferConverter::JpegMode classifyJpegMode(uint8_t sofMarker) {
  using JpegMode = JpegToFramebufferConverter::JpegMode;
  if (sofMarker == 0xC0) return JpegMode::Baseline;
  if (sofMarker == 0xC2) return JpegMode::Progressive;
  return JpegMode::Other;
}

bool readJpegDimensionsFromHeader(const std::string& imagePath, ImageDimensions& out,
                                  JpegToFramebufferConverter::JpegMode* outMode = nullptr) {
  FsFile f;
  if (!Storage.openFileForRead("JPG", imagePath, f)) {
    LOG_ERR("JPG", "Failed to open file for dimensions: %s", imagePath.c_str());
    return false;
  }

  auto readByte = [&f](uint8_t& b) -> bool { return f.read(&b, 1) == 1; };
  auto readU16BE = [&f](uint16_t& v) -> bool {
    uint8_t b[2];
    if (f.read(b, 2) != 2) return false;
    v = static_cast<uint16_t>((static_cast<uint16_t>(b[0]) << 8) | b[1]);
    return true;
  };

  uint8_t b0 = 0;
  uint8_t b1 = 0;
  if (!readByte(b0) || !readByte(b1) || b0 != 0xFF || b1 != 0xD8) {
    f.close();
    LOG_ERR("JPG", "Not a JPEG file: %s", imagePath.c_str());
    return false;
  }

  while (f.available()) {
    uint8_t prefix = 0;
    if (!readByte(prefix)) break;
    if (prefix != 0xFF) continue;

    uint8_t marker = 0;
    do {
      if (!readByte(marker)) {
        f.close();
        return false;
      }
    } while (marker == 0xFF);

    if (marker == 0x00 || marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) {
      continue;
    }

    uint16_t segLen = 0;
    if (!readU16BE(segLen) || segLen < 2) {
      f.close();
      return false;
    }

    const bool isSof = (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
                       (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
    if (isSof) {
      uint8_t sof[5];
      if (segLen < 7 || f.read(sof, sizeof(sof)) != static_cast<int>(sizeof(sof))) {
        f.close();
        return false;
      }
      uint16_t height = static_cast<uint16_t>((static_cast<uint16_t>(sof[1]) << 8) | sof[2]);
      uint16_t width = static_cast<uint16_t>((static_cast<uint16_t>(sof[3]) << 8) | sof[4]);
      f.close();
      if (width == 0 || height == 0) {
        LOG_ERR("JPG", "Invalid JPEG dimensions %ux%u: %s", width, height, imagePath.c_str());
        return false;
      }

      if (width > static_cast<uint16_t>(std::numeric_limits<int16_t>::max()) ||
          height > static_cast<uint16_t>(std::numeric_limits<int16_t>::max())) {
        LOG_ERR("JPG", "JPEG dimensions out of supported range %ux%u: %s", width, height, imagePath.c_str());
        return false;
      }

      out.width = static_cast<int16_t>(width);
      out.height = static_cast<int16_t>(height);
      if (outMode) *outMode = classifyJpegMode(marker);
      return true;
    }

    const int32_t skip = static_cast<int32_t>(segLen) - 2;
    if (!f.seek(f.position() + skip)) {
      f.close();
      return false;
    }
  }

  f.close();
  LOG_ERR("JPG", "No SOF marker found for dimensions: %s", imagePath.c_str());
  return false;
}

// Choose the coarse DCT downscale factor. Returns the scale denominator (1, 2, 4 or 8)
// and sets tjpgScale to the matching TJpgDec scale exponent (0=1/1, 1=1/2, 2=1/4, 3=1/8).
int chooseJpegScale(float targetScale, uint8_t& tjpgScale) {
  if (targetScale <= 0.125f) {
    tjpgScale = 3;
    return 8;
  }
  if (targetScale <= 0.25f) {
    tjpgScale = 2;
    return 4;
  }
  if (targetScale <= 0.5f) {
    tjpgScale = 1;
    return 2;
  }
  tjpgScale = 0;
  return 1;
}

// Fixed-point 16.16 arithmetic avoids software float emulation on ESP32-C3 (no FPU).
constexpr int FP_SHIFT = 16;
constexpr int32_t FP_ONE = 1 << FP_SHIFT;
constexpr int32_t FP_MASK = FP_ONE - 1;

// Largest dst->src ratio (per axis, after the DCT step) the area average handles. Weights are
// 8.8 per axis, so a pixel's total weight is invX * invY * 2^16 and its sum at most 255 times
// that: under 12 per axis keeps both within uint32 and the carried weight (>> 8) within
// uint16. A residual scale below 1/12 is a thumbnail, and keeps nearest-neighbour.
constexpr int32_t MAX_AREA_INV_FP = 12 << FP_SHIFT;

// One decoded block, and where its pixels land in the destination. [xs, xe) x [ys, ye) are the
// dst pixels whose footprint ENDS in this block (unclipped); column xe and row ye start in it
// and end in a later block.
struct AreaBlock {
  const uint8_t* pixels;
  int stride;
  int32_t leftFP, rightFP, topFP, bottomFP;  // block extent in 16.16 source coordinates
  int xs, xe, ys, ye;
};

// Coverage-weighted sum of the block's pixels under dst pixel (d, r)'s footprint, clamped to
// the block. Adds to sum/weight (weights 8.8 per axis).
void areaContribution(const JpegContext& ctx, const AreaBlock& b, const int d, const int r, uint32_t& sum,
                      uint32_t& weight) {
  const int32_t x0 = std::max(d * ctx.invScaleFPX, b.leftFP);
  const int32_t x1 = std::min((d + 1) * ctx.invScaleFPX, b.rightFP);
  const int32_t y0 = std::max(r * ctx.invScaleFPY, b.topFP);
  const int32_t y1 = std::min((r + 1) * ctx.invScaleFPY, b.bottomFP);
  if (x1 <= x0 || y1 <= y0) return;
  const int blockX = b.leftFP >> FP_SHIFT;
  const int blockY = b.topFP >> FP_SHIFT;
  for (int32_t iy = y0 >> FP_SHIFT; (iy << FP_SHIFT) < y1; iy++) {
    const uint32_t wy = static_cast<uint32_t>(std::min((iy + 1) << FP_SHIFT, y1) - std::max(iy << FP_SHIFT, y0)) >> 8;
    const uint8_t* srcRow = &b.pixels[(iy - blockY) * b.stride];
    for (int32_t ix = x0 >> FP_SHIFT; (ix << FP_SHIFT) < x1; ix++) {
      const uint32_t wx = static_cast<uint32_t>(std::min((ix + 1) << FP_SHIFT, x1) - std::max(ix << FP_SHIFT, x0)) >> 8;
      const uint32_t w = wx * wy;
      sum += w * srcRow[ix - blockX];
      weight += w;
    }
  }
}

// First block of an MCU row: forget the previous row's column carry and open the slot this
// MCU row's pending dst row collects into.
void areaBeginBlock(JpegContext& ctx, const AreaBlock& b, const bool rowStart) {
  AreaCarry* carry = ctx.areaCarry.get();
  if (!carry || !rowStart) return;
  carry->rowCol = -1;
  if (b.ye < ctx.dstHeight) carry->open(b.ye, b.ys);
}

// Hand this block's share of the pixels it does not own to the blocks that will: column xe for
// the owned rows (the next block in this MCU row), row ye for every column it touches (the
// next MCU row). Runs after the owned pixels were emitted, since those consumed the old carry.
void areaEndBlock(JpegContext& ctx, const AreaBlock& b) {
  AreaCarry* carry = ctx.areaCarry.get();
  if (!carry) return;

  if (b.xe < ctx.dstWidth) {
    // A block that owns no column passes the carry it received on, plus its own share.
    const bool continues = b.xs == b.xe && carry->rowCol == b.xs;
    for (int r = std::max(b.ys, 0); r < std::min(b.ye, ctx.dstHeight); r++) {
      const int i = r - b.ys;
      if (i >= AreaCarry::MAX_ROWS) break;
      uint32_t sum = continues ? carry->rowSum[i] : 0;
      uint32_t weight = continues ? carry->rowWeight[i] : 0;
      areaContribution(ctx, b, b.xe, r, sum, weight);
      carry->rowSum[i] = sum;
      carry->rowWeight[i] = weight;
    }
    carry->rowCol = b.xe;
  } else {
    carry->rowCol = -1;
  }

  if (b.ye < ctx.dstHeight) {
    AreaCarry::PendingRow* slot = carry->slotFor(b.ye);
    if (!slot) return;
    for (int d = std::max(b.xs, 0); d <= std::min(b.xe, ctx.dstWidth - 1); d++) {
      uint32_t sum = 0;
      uint32_t weight = 0;
      areaContribution(ctx, b, d, b.ye, sum, weight);
      AreaCarry::add(*slot, d, sum, weight);
    }
  }
}

// Emit one decoded grayscale block into the framebuffer (and the streaming cache),
// applying fine resampling and the active ditherer. Engine-neutral: it takes a raw
// 8-bit grayscale block — densely packed at `stride`, with `validW` valid columns and
// `blockH` rows — at its scaled-source-space origin (blockX, blockY). Consumed by the
// TJpgDec output callback below. Returns 1 to continue decoding (0 would abort),
// matching the engine's callback contract.
int emitGrayBlock(JpegContext& ctxRef, const uint8_t* pixels, int blockX, int blockY, int validW, int blockH,
                  int stride) {
  JpegContext* ctx = &ctxRef;

  // Feed the interrupt WDT every block — large JPEGs can take many seconds.
  HalSystem::feedWatchdog();

  if (stride <= 0 || blockH <= 0 || validW <= 0) return 1;

  const int32_t fineScaleFPX = ctx->fineScaleFPX;
  const int32_t invScaleFPX = ctx->invScaleFPX;
  const int32_t fineScaleFPY = ctx->fineScaleFPY;
  const int32_t invScaleFPY = ctx->invScaleFPY;
  GfxRenderer& renderer = *ctx->renderer;
  const int cfgX = ctx->config->x;
  const int cfgY = ctx->config->y;

  // Determine destination pixel range covered by this source block
  const int srcYEnd = blockY + blockH;
  const int srcXEnd = blockX + validW;

  int dstYStart = (int)((int64_t)blockY * fineScaleFPY >> FP_SHIFT);
  int dstYEnd = (srcYEnd >= ctx->scaledSrcHeight) ? ctx->dstHeight : (int)((int64_t)srcYEnd * fineScaleFPY >> FP_SHIFT);
  int dstXStart = (int)((int64_t)blockX * fineScaleFPX >> FP_SHIFT);
  int dstXEnd = (srcXEnd >= ctx->scaledSrcWidth) ? ctx->dstWidth : (int)((int64_t)srcXEnd * fineScaleFPX >> FP_SHIFT);

  // Area-average bookkeeping works on the unclipped ranges: a pixel off screen still has to
  // pass its share on, or the visible pixel next to it would inherit a stale carry.
  const AreaBlock area{pixels,
                       stride,
                       blockX << FP_SHIFT,
                       srcXEnd << FP_SHIFT,
                       blockY << FP_SHIFT,
                       srcYEnd << FP_SHIFT,
                       dstXStart,
                       dstXEnd,
                       dstYStart,
                       dstYEnd};
  areaBeginBlock(*ctx, area, blockX == 0);

  // Pre-clamp destination ranges to screen bounds (eliminates per-pixel screen checks)
  int clampYMax = ctx->dstHeight;
  if (ctx->screenHeight - cfgY < clampYMax) clampYMax = ctx->screenHeight - cfgY;
  if (dstYStart < -cfgY) dstYStart = -cfgY;
  if (dstYEnd > clampYMax) dstYEnd = clampYMax;

  int clampXMax = ctx->dstWidth;
  if (ctx->screenWidth - cfgX < clampXMax) clampXMax = ctx->screenWidth - cfgX;
  if (dstXStart < -cfgX) dstXStart = -cfgX;
  if (dstXEnd > clampXMax) dstXEnd = clampXMax;

  if (dstYStart >= dstYEnd) {
    areaEndBlock(*ctx, area);
    return 1;
  }

  // Row-band bookkeeping for the stateful ditherers (see JpegContext::ditherBand).
  // blockX == 0 marks the first MCU of this row (open a fresh band); srcXEnd
  // reaching the scaled source width marks the last MCU (the band is complete and
  // must be dithered+emitted before returning). Both can fire on the same call for
  // narrow images. This must run even when this particular block is entirely
  // clipped in X below, since it may still be the block that opens or closes the row.
  const bool useBand = (ctx->ditherBand != nullptr);
  const bool rowStart = (blockX == 0);
  const bool rowEnd = (srcXEnd >= ctx->scaledSrcWidth);

  if (useBand && rowStart) {
    ctx->ditherBandTop = dstYStart;
    ctx->ditherBandUsedRows = dstYEnd - dstYStart;
    if (ctx->ditherBandUsedRows > ctx->ditherBandCapacityRows) ctx->ditherBandUsedRows = ctx->ditherBandCapacityRows;
    ctx->ditherBandXStart = ctx->dstWidth;
    ctx->ditherBandXEnd = 0;
  }

  if (dstXStart >= dstXEnd) {
    areaEndBlock(*ctx, area);
    if (useBand && rowEnd) flushDitherBand(*ctx);
    return 1;
  }

  if (useBand) {
    if (dstXStart < ctx->ditherBandXStart) ctx->ditherBandXStart = dstXStart;
    if (dstXEnd > ctx->ditherBandXEnd) ctx->ditherBandXEnd = dstXEnd;
  }

  // Pre-compute orientation and render-mode state once per callback invocation.
  // Unused (but harmless to init) when useBand: this block's pixels are buffered
  // into ctx->ditherBand instead, and flushDitherBand() does the actual writing
  // once the whole MCU row band is complete.
  DirectPixelWriter pw;
  pw.init(renderer);

  // The cache streams to disk one MCU-row band at a time. Flushing rows below
  // this block (raster order guarantees they are final) repositions the band;
  // the band-relative origin/extent passed to init() then map screen rows to
  // the small streaming buffer rather than a full-image one. If a flush write
  // fails, stop caching for the rest of this decode (and let finalize() drop
  // the partial file) rather than writing past the band buffer.
  //
  // Ported from upstream commit d9bcef7a (crosspoint-reader#2230).
  // Skipped entirely when useBand: flushDitherBand() drives the cache once per
  // complete row band instead of once per (partial-width) MCU block.
  bool caching = !useBand && ctx->caching;
  DirectCacheWriter cw;
  if (caching) {
    if (!ctx->cache.advanceTo(dstYStart)) {
      caching = false;
      ctx->caching = false;
    } else {
      cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.originX, ctx->config->y + ctx->cache.bandStart,
              ctx->cache.width, ctx->cache.bandRows);
    }
  }

  // Sink one already-resampled grayscale pixel: buffer it for later banded
  // dithering, or dither+write it immediately (the pre-existing behavior, still
  // used for the stateless Bayer path where pixel order doesn't matter).
  auto sinkPixel = [&](int dstX, int dstY, int outX, int outY, uint8_t gray) {
    if (useBand) {
      // ditherBandCapacityRows is sized from the decoder's real block height (see
      // maxBlockDstRows, shared with PixelCache); this guard is a defensive backstop
      // against that ever being violated, trading a dropped row for a heap overflow.
      const int bandRow = dstY - ctx->ditherBandTop;
      if (bandRow < 0 || bandRow >= ctx->ditherBandCapacityRows) return;
      ctx->ditherBand[static_cast<size_t>(bandRow) * static_cast<size_t>(ctx->dstWidth) + static_cast<size_t>(dstX)] =
          gray;
      return;
    }
    uint8_t dithered = ditherGray(*ctx, gray, dstX, outX, outY);
    pw.writePixel(outX, dithered);
    if (caching) cw.writePixel(outX, dithered);
  };

  // === 1:1 fast path: no scaling math ===
  if (fineScaleFPX == FP_ONE && fineScaleFPY == FP_ONE) {
    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      const int outY = cfgY + dstY;
      if (!useBand) {
        prepareOneBitDitherRow(*ctx, dstY);
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
        prepareDitherRow(*ctx, dstY);
#endif
        pw.beginRow(outY);
        if (caching) cw.beginRow(outY);
      }
      const uint8_t* row = &pixels[(dstY - blockY) * stride];
      for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        uint8_t gray = row[dstX - blockX];
        sinkPixel(dstX, dstY, outX, outY, gray);
      }
    }
    if (useBand && rowEnd) flushDitherBand(*ctx);
    return 1;
  }

  // === Bilinear interpolation (upscale: fineScale > 1.0) ===
  // Smooths block boundaries that would otherwise create visible banding
  // on progressive JPEG DC-only decode (1/8 resolution upscaled to target).
  if (fineScaleFPX > FP_ONE && fineScaleFPY > FP_ONE) {
    // Pre-compute safe X range where lx0 and lx0+1 are both in [0, validW-1].
    // Only the left/right edge pixels (typically 0-2 and 1-8 respectively) need clamping.
    int safeXStart = (int)(((int64_t)blockX * fineScaleFPX + FP_MASK) >> FP_SHIFT);
    int safeXEnd = (int)((int64_t)(blockX + validW - 1) * fineScaleFPX >> FP_SHIFT);
    if (safeXStart < dstXStart) safeXStart = dstXStart;
    if (safeXEnd > dstXEnd) safeXEnd = dstXEnd;
    if (safeXStart > safeXEnd) safeXEnd = safeXStart;

    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      const int outY = cfgY + dstY;
      if (!useBand) {
        prepareOneBitDitherRow(*ctx, dstY);
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
        prepareDitherRow(*ctx, dstY);
#endif
        pw.beginRow(outY);
        if (caching) cw.beginRow(outY);
      }
      const int32_t srcFyFP = dstY * invScaleFPY;
      const int32_t fy = srcFyFP & FP_MASK;
      const int32_t fyInv = FP_ONE - fy;
      int ly0 = (srcFyFP >> FP_SHIFT) - blockY;
      int ly1 = ly0 + 1;
      if (ly0 < 0) ly0 = 0;
      if (ly0 >= blockH) ly0 = blockH - 1;
      if (ly1 >= blockH) ly1 = blockH - 1;

      const uint8_t* row0 = &pixels[ly0 * stride];
      const uint8_t* row1 = &pixels[ly1 * stride];

      // Left edge (with X boundary clamping)
      for (int dstX = dstXStart; dstX < safeXStart; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        int lx0 = (srcFxFP >> FP_SHIFT) - blockX;
        int lx1 = lx0 + 1;
        if (lx0 < 0) lx0 = 0;
        if (lx1 < 0) lx1 = 0;
        if (lx0 >= validW) lx0 = validW - 1;
        if (lx1 >= validW) lx1 = validW - 1;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        sinkPixel(dstX, dstY, outX, outY, gray);
      }

      // Interior (no X boundary checks — lx0 and lx0+1 guaranteed in bounds)
      for (int dstX = safeXStart; dstX < safeXEnd; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        const int lx0 = (srcFxFP >> FP_SHIFT) - blockX;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx0 + 1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx0 + 1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        sinkPixel(dstX, dstY, outX, outY, gray);
      }

      // Right edge (with X boundary clamping)
      for (int dstX = safeXEnd; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        int lx0 = (srcFxFP >> FP_SHIFT) - blockX;
        int lx1 = lx0 + 1;
        if (lx0 >= validW) lx0 = validW - 1;
        if (lx1 >= validW) lx1 = validW - 1;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        sinkPixel(dstX, dstY, outX, outY, gray);
      }
    }
    if (useBand && rowEnd) flushDitherBand(*ctx);
    return 1;
  }

  // === Area average (downscale: fineScale < 1.0) ===
  // Each destination pixel is the coverage-weighted mean of the source pixels under its
  // footprint. Nearest-neighbour took one sample per pixel, so at the typical 0.6-0.9 residual
  // scale left after the DCT's 1/2^n step, every source column or row the sample grid skipped
  // simply vanished: 1-px strokes in diagrams broke up and fine grid lines dropped out.
  // Pixels straddling a block edge collect the earlier blocks' shares through AreaCarry.
  // Nearest-neighbour remains for mixed up/down scales and for residuals below 1/12.
  AreaCarry* carry = ctx->areaCarry.get();
  AreaCarry::PendingRow* above = carry ? carry->slotFor(area.ys) : nullptr;
  const bool takeRowCarry = carry && carry->rowCol == area.xs;

  for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
    const int outY = cfgY + dstY;
    if (!useBand) {
      prepareOneBitDitherRow(*ctx, dstY);
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
      prepareDitherRow(*ctx, dstY);
#endif
      pw.beginRow(outY);
      if (caching) cw.beginRow(outY);
    }
    const int32_t srcFyFP = dstY * invScaleFPY;
    int ly = (srcFyFP >> FP_SHIFT) - blockY;
    if (ly < 0) ly = 0;
    if (ly >= blockH) ly = blockH - 1;
    const uint8_t* row = &pixels[ly * stride];
    const int carryIndex = dstY - area.ys;

    for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
      const int outX = cfgX + dstX;
      const int32_t srcFxFP = dstX * invScaleFPX;
      int lx = (srcFxFP >> FP_SHIFT) - blockX;
      if (lx < 0) lx = 0;
      if (lx >= validW) lx = validW - 1;
      uint8_t gray = row[lx];

      if (ctx->areaAverage) {
        uint32_t sum = 0;
        uint32_t weight = 0;
        areaContribution(*ctx, area, dstX, dstY, sum, weight);
        if (takeRowCarry && dstX == area.xs && carryIndex < AreaCarry::MAX_ROWS) {
          sum += carry->rowSum[carryIndex];
          weight += carry->rowWeight[carryIndex];
        }
        if (above && dstY == area.ys) {
          const uint32_t w = above->weight[dstX];
          sum += (static_cast<uint32_t>(above->mean[dstX]) * w) << 8;
          weight += w << 8;
        }
        if (weight > 0) gray = static_cast<uint8_t>((sum + weight / 2) / weight);
      }

      sinkPixel(dstX, dstY, outX, outY, gray);
    }
  }

  areaEndBlock(*ctx, area);
  if (useBand && rowEnd) flushDitherBand(*ctx);
  return 1;
}

// TJpgDec output callback: forward one decoded grayscale MCU block to emitGrayBlock.
// JRECT is inclusive; with JD_FORMAT=2 the bitmap is 8-bit gray packed tightly at the
// block width, and MCUs arrive in raster top-to-bottom order (PixelCache contract).
int tjpgOutput(JDEC* jd, void* bitmap, JRECT* rect) {
  JpegContext* ctx = static_cast<TJpgSession*>(jd->device)->ctx;
  if (!ctx) return 0;
  const int validW = rect->right - rect->left + 1;
  const int blockH = rect->bottom - rect->top + 1;
  return emitGrayBlock(*ctx, static_cast<const uint8_t*>(bitmap), rect->left, rect->top, validW, blockH, validW);
}

bool progressiveOutput(void* user, uint16_t y, const uint8_t* grayscale, uint16_t width) {
  auto* ctx = static_cast<JpegContext*>(user);
  if (!ctx || width != ctx->dstWidth || y >= ctx->dstHeight) return false;
  return emitGrayBlock(*ctx, grayscale, 0, y, width, 1, width) != 0;
}

// Full progressive decoder: each band is a full-width block at 1/2^s scale, exactly what
// TJpgDec's MCU rows are, so it takes the same resample/dither/cache path.
struct FullProgressiveSink {
  JpegContext* ctx;
  bool emitted;
};

bool fullProgressiveOutput(void* user, uint16_t y, const uint8_t* gray, uint16_t width, uint16_t rows,
                           uint16_t stride) {
  auto* sink = static_cast<FullProgressiveSink*>(user);
  sink->emitted = true;
  return emitGrayBlock(*sink->ctx, gray, 0, y, width, rows, stride) != 0;
}

bool progressiveShouldAbort(void*) {
  if (!CooperativeAbort::shouldAbortLongTask()) return false;
  CooperativeAbort::markAborted();
  return true;
}

// The full progressive decoder reads the whole file before its first band (emitGrayBlock feeds
// the watchdog per band, but not before one exists), so its abort hook feeds it too.
bool fullProgressiveShouldAbort(void* user) {
  HalSystem::feedWatchdog();
  return progressiveShouldAbort(user);
}

// Fine-scale factors for scaledSrc* -> dst*, and whether the downscale area-averages. Area
// averaging covers pure downscales (see emitGrayBlock); the carry that makes it exact across
// block edges costs 2 x 3 bytes per output column. Re-run when a decode falls back to the DC
// preview, which resizes to the destination itself.
void configureResample(JpegContext& ctx, const bool allowAreaAverage) {
  ctx.fineScaleFPX = (int32_t)((int64_t)ctx.dstWidth * FP_ONE / ctx.scaledSrcWidth);
  ctx.invScaleFPX = (int32_t)((int64_t)ctx.scaledSrcWidth * FP_ONE / ctx.dstWidth);
  ctx.fineScaleFPY = (int32_t)((int64_t)ctx.dstHeight * FP_ONE / ctx.scaledSrcHeight);
  ctx.invScaleFPY = (int32_t)((int64_t)ctx.scaledSrcHeight * FP_ONE / ctx.dstHeight);
  ctx.areaAverage = allowAreaAverage && ctx.fineScaleFPX <= FP_ONE && ctx.fineScaleFPY <= FP_ONE &&
                    (ctx.fineScaleFPX < FP_ONE || ctx.fineScaleFPY < FP_ONE) && ctx.invScaleFPX < MAX_AREA_INV_FP &&
                    ctx.invScaleFPY < MAX_AREA_INV_FP;
  ctx.areaCarry.reset();
  if (ctx.areaAverage) {
    ctx.areaCarry = makeUniqueNoThrow<AreaCarry>();
    if (!ctx.areaCarry || !ctx.areaCarry->allocate(ctx.dstWidth)) {
      LOG_DBG("JPG", "No heap for the %d-column area carry; block edges average per block", ctx.dstWidth);
      ctx.areaCarry.reset();
    }
  }
}

// Draw a simple bordered placeholder where an undecodable (non-baseline) JPEG would
// have gone, so the layout shows a framed gap rather than a silent blank.
void drawUnsupportedPlaceholder(GfxRenderer& renderer, const RenderConfig& config) {
  const int w = config.maxWidth;
  const int h = config.maxHeight;
  if (w <= 0 || h <= 0) return;
  if (config.x < 0 || config.y < 0 || config.x + w > renderer.getScreenWidth() ||
      config.y + h > renderer.getScreenHeight()) {
    return;
  }
  constexpr int BORDER = 1;
  renderer.drawRect(config.x, config.y, w, h, BORDER, true);
}

}  // namespace

bool JpegToFramebufferConverter::getDimensionsFromBuffer(const uint8_t* buf, const size_t len, ImageDimensions& out,
                                                         JpegMode* outMode, bool* needMore) {
  if (needMore) *needMore = false;
  if (!buf || len < 4) return false;
  if (buf[0] != 0xFF || buf[1] != 0xD8) return false;

  size_t pos = 2;
  while (pos + 3 < len) {
    if (buf[pos] != 0xFF) {
      pos++;
      continue;
    }
    pos++;
    uint8_t marker = buf[pos++];
    while (marker == 0xFF && pos < len) marker = buf[pos++];
    if (marker == 0x00 || marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) continue;
    if (pos + 1 >= len) break;
    const uint16_t segLen = (static_cast<uint16_t>(buf[pos]) << 8) | buf[pos + 1];
    if (segLen < 2) return false;
    const bool isSof = (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
                       (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
    if (isSof) {
      if (pos + 6 >= len) break;  // the frame header straddles the end of the buffer
      const uint16_t h = (static_cast<uint16_t>(buf[pos + 3]) << 8) | buf[pos + 4];
      const uint16_t w = (static_cast<uint16_t>(buf[pos + 5]) << 8) | buf[pos + 6];
      if (w == 0 || h == 0 || w > 0x7FFF || h > 0x7FFF) return false;
      out.width = static_cast<int16_t>(w);
      out.height = static_cast<int16_t>(h);
      if (outMode) *outMode = classifyJpegMode(marker);
      return true;
    }
    if (marker == 0xDA) return false;  // SOS — entropy data begins, no SOF was found
    pos += segLen;
  }
  // Ran off the end with the marker structure still consistent: the header continues beyond
  // `len` (Photoshop-style Exif/IPTC/XMP/ICC runs to tens of KB) and a longer read finds SOF.
  // Distinct from the failures above, which no amount of extra bytes would fix.
  if (needMore) *needMore = true;
  return false;
}

bool JpegToFramebufferConverter::getDimensionsFromZipEntryStreaming(const std::string& epubPath,
                                                                    const std::string& entryPath, ImageDimensions& out,
                                                                    JpegMode* outMode) {
  ZipFile zip(epubPath);
  ZipFile::EntryReader reader(zip, 512);
  if (!reader.open(entryPath.c_str())) return false;
  return getDimensionsFromEntryReader(reader, out, outMode);
}

bool JpegToFramebufferConverter::getDimensionsFromEntryReader(ZipFile::EntryReader& reader, ImageDimensions& out,
                                                              JpegMode* outMode) {
  if (!reader.isOpen()) return false;

  // Pull decompressed bytes one chunk at a time. Segment bodies are skipped byte-by-byte
  // through this same source, so memory stays bounded no matter how large the metadata is.
  uint8_t chunk[512];
  size_t chunkLen = 0;
  size_t chunkPos = 0;
  bool streamDone = false;
  auto nextByte = [&](uint8_t& b) -> bool {
    while (chunkPos >= chunkLen) {
      if (streamDone) return false;
      size_t produced = 0;
      bool done = false;
      if (!reader.step(chunk, sizeof(chunk), &produced, &done)) return false;
      chunkLen = produced;
      chunkPos = 0;
      streamDone = done;
      if (produced == 0 && done) return false;
    }
    b = chunk[chunkPos++];
    return true;
  };

  uint8_t b0 = 0, b1 = 0;
  if (!nextByte(b0) || !nextByte(b1) || b0 != 0xFF || b1 != 0xD8) return false;  // SOI

  while (true) {
    uint8_t b = 0;
    if (!nextByte(b)) return false;
    if (b != 0xFF) continue;  // resync to next marker prefix
    uint8_t marker = 0;
    do {
      if (!nextByte(marker)) return false;  // skip fill bytes
    } while (marker == 0xFF);
    // Standalone markers carry no length payload.
    if (marker == 0x00 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) continue;

    uint8_t l0 = 0, l1 = 0;
    if (!nextByte(l0) || !nextByte(l1)) return false;
    const uint16_t segLen = (static_cast<uint16_t>(l0) << 8) | l1;
    if (segLen < 2) return false;

    const bool isSof = (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
                       (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
    if (isSof) {
      uint8_t sof[5] = {0};  // precision, height(hi,lo), width(hi,lo)
      for (uint8_t& s : sof)
        if (!nextByte(s)) return false;
      const uint16_t h = (static_cast<uint16_t>(sof[1]) << 8) | sof[2];
      const uint16_t w = (static_cast<uint16_t>(sof[3]) << 8) | sof[4];
      if (w == 0 || h == 0 || w > 0x7FFF || h > 0x7FFF) return false;
      out.width = static_cast<int16_t>(w);
      out.height = static_cast<int16_t>(h);
      if (outMode) *outMode = classifyJpegMode(marker);
      return true;
    }
    if (marker == 0xDA) return false;  // SOS — entropy data begins, no SOF was found

    for (uint16_t i = 0; i < segLen - 2; i++) {
      uint8_t skip = 0;
      if (!nextByte(skip)) return false;
    }
  }
}

bool JpegToFramebufferConverter::getModeFromHeader(const std::string& imagePath, JpegMode& out) {
  ImageDimensions dims{};
  return readJpegDimensionsFromHeader(imagePath, dims, &out);
}

bool JpegToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  if (!readJpegDimensionsFromHeader(imagePath, out)) {
    return false;
  }
  LOG_TRC("JPG", "Image dimensions: %dx%d", out.width, out.height);
  return true;
}

bool JpegToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                     const RenderConfig& config) {
  LOG_TRC("JPG", "Decoding JPEG: %s", imagePath.c_str());
  jpgCheckHeap("jpg_decode_entry");

  // No pre-decode marker validation here. An EOI gate used to guard JPEGDEC, which
  // hard-faulted on the garbage entropy data of a file truncated by a crashed
  // extraction; TJpgDec streams instead and its input callback simply returns short
  // at EOF, so truncation surfaces as JDR_INP and is handled below like any other
  // decode failure. The gate outlived its decoder and was rejecting valid images —
  // trailing NUL padding after EOI is common enough that it deleted and re-extracted
  // a good cover on every single open. Match the cover/thumbnail path
  // (Epub::coverImageCachedValidOnly): trust the magic bytes, let the decode fail.

  JpegContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();
  ctx.effectiveDitherMode = config.ditherMode;

  // TJpgDec handles baseline JPEGs. Progressive ones go to ProgressiveJpeg (every scan, band by
  // band) when the frame and its workspace allow, else to the DC-only preview (1/8 resolution).
  JpegMode mode = JpegMode::Baseline;
  if (!getModeFromHeader(imagePath, mode)) {
    LOG_ERR("JPG", "Could not determine JPEG mode (no SOF marker): %s", imagePath.c_str());
    return false;
  }
  if (mode == JpegMode::Other) {
    LOG_INF("JPG", "Unsupported JPEG mode — drawing placeholder: %s", imagePath.c_str());
    drawUnsupportedPlaceholder(renderer, config);
    return true;
  }

  const size_t requiredHeap = mode == JpegMode::Progressive ? MIN_FREE_HEAP_FOR_PROGRESSIVE_JPEG : minFreeHeapForJpeg();
  const size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < requiredHeap) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", freeHeap, requiredHeap);
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("JPG", imagePath, file)) {
    LOG_ERR("JPG", "Failed to open JPEG for decode: %s", imagePath.c_str());
    return false;
  }

  TJpgSession session{&file, &ctx};
  // Declared for the whole function but only filled on the baseline branch below (the
  // progressive path uses its own decoder and never needs the pool). Must outlive jd_decomp,
  // which reads through it — hence function scope rather than a block-local inside the branch.
  std::unique_ptr<JpegWorkPool> pool;
  JDEC jdec{};
  JRESULT jr = JDR_OK;
  int srcWidth = 0;
  int srcHeight = 0;
  ProgressiveJpeg::ImageInfo progressiveInfo;
  bool fullProgressive = false;
  if (mode == JpegMode::Progressive) {
    fullProgressive = ProgressiveJpeg::probe(file, progressiveInfo) == ProgressiveJpeg::Result::Ok;
    if (fullProgressive) {
      srcWidth = progressiveInfo.width;
      srcHeight = progressiveInfo.height;
    } else {
      ProgressiveJpegDc::ImageInfo info;
      const auto probeResult = ProgressiveJpegDc::probe(file, info);
      if (probeResult != ProgressiveJpegDc::Result::Ok) {
        LOG_ERR("JPG", "Progressive JPEG probe failed: %s", ProgressiveJpegDc::resultName(probeResult));
        file.close();
        return false;
      }
      srcWidth = info.width;
      srcHeight = info.height;
    }
  } else {
    pool = makeUniqueNoThrow<JpegWorkPool>(TJPG_WORK_POOL_SIZE);
    if (!pool || !*pool) {
      LOG_ERR("JPG", "Failed to allocate TJpgDec work pool (%u bytes)", static_cast<unsigned>(TJPG_WORK_POOL_SIZE));
      file.close();
      return false;
    }
    jr = jd_prepare(&jdec, tjpgInput, pool->get(), TJPG_WORK_POOL_SIZE, &session);
    if (jr != JDR_OK) {
      LOG_ERR("JPG", "TJpgDec prepare failed (jr=%d): %s", jr, imagePath.c_str());
      file.close();
      return false;
    }
    srcWidth = jdec.width;
    srcHeight = jdec.height;
  }
  if (srcWidth <= 0 || srcHeight <= 0) {
    LOG_ERR("JPG", "Invalid JPEG dimensions: %dx%d", srcWidth, srcHeight);
    file.close();
    return false;
  }

  // Calculate overall target scale
  float targetScale;
  int destWidth, destHeight;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    destWidth = config.maxWidth;
    destHeight = config.maxHeight;
    targetScale = (float)destWidth / srcWidth;
  } else {
    float scaleX = (config.maxWidth > 0 && srcWidth > config.maxWidth) ? (float)config.maxWidth / srcWidth : 1.0f;
    float scaleY = (config.maxHeight > 0 && srcHeight > config.maxHeight) ? (float)config.maxHeight / srcHeight : 1.0f;
    targetScale = (scaleX < scaleY) ? scaleX : scaleY;
    if (targetScale > 1.0f) targetScale = 1.0f;

    destWidth = (int)(srcWidth * targetScale);
    destHeight = (int)(srcHeight * targetScale);
  }

  // Coarse DCT downscale (TJpgDec's built-in 1/1..1/8, or the progressive decoder's reduced
  // IDCT); the fine resampler in emitGrayBlock covers the residual ratio.
  uint8_t tjpgScale = 0;
  int jpegScaleDenom = 1;
  std::unique_ptr<JpegWorkPool> progressiveWorkspace;
  size_t progressiveWorkspaceBytes = 0;
  if (mode == JpegMode::Baseline) {
    jpegScaleDenom = chooseJpegScale(targetScale, tjpgScale);
  } else if (fullProgressive) {
    // A coarser scale keeps fewer coefficients per block: step down until the workspace fits.
    chooseJpegScale(targetScale, tjpgScale);
    for (; tjpgScale <= 3; ++tjpgScale) {
      progressiveWorkspaceBytes = ProgressiveJpeg::workspaceBytes(progressiveInfo, tjpgScale);
      progressiveWorkspace =
          makeUniqueNoThrow<JpegWorkPool>(progressiveWorkspaceBytes, PROGRESSIVE_WORKSPACE_HEAP_FLOOR);
      if (progressiveWorkspace && *progressiveWorkspace) break;
      progressiveWorkspace.reset();
    }
    if (progressiveWorkspace) {
      jpegScaleDenom = 1 << tjpgScale;
    } else {
      LOG_INF("JPG", "No room for a progressive workspace (%u free); DC-only preview",
              static_cast<unsigned>(ESP.getFreeHeap()));
      fullProgressive = false;
      tjpgScale = 0;
    }
  }
  // The DC-only preview resizes to the destination itself.
  const bool dcPreview = mode == JpegMode::Progressive && !fullProgressive;

  // TJpgDec descales by floor(dim / 2^scale): each MCU side (8 or 16 px) is a multiple of
  // the scale denominator, so its per-MCU shifts sum to exactly the floor. Match that here
  // (not ceil) so scaledSrc* equals the decoder's true output extent — the fine-scale
  // factors and the right/bottom edge snapping below are derived from these. ProgressiveJpeg
  // emits the same floor extent.
  ctx.scaledSrcWidth = dcPreview ? destWidth : srcWidth / jpegScaleDenom;
  ctx.scaledSrcHeight = dcPreview ? destHeight : srcHeight / jpegScaleDenom;

  // Validate memory footprint against the post-scaling decode size, not raw dimensions.
  // A 1447x2200 image decoded at 1/4 scale is only ~362x550 — well within limits.
  if (!validateImageDimensions(ctx.scaledSrcWidth, ctx.scaledSrcHeight, "JPEG")) {
    file.close();
    return false;
  }
  ctx.dstWidth = destWidth;
  ctx.dstHeight = destHeight;
  if (destWidth <= 0 || destHeight <= 0) {
    LOG_ERR("JPG", "Zero-sized output (%dx%d), aborting", destWidth, destHeight);
    file.close();
    return false;
  }
  configureResample(ctx, !dcPreview);

  LOG_TRC("JPG", "JPEG %dx%d -> %dx%d (scale %.2f, jpegScale 1/%d, fineScale %.2f)", srcWidth, srcHeight, destWidth,
          destHeight, targetScale, jpegScaleDenom, (float)destWidth / ctx.scaledSrcWidth);

  // The tallest block the decoder emits, in scaled-source rows: a TJpgDec MCU row is 8 * msy
  // source rows (8 for grayscale/4:4:4, 16 for 4:2:0) descaled by the DCT step, a ProgressiveJpeg
  // band one block row at its output scale. Our fine scale maps it to this many output rows --
  // the tallest span either the disk cache band or the dither row band (below) ever needs to hold
  // in one piece. The DC preview emits single rows. It used to be a flat 16: at 0.95 residual
  // scale on the X3 that is 17 rows at 512 px, over the 8 KB dither-band budget, and the BW
  // rendition silently fell back to Bayer (and lost its companion cache with it).
  const int srcBlockRows = fullProgressive ? (8 >> tjpgScale) : ((8 * jdec.msy) >> tjpgScale);
  const int maxBlockDstRows = dcPreview ? 1 : (int)(((int64_t)srcBlockRows * ctx.fineScaleFPY) >> FP_SHIFT) + 2;

  // Start streaming the pixel cache to disk.
  // (See PixelCache for why streaming replaced a full-image buffer; ported from
  // upstream commit d9bcef7a, crosspoint-reader#2230.)
  ctx.caching = shouldEnableJpegCache(config, destWidth, destHeight, maxBlockDstRows);
#ifdef JPG_DIAG_DISABLE_CACHE
  // Diagnostic bisect: skip the streaming band-cache writer (DirectCacheWriter into
  // ctx.cache.buffer) entirely. Define in platformio.local.ini (build_flags) for a
  // dedicated run, never in normal builds.
  ctx.caching = false;
#endif
  if (ctx.caching) {
    if (!ctx.cache.begin(config.cachePath, destWidth, destHeight, config.x, config.y, maxBlockDstRows)) {
      LOG_ERR("JPG", "Failed to start cache stream, continuing without caching");
      ctx.caching = false;
    }
  }

  if (shouldForceBayerDither(config)) {
    LOG_DBG("JPG", "Low-memory mode: forcing Bayer dithering (%u free)", static_cast<unsigned>(ESP.getFreeHeap()));
    ctx.effectiveDitherMode = ImageDitherMode::Bayer;
  }

  // The row-based ditherers (Atkinson1Bit, Atkinson, DiffusedBayer) carry
  // left-to-right error-diffusion state and need pixels delivered in true raster
  // order. TJpgDec instead delivers one MCU block at a time — see
  // JpegContext::ditherBand. Buffer a full MCU row band before constructing any
  // of these so emitGrayBlock always has somewhere to stash pixels; if the band
  // can't be allocated, skip straight to the stateless (order-independent) Bayer
  // path below rather than dithering with corrupted state.
  bool wantsStatefulDither = config.monochromeOutput;
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  if (!wantsStatefulDither && config.useDithering &&
      (ctx.effectiveDitherMode == ImageDitherMode::Atkinson ||
       ctx.effectiveDitherMode == ImageDitherMode::DiffusedBayer)) {
    wantsStatefulDither = true;
  }
#endif

  bool ditherBandReady = false;
  if (wantsStatefulDither && destWidth > 0) {
    // Budget: keep well under the headroom left after the 12KB TJpgDec work pool
    // and up to 24KB PixelCache band on a page that may have only ~55KB free.
    constexpr size_t DITHER_BAND_MAX_BYTES = 8 * 1024;
    int bandRows = maxBlockDstRows < 1 ? 1 : maxBlockDstRows;
    size_t maxRowsByMem = DITHER_BAND_MAX_BYTES / (size_t)destWidth;
    if (maxRowsByMem < 1) maxRowsByMem = 1;
    if ((size_t)bandRows <= maxRowsByMem) {
      ctx.ditherBand = makeUniqueNoThrow<uint8_t[]>((size_t)destWidth * (size_t)bandRows);
      if (ctx.ditherBand) {
        ctx.ditherBandCapacityRows = bandRows;
        ditherBandReady = true;
      } else {
        LOG_ERR("JPG", "OOM allocating %d-row dither band (%u bytes); falling back to Bayer dithering", bandRows,
                (unsigned)((size_t)destWidth * (size_t)bandRows));
      }
    } else {
      LOG_DBG("JPG", "Dither band needs %d rows > %u budget rows; falling back to Bayer dithering", bandRows,
              (unsigned)maxRowsByMem);
    }
  }

  // See PngToFramebufferConverter for rationale: BW-only display needs a 1-bit
  // dither so mid-grays don't collapse to black under DirectPixelWriter's `< 3` rule.
  if (config.monochromeOutput && ditherBandReady) {
    ctx.atkinson1BitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(destWidth);
    if (!ctx.atkinson1BitDitherer) {
      LOG_ERR("JPG", "Failed to allocate 1-bit Atkinson ditherer, falling back to 4-level dither");
    }
  }

  if (config.useDithering && !ctx.atkinson1BitDitherer && ditherBandReady) {
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
    switch (ctx.effectiveDitherMode) {
      case ImageDitherMode::Atkinson:
        ctx.atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(destWidth);
        if (!ctx.atkinsonDitherer) {
          LOG_ERR("JPG", "Failed to allocate Atkinson ditherer, falling back to Bayer");
        }
        break;
      case ImageDitherMode::DiffusedBayer:
        ctx.diffusedBayerDitherer = makeUniqueNoThrow<DiffusedBayerDitherer>(destWidth);
        if (!ctx.diffusedBayerDitherer) {
          LOG_ERR("JPG", "Failed to allocate diffused Bayer ditherer, falling back to Bayer");
        }
        break;
      case ImageDitherMode::Bayer:
      case ImageDitherMode::COUNT:
      default:
        break;
    }
#endif
  }

  // If neither stateful ditherer ended up constructed (band unavailable, or alloc
  // failed above), drop the band too so emitGrayBlock takes the direct-write path.
  if (!ctx.atkinson1BitDitherer
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
      && !ctx.atkinsonDitherer && !ctx.diffusedBayerDitherer
#endif
  ) {
    ctx.ditherBand.reset();
    ctx.ditherBandCapacityRows = 0;
  }

  // Companion rendition: needs the raster-order band (see JpegContext::Companion) and its own
  // streaming cache band, gated like the primary's.
  if (!config.companionCachePath.empty() && ctx.ditherBand && ctx.caching &&
      shouldEnableJpegCache(config, destWidth, destHeight, maxBlockDstRows)) {
    ctx.companion = makeUniqueNoThrow<JpegContext::Companion>();
    if (ctx.companion && !config.monochromeOutput) {
      ctx.companion->atkinson1Bit = makeUniqueNoThrow<Atkinson1BitDitherer>(destWidth);
      if (!ctx.companion->atkinson1Bit) ctx.companion.reset();
    }
    if (ctx.companion && !ctx.companion->cache.begin(config.companionCachePath, destWidth, destHeight, config.x,
                                                     config.y, maxBlockDstRows)) {
      LOG_ERR("JPG", "Failed to start companion cache stream, continuing with one variant");
      ctx.companion.reset();
    }
  }

  jpgCheckHeap("jpg_before_decode");
  unsigned long decodeStart = millis();
  ProgressiveJpegDc::Result progressiveResult = ProgressiveJpegDc::Result::Ok;
  bool runDcPreview = dcPreview;
  if (fullProgressive) {
    ProgressiveJpeg::DecodeOptions options;
    options.scaleShift = tjpgScale;
    options.shouldAbort = fullProgressiveShouldAbort;
    options.workspace = progressiveWorkspace->get();
    options.workspaceSize = progressiveWorkspaceBytes;
    FullProgressiveSink sink{&ctx, false};
    const auto result = ProgressiveJpeg::decode(file, options, fullProgressiveOutput, &sink);
    progressiveWorkspace.reset();
    if (result == ProgressiveJpeg::Result::Aborted) {
      progressiveResult = ProgressiveJpegDc::Result::Aborted;
    } else if (result != ProgressiveJpeg::Result::Ok) {
      LOG_ERR("JPG", "Progressive JPEG full decode failed (%s)%s: %s", ProgressiveJpeg::resultName(result),
              sink.emitted ? "" : ", trying the DC preview", imagePath.c_str());
      if (sink.emitted) {
        progressiveResult = ProgressiveJpegDc::Result::InvalidData;
      } else {
        // Nothing was drawn or cached yet, so the preview can start over on the same context.
        ctx.scaledSrcWidth = destWidth;
        ctx.scaledSrcHeight = destHeight;
        configureResample(ctx, false);
        runDcPreview = true;
      }
    }
  }
  if (runDcPreview) {
    ProgressiveJpegDc::DecodeOptions options;
    options.outputWidth = destWidth;
    options.outputHeight = destHeight;
    options.shouldAbort = progressiveShouldAbort;
    progressiveResult = ProgressiveJpegDc::decode(file, options, progressiveOutput, &ctx);
  } else if (mode == JpegMode::Baseline) {
    jr = jd_decomp(&jdec, tjpgOutput, tjpgScale);
  }
  unsigned long decodeTime = millis() - decodeStart;
  // Check before abort() so a corrupt reading is attributed to the decode itself
  // rather than to the SD-buffer free inside cache cleanup. Runs on both paths.
  jpgCheckHeap("jpg_after_decode");
  file.close();

  if (mode == JpegMode::Progressive && progressiveResult != ProgressiveJpegDc::Result::Ok) {
    LOG_ERR("JPG", "Progressive JPEG decode failed (%s): %s", ProgressiveJpegDc::resultName(progressiveResult),
            imagePath.c_str());
    if (ctx.caching) ctx.cache.abort();
    if (ctx.companion) ctx.companion->cache.abort();
    drawUnsupportedPlaceholder(renderer, config);
    return true;
  }
  if (mode == JpegMode::Baseline && jr != JDR_OK) {
    LOG_ERR("JPG", "TJpgDec decode failed (jr=%d): %s", jr, imagePath.c_str());
    if (ctx.caching) ctx.cache.abort();
    if (ctx.companion) ctx.companion->cache.abort();
    return false;
  }

  LOG_DBG("JPG", "JPEG decoding complete - render time: %lu ms%s", decodeTime, ctx.companion ? " (both variants)" : "");

  // Finalize the streamed cache file. Note: a flush failure mid-decode clears
  // ctx.caching (the partial file is dropped), so re-read the flag here.
  if (ctx.caching) {
    ctx.cache.finalize();
  }
  // Only alongside a complete primary: a companion without it would be a cache nobody asked for.
  if (ctx.companion) {
    if (ctx.caching) {
      ctx.companion->cache.finalize();
    } else {
      ctx.companion->cache.abort();
    }
  }

  return true;
}

bool JpegToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasJpgExtension(extension);
}
