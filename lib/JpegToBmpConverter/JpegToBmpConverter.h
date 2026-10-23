#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>

class BuildArena;  // lib/Memory -- optional scratch for the work pool / progressive workspace
class Print;
class ZipFile;

// A JPEG whose decode fails after it has delivered rows -- a file cut short, a corrupt stretch --
// or that finishes short of what the file should hold (ProgressiveJpeg's Degraded) still makes a
// complete BMP of its box: the rows decoded so far, and white below. Every entry point reports it
// as converted, and the callers keep it like any cover: the BMP is the only thing Home and the
// sleep screen can draw, and a cover that is not kept ends, after Home's retry budget, as the
// no-cover placeholder for good. The covers that reach a conversion have passed a completeness
// check first (Epub::coverImageComplete: the EOI marker) or are read in place from a complete
// archive, so what is short here is short in the source, and converting again would give the same.
// A decode that stops for input, or delivers no row, fails as before.
class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true, bool eightBit = false,
                                          BuildArena* scratch = nullptr);

 public:
  // grayscale8Bit: emit an 8-bit BMP instead of quantizing to the display's four
  // levels here, deferring dithering to draw time. Only worth it for a consumer
  // that can use the extra tonal range (the sleep screen's adaptive tone filter);
  // it costs 4x the SD footprint of a 2-bit cover.
  // `scratch` (every entry point): a lent region for the decoder's working memory -- the
  // TJpgDec pool or the progressive workspace -- so a cover decode needs only the row pipeline
  // from the heap. Null decodes from the heap behind the historical free-heap gates.
  static bool jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop = true, bool grayscale8Bit = false,
                                  BuildArena* scratch = nullptr);
  // Convert with custom target size (for thumbnails)
  static bool jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                          BuildArena* scratch = nullptr);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering.
  // crop=true fills the box and centre-crops the overflow, so the BMP is exactly the box; false fits
  // the whole image inside it, so one dimension is the box's and the other at most.
  static bool jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                              BuildArena* scratch = nullptr, bool crop = true);

  // One 1-bit, crop-fitted BMP per target from a SINGLE decode (the Lyra carousel's two cover
  // thumbnails): the DCT pre-scale is the one the largest target needs, and every target gets its
  // own resampler, ditherer and BMP stream fed from the same source rows, so each is still dithered
  // once from gray -- never rescaled after dithering. The largest target's BMP is byte-identical to
  // what jpegFileTo1BitBmpStreamWithSize writes for it alone. False when any output failed.
  static constexpr int kMaxTargets = 2;
  struct BmpTarget {
    Print* out;
    int maxWidth;
    int maxHeight;
  };
  static bool jpegFileTo1BitBmpStreamsWithSizes(FsFile& jpegFile, const BmpTarget* targets, int count,
                                                BuildArena* scratch = nullptr);
};

// jpegFileTo1BitBmpStreamsWithSizes in slices, for a caller that has to stay responsive: Home's
// cover pass runs a few units, checks for a button press, and comes back to the same place --
// where a one-shot conversion that gives way to input throws its work away and starts over from
// the file's first byte (memory audit 2026-09, R9 item 3). The BMPs are byte-identical to the
// one-shot conversion's however the work is sliced.
//
// Baseline JPEGs (TJpgDec) and full progressive decodes run sliced. A progressive image the
// one-shot conversion resizes from its 1/8 decode (no scale's workspace fits, or only 1/8 does and
// the box is larger) is refused, and so is anything else the one-shot path would reject: begin()
// returns null and the caller falls back to the one-shot conversion, which handles every case.
//
// The session keeps its decoder state, working memory and row pipeline between calls -- from
// `scratch` when there is room, else the heap -- so `scratch` must stay lent, the JPEG file and
// every output open, and nothing else may take a block from `scratch` until the session is gone.
class JpegThumbSession {
 public:
  enum class Status : uint8_t { Running, Done, Error };

  // Starts at the file's current position (0 for a JPEG of its own, the entry's data offset for a
  // cover stored uncompressed in the EPUB). Each target's BMP header is written into its buffered
  // stream here; a null return may leave that header in the output, which the caller discards.
  static std::unique_ptr<JpegThumbSession> begin(FsFile& jpegFile, const JpegToBmpConverter::BmpTarget* targets,
                                                 int count, BuildArena* scratch = nullptr);

  // Does up to `units` of decoding: a unit is one MCU row of a baseline image, or 4 KB of a
  // progressive image's index pass and then one band. Running: call again. Done: every BMP is
  // complete and flushed -- also for a decode that ended early after rows (see JpegToBmpConverter).
  // Error: the conversion failed; the outputs hold partial data.
  Status continueSteps(uint16_t units);

  bool progressive() const;

  ~JpegThumbSession();
  JpegThumbSession(const JpegThumbSession&) = delete;
  JpegThumbSession& operator=(const JpegThumbSession&) = delete;

 private:
  struct Impl;
  explicit JpegThumbSession(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
