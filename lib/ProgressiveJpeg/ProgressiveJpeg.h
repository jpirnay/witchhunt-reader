#pragma once

#include <BuildArena.h>
#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <memory>

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
// WHOLE block, so a 64-bit mask per block tracks that for the ones not kept. At 1/8 only the DC
// coefficient is kept, so the AC scans are not decoded at all.
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
  Pending,  // Decoder::step(): more work remains
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
// `base`: where the JPEG starts in the file -- 0 for a file of its own, the entry's data offset for
// an image stored uncompressed in a ZIP and read in place. Every offset is relative to it.
Result probe(FsFile& file, ImageInfo& info, uint32_t base = 0);

// Working memory decode() needs at output scale 1/2^scaleShift, including alignment slack.
size_t workspaceBytes(const ImageInfo& info, uint8_t scaleShift);
// The read windows a decode can be given (DecodeOptions::readWindows): a window for each luma scan
// instead of one buffer they all share. The output is the same; the reads are a fraction. 8 KB.
size_t scanWindowBytes();

// One band of output rows: `rows` rows of `width` gray pixels starting at output row `y`, row
// pitch `stride`. Rows arrive in order and cover floor(height >> s) rows in total. Return false
// to stop the decode (Result::Stopped).
using BandCallback = bool (*)(void* user, uint16_t y, const uint8_t* gray, uint16_t width, uint16_t rows,
                              uint16_t stride);
using AbortCallback = bool (*)(void* user);

// Where a decode's time went, for the caller's log. Times need DecodeOptions::clock.
struct DecodeStats {
  uint32_t indexMs = 0;  // header + scan index: one pass over the whole file
  uint32_t bandsMs = 0;  // entropy decode, IDCT and the band callbacks (which include the caller's
                         // resample/dither/cache work)
  uint32_t reads = 0;    // file reads (each a seek + up to 512 bytes, or a window's worth)
  uint32_t bytesRead = 0;
};

struct DecodeOptions {
  // Where the JPEG starts in the file (see probe()).
  uint32_t base = 0;
  uint8_t scaleShift = 0;  // 0..3: output is floor(width >> s) x floor(height >> s)
  // Polled before every band and every 32 KB while indexing the file (the whole file is read
  // before the first band): a watchdog feed belongs here. True aborts with Result::Aborted.
  // A Decoder driven in steps polls it the same way within each step.
  AbortCallback shouldAbort = nullptr;
  void* abortUser = nullptr;
  // Caller-provided working memory of at least workspaceBytes() (e.g. a scratch arena). When
  // null, decode() takes one heap block of that size for the duration of the call.
  uint8_t* workspace = nullptr;
  size_t workspaceSize = 0;
  // Optional read windows of at least scanWindowBytes() (ReadWindows takes them from a lent region),
  // valid until the decode ends. Without them every scan reads through one shared 512-byte buffer.
  uint8_t* readWindows = nullptr;
  size_t readWindowsSize = 0;
  // Optional instrumentation: filled when non-null; times only when `clock` (ms) is given too.
  DecodeStats* stats = nullptr;
  uint32_t (*clock)() = nullptr;
  // At 1/8: a pixel for every block, the partial ones at the right and bottom edges too, so the
  // output is ceil(width / 8) x ceil(height / 8). Each of those is its block's average, padding
  // included -- a resampler that maps the picture onto a box wants them all (decodeResized); a
  // caller that wants the scale's own size (floor, as at every scale) leaves it off.
  bool wholeBlocks = false;
};

// Every failure is reported before the first band is emitted unless the entropy data itself is
// corrupt or the card fails a read or seek (IoError), so a caller can fall back to another decoder
// on Unsupported / OutOfMemory.
Result decode(FsFile& file, const DecodeOptions& options, BandCallback callback, void* user);

// One output row of decodeResized(): `width` gray pixels at row `y`. False stops the decode.
using RowCallback = bool (*)(void* user, uint16_t y, const uint8_t* gray, uint16_t width);

// The whole picture at the size of a box: the 1/8 decode (a DC-only session, see workspaceBytes)
// resampled bilinearly to outputWidth x outputHeight, rows in order. The converters' last resort
// when the heap holds no finer scale's workspace. A 1/8 level is a whole block's average, so an
// upscale has to interpolate between them in both axes, or it shows the block grid; a converter's
// per-band resampler sees one row of blocks at a time and cannot. The source is every block
// (DecodeOptions::wholeBlocks), mapped corner to corner onto the box. The workspace comes as for
// decode() (options.scaleShift and wholeBlocks are set here), the resampler's three output rows
// from the heap. Results as decode()'s.
Result decodeResized(FsFile& file, const DecodeOptions& options, uint16_t outputWidth, uint16_t outputHeight,
                     RowCallback callback, void* user);

// decode() in pieces, for a caller that must stay responsive between them (Home's cover pass
// pauses for a button press and resumes where it stopped instead of starting over). decode() is
// begin() + step() until done, so the output is identical however the work is sliced.
//
// All state lives at the front of the workspace; the file and the workspace must stay valid, and
// the file open, until end(). Other reads of the same file between steps are fine: every read
// seeks first.
class Decoder {
 public:
  Decoder() = default;
  ~Decoder() { end(); }
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;

  // Probes the file and claims the workspace. Ok: call step(). Anything else is final (the same
  // results decode() reports before its first band).
  Result begin(FsFile& file, const DecodeOptions& options, BandCallback callback, void* user);
  // Does up to `units` of work: while indexing, a unit is 4 KB of the file; after that, one band.
  // The index pass and the bands never share a step. Pending: call again. Ok: every band was
  // emitted. Aborted (the abort hook fired): stopped between two units, and the session can be
  // stepped on or checkpointed. Anything else is final; a finished decode reports its outcome again.
  Result step(uint16_t units);

  // A paused decode as bytes, for a caller that must give its workspace back before it can finish
  // (the reader's image lane, whose region returns to the display on every page turn). Valid after
  // begin() and after a step() that returned Pending or Aborted: a decode only ever stops between
  // index chunks or bands, where the session holds its whole state. checkpointSize() bytes.
  size_t checkpointSize() const;
  bool writeCheckpoint(FsFile& out) const;
  // begin() from a checkpoint of the same image (same file contents, base and scale): claims the
  // workspace like begin() and continues where the checkpoint stopped, so the rows still to come
  // are exactly the ones the uninterrupted decode would have emitted. InvalidData when the
  // checkpoint does not fit -- another image, scale or firmware layout; the caller starts over.
  Result resume(FsFile& file, const DecodeOptions& options, BandCallback callback, void* user, FsFile& checkpoint);
  uint16_t bandsDone() const;
  uint16_t bandCount() const;
  // Releases the workspace (when the decoder took its own) and rewinds the file to the JPEG's
  // start. Safe to call at any point, and more than once.
  void end();
  bool active() const { return session_ != nullptr; }

 private:
  Result claim(FsFile& file, const DecodeOptions& options, BandCallback callback, void* user);

  // The session at the front of the workspace: one for every scan, or at 1/8 one for the DC scans
  // alone (dcOnly_), which is a fraction of the size.
  void* session_ = nullptr;
  bool dcOnly_ = false;
  std::unique_ptr<uint8_t[]> owned_;
  FsFile* file_ = nullptr;
  DecodeOptions options_{};
};

const char* resultName(Result result);

// A decode's read windows (scanWindowBytes()) in a block of a lent region, when it has room: the
// one way every caller gets them, and never from the heap -- a decode without them reads more,
// but no worse. take() reserves the block; it goes back when this object does, so the object must
// be released before any block reserved ahead of it (the region is LIFO), and must outlive the
// decode it was applied to.
class ReadWindows {
 public:
  ReadWindows() = default;
  ~ReadWindows() { release(); }
  ReadWindows(const ReadWindows&) = delete;
  ReadWindows& operator=(const ReadWindows&) = delete;

  // True when `region` served them.
  bool take(BuildArena* region);
  void release();
  // Hands them, if any, to a decode.
  void applyTo(DecodeOptions& options) const;
  bool held() const { return data_ != nullptr; }

 private:
  BuildArena* region_ = nullptr;
  BuildArena::Block block_;
  uint8_t* data_ = nullptr;
};

}  // namespace ProgressiveJpeg
