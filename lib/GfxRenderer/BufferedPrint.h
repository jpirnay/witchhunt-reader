#pragma once

#include <Print.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

// Batches small writes before they reach the underlying Print.
//
// Every BMP writer here emits one file write per output row, and a row is small: a 1-bit
// 340x540 cover thumbnail is 44 bytes a row, so it reached the card as 540 separate writes.
// That is expensive twice over on this stack -- a bare FsFile call costs ~1.5 ms whatever its
// size (see BufferedFileIO.h, where exactly this arithmetic cost a 1732-spine first-open index
// ~28 s), and a sub-sector write is worse still because the driver has to read the sector back
// before it can modify it. Measured on X4 through PixelCache, a 116-byte row write ran ~3.7 ms,
// which put a pair of cover thumbnails at ~3.4 s of pure call overhead.
//
// Wrapping the sink is the whole fix: the converters go on writing a row at a time and the rows
// coalesce here into 4 KB transfers. Nothing about the bytes changes, only how many calls carry
// them.
class BufferedPrint final : public Print {
 public:
  static constexpr size_t DEFAULT_BUFFER_BYTES = 4096;

  explicit BufferedPrint(Print& out, const size_t bufferBytes = DEFAULT_BUFFER_BYTES) : out_(out) {
    // nothrow: a failed allocation degrades to pass-through, which is exactly the old
    // behaviour, so a tight heap costs speed and never correctness.
    buf_.reset(new (std::nothrow) uint8_t[bufferBytes]);
    cap_ = buf_ ? bufferBytes : 0;
  }
  ~BufferedPrint() override { flushBuffer(); }
  BufferedPrint(const BufferedPrint&) = delete;
  BufferedPrint& operator=(const BufferedPrint&) = delete;

  size_t write(const uint8_t b) override { return write(&b, 1); }

  size_t write(const uint8_t* data, const size_t n) override {
    if (cap_ == 0) return out_.write(data, n);
    // A payload that cannot fit is not worth splitting: drain first so ordering holds, then
    // hand it straight to the sink -- it is already a large write, the kind this exists to make.
    if (n >= cap_) {
      if (!flushBuffer()) return 0;
      return out_.write(data, n);
    }
    if (fill_ + n > cap_ && !flushBuffer()) return 0;
    memcpy(bufferBase() + fill_, data, n);
    fill_ += n;
    return n;
  }

  // Named flushBuffer(), not flush(): Print already declares a `virtual void flush()`, and
  // silently overriding it with a different return type is a compile error on device and a
  // trap everywhere else. Callers should call this explicitly and fold the result into their
  // own success, so a write that fails at the very end is not reported as a complete file;
  // the destructor call is a best-effort backstop for paths that bail out early.
  bool flushBuffer() {
    if (fill_ == 0) return true;
    const size_t n = fill_;
    fill_ = 0;  // cleared first: a failing sink must not make every later flush retry the same bytes
    return out_.write(bufferBase(), n) == n;
  }

  // Drop whatever is pending without writing it. For the error paths that close and delete the
  // output file: without this the destructor would flush into a closed handle.
  void discard() { fill_ = 0; }

 private:
  // Explicitly typed, rather than calling buf_.get() at the use sites: cppcheck does not resolve
  // std::unique_ptr<uint8_t[]>::get() and reads arithmetic on it as void* pointer maths
  // (arithOperationsOnVoidPointer). Same accessor pattern, for the same reason, as
  // BufferedFileWriter::bufferBase() in Serialization/BufferedFileIO.h.
  uint8_t* bufferBase() const { return buf_.get(); }

  Print& out_;
  std::unique_ptr<uint8_t[]> buf_;
  size_t cap_ = 0;
  size_t fill_ = 0;
};
