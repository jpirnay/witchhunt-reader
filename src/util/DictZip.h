#pragma once

// Ported from crosspoint-reader (PR #2583 by Uri Tauber, OOM fix #2791 by
// William Floyd). Reworked to inflate into a caller buffer with a
// session-held window instead of staging through a temp file.

#include <HalStorage.h>

#include <cstdint>
#include <memory>

class BuildArena;

// Random-access reader for dictzip (.dict.dz) files: gzip with an extra "RA"
// field holding a chunk table, so any byte range can be decompressed without
// inflating the whole file. Format: https://linux.die.net/man/1/dictzip
namespace DictZip {

struct Info {
  uint32_t dataOffset = 0;   // file offset where compressed chunk data starts
  uint32_t totalSize = 0;    // uncompressed size (gzip ISIZE trailer)
  uint16_t chunkLength = 0;  // uncompressed bytes per chunk
  // Cumulative compressed offsets, chunkCount+1 entries: in the arena parse() was given, else in ownedOffsets.
  const uint32_t* chunkOffsets = nullptr;
  uint32_t chunkOffsetCount = 0;
  std::unique_ptr<uint32_t[]> ownedOffsets;
  bool valid = false;
};

// Why an extraction failed, so callers can report the accurate cause.
enum class ExtractError : uint8_t {
  None,        // success
  LowMemory,   // an allocation failed — the ~32KB inflate window or the chunk
               // scratch couldn't be obtained from the fragmented heap (or
               // from the arena a caller lent)
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
//
// Or, in arena mode, all of it comes out of a caller's arena for as long as the
// caller holds it: the word screen lends a lookup the secondary framebuffer
// (docs/memory-allocation-strategy.md §3), so the heap gives nothing at all.
class Scratch {
 public:
  Scratch();
  ~Scratch();
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  // Allocate the window and chunk scratch if not already held. Idempotent;
  // false means the heap could not supply them (ExtractError::LowMemory).
  bool ensure();

  // Arena mode: the window, the chunk scratch and the chunk table of `path` (the
  // .dict.dz the extractions will read, parsed here) are taken from `arena`, in
  // one block above whatever its owner holds. Idempotent while bound to it. False,
  // with nothing left taken from the arena and the scratch empty, when the arena
  // cannot hold them all (*outError LowMemory) or the header does not read or
  // parse: the caller falls back to ensure() and the heap. Releases a heap-mode
  // scratch first. The arena must outlive the binding, which release() ends.
  bool ensure(BuildArena& arena, const char* path, ExtractError* outError = nullptr);

  // Drop everything, including the cached chunk table. Call when the session
  // ends so a reading session does not carry ~35KB it no longer needs. In arena
  // mode this rewinds the arena to where ensure() found it.
  void release();

  bool ready() const { return state != nullptr; }

 private:
  friend bool extractEntry(const char*, uint32_t, uint32_t, uint8_t*, Scratch&, ExtractError*);
  // `path`'s chunk table into the state, unless it holds that one already.
  bool loadInfo(HalFile& file, const char* path, ExtractError* outError);
  void dropInfo();

  uint8_t* window = nullptr;  // inflate back-reference ring, allocated first (largest)
  ScratchState* state = nullptr;
  // Heap mode owns both; arena mode places them in `arena`, in a block whose token the state keeps.
  std::unique_ptr<uint8_t[]> ownedWindow;
  std::unique_ptr<ScratchState> ownedState;
  BuildArena* arena = nullptr;
};

// Parse the dictzip header/chunk table. On failure, *outError (if provided)
// reports ReadError for a truncated/IO read or Decompress for a malformed file,
// and LowMemory when the chunk table found no room. The table goes into
// `tableArena` when given, else onto the heap (Info::ownedOffsets).
bool parse(HalFile& file, Info* info, ExtractError* outError = nullptr, BuildArena* tableArena = nullptr);

// Decompress the uncompressed byte range [offset, offset+size) straight into
// dest, which must have room for size bytes. Writing into the caller's buffer
// rather than a temp file is what removes the SD write + FAT update + reopen +
// reread that a definition used to cost.
// On failure, *outError (if provided) reports the specific cause so callers can
// distinguish low memory from IO from a corrupt file.
bool extractEntry(const char* path, uint32_t offset, uint32_t size, uint8_t* dest, Scratch& scratch,
                  ExtractError* outError = nullptr);

}  // namespace DictZip
