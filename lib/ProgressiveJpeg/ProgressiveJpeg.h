#pragma once

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
// Workspace past workspaceBytes() that, when the caller gives it, the decode reads the file
// through: a window for each luma scan instead of one buffer they all share. The output is the
// same; the reads are a fraction (8 KB).
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
  // Caller-provided working memory of at least workspaceBytes() (e.g. a scratch arena), plus
  // scanWindowBytes() for the read windows if it can spare them. When null, decode() takes one
  // heap block of workspaceBytes() for the duration of the call.
  uint8_t* workspace = nullptr;
  size_t workspaceSize = 0;
  // Optional instrumentation: filled when non-null; times only when `clock` (ms) is given too.
  DecodeStats* stats = nullptr;
  uint32_t (*clock)() = nullptr;
};

// Every failure is reported before the first band is emitted unless the entropy data itself is
// corrupt, so a caller can fall back to another decoder on Unsupported / OutOfMemory.
Result decode(FsFile& file, const DecodeOptions& options, BandCallback callback, void* user);

struct Session;

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

  Session* session_ = nullptr;
  std::unique_ptr<uint8_t[]> owned_;
  FsFile* file_ = nullptr;
  DecodeOptions options_{};
};

const char* resultName(Result result);

}  // namespace ProgressiveJpeg
