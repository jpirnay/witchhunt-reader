#pragma once

// Ported from crosspoint-reader (PR #2583 by Uri Tauber, OOM fix #2791 by
// William Floyd). Reworked to inflate into a caller buffer with a
// session-held window instead of staging through a temp file.

#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <vector>

// Random-access reader for dictzip (.dict.dz) files: gzip with an extra "RA"
// field holding a chunk table, so any byte range can be decompressed without
// inflating the whole file. Format: https://linux.die.net/man/1/dictzip
namespace DictZip {

struct Info {
  uint32_t dataOffset = 0;             // file offset where compressed chunk data starts
  uint32_t totalSize = 0;              // uncompressed size (gzip ISIZE trailer)
  uint16_t chunkLength = 0;            // uncompressed bytes per chunk
  std::vector<uint32_t> chunkOffsets;  // cumulative compressed offsets, chunkCount+1 entries
  bool valid = false;
};

// Why an extraction failed, so callers can report the accurate cause.
enum class ExtractError : uint8_t {
  None,        // success
  LowMemory,   // an allocation failed — the ~32KB inflate window or the chunk
               // scratch couldn't be obtained from the fragmented heap
  ReadError,   // file open / read / bad-offset failure (IO or a bogus .idx
               // offset), not a compression problem
  Decompress,  // the compressed stream itself was bad (inflate failed, or the
               // .dz header/chunk table didn't parse) — corrupt/truncated .dz
};

// Defined in DictZip.cpp: the inflate reader, its input/discard buffers and the
// cached chunk table. Opaque here so the header stays free of uzlib.
struct ScratchState;

// The heap an extraction needs, owned by the caller across a whole lookup
// session instead of taken and released per lookup.
//
// The ~32KB inflate window is the one allocation on this path that a reading
// session's fragmented heap cannot always satisfy, and taking it per lookup is
// what makes a dictionary "stop finding words until restart": every lookup is a
// fresh 32768-byte contiguity gate, and each one that lands in a hole leaves the
// heap slightly worse. Held once per session it is a single gate, passed or
// failed at the point the user opens the dictionary. The parsed chunk table is
// cached here for the same reason — it is identical for every lookup against
// one file, and re-parsing it meant a second vector allocation per word.
class Scratch {
 public:
  Scratch();
  ~Scratch();
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  // Allocate the window and chunk scratch if not already held. Idempotent;
  // false means the heap could not supply them (ExtractError::LowMemory).
  bool ensure();

  // Drop everything, including the cached chunk table. Call when the session
  // ends so a reading session does not carry ~35KB it no longer needs.
  void release();

  bool ready() const { return state != nullptr; }

 private:
  friend bool extractEntry(const char*, uint32_t, uint32_t, uint8_t*, Scratch&, ExtractError*);
  std::unique_ptr<uint8_t[]> window;  // inflate back-reference ring, allocated first (largest)
  std::unique_ptr<ScratchState> state;
};

// Parse the dictzip header/chunk table. On failure, *outError (if provided)
// reports ReadError for a truncated/IO read or Decompress for a malformed file.
bool parse(HalFile& file, Info* info, ExtractError* outError = nullptr);

// Decompress the uncompressed byte range [offset, offset+size) straight into
// dest, which must have room for size bytes. Writing into the caller's buffer
// rather than a temp file is what removes the SD write + FAT update + reopen +
// reread that a definition used to cost.
// On failure, *outError (if provided) reports the specific cause so callers can
// distinguish low memory from IO from a corrupt file.
bool extractEntry(const char* path, uint32_t offset, uint32_t size, uint8_t* dest, Scratch& scratch,
                  ExtractError* outError = nullptr);

}  // namespace DictZip
