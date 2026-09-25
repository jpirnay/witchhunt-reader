#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

// Full decoder for progressive (SOF2) JPEGs: grayscale (luma) output at 1/1, 1/2, 1/4 or 1/8
// scale, streamed top to bottom in bands, without a whole-image coefficient buffer.
//
// A progressive file stores the picture as a sequence of scans, each a pass over the whole image
// that refines some coefficients. Decoding in bands therefore needs every scan to be positioned
// at the band's first block at once. One pass over the file indexes the scans; after that each
// scan gets a resumable cursor (file offset, bit buffer, EOB run, DC predictors, restart state)
// and a band is decoded by advancing every luma scan across it in file order. The file is still
// read about once in total; the working set is one band of coefficients.
//
// Only the coefficients the output scale needs are kept (the top-left 8/2^s square of each
// block) and inverted with an n-point IDCT -- libjpeg's reduced-size decoding: a proper low-pass
// resample of the block, sharper and with less aliasing than a box average of the full decode.
// Successive-approximation refinement needs to know which coefficients are non-zero across the
// WHOLE block, so a 64-bit mask per block tracks that for the ones not kept.
//
// Chroma is decoded only where the bitstream forces it (interleaved DC scans) and discarded.
namespace ProgressiveJpeg {

enum class Result : uint8_t {
  Ok,
  Unsupported,  // not SOF2, not 8-bit, unusual sampling, or more scans/tables than we index
  InvalidData,
  IoError,
  OutOfMemory,
  Aborted,
  Stopped,  // the band callback asked to stop
};

struct ImageInfo {
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t componentCount = 0;
  uint8_t maxHorizontal = 1;  // sampling factors, max over all components
  uint8_t maxVertical = 1;
};

// Reads marker segments through SOF and rewinds the file. Unsupported for anything
// decode() would refuse on geometry alone (non-SOF2, precision, luma not at full sampling).
Result probe(FsFile& file, ImageInfo& info);

// Working memory decode() needs at output scale 1/2^scaleShift, including alignment slack.
size_t workspaceBytes(const ImageInfo& info, uint8_t scaleShift);

// One band of output rows: `rows` rows of `width` gray pixels starting at output row `y`, row
// pitch `stride`. Rows arrive in order and cover floor(height >> s) rows in total. Return false
// to stop the decode (Result::Stopped).
using BandCallback = bool (*)(void* user, uint16_t y, const uint8_t* gray, uint16_t width, uint16_t rows,
                              uint16_t stride);
using AbortCallback = bool (*)(void* user);

struct DecodeOptions {
  uint8_t scaleShift = 0;  // 0..3: output is floor(width >> s) x floor(height >> s)
  // Polled before every band and every 32 KB while indexing the file (the whole file is read
  // before the first band): a watchdog feed belongs here. True aborts with Result::Aborted.
  AbortCallback shouldAbort = nullptr;
  void* abortUser = nullptr;
  // Caller-provided working memory of at least workspaceBytes() (e.g. a scratch arena). When
  // null, decode() takes one heap block of that size for the duration of the call.
  uint8_t* workspace = nullptr;
  size_t workspaceSize = 0;
};

// Every failure is reported before the first band is emitted unless the entropy data itself is
// corrupt, so a caller can fall back to another decoder on Unsupported / OutOfMemory.
Result decode(FsFile& file, const DecodeOptions& options, BandCallback callback, void* user);

const char* resultName(Result result);

}  // namespace ProgressiveJpeg
