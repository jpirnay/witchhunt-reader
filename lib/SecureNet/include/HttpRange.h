#pragma once

// WitchReader SecureNet — HTTP Range helpers for bounded (chunked) downloads.
//
// Why: wolfSSL reads each TLS record into one heap buffer of the record's length, and a server may
// send records of up to 16 KB. On the C3 that is a 17,408 B block (TLSF rounds a 16,401 B request up
// to its size class), which a network session on the X3 does not have once the handshake and the
// Wi-Fi receive buffers have taken their share. A Range request for C bytes bounds the response, and
// with it every record, to C plus the response headers: the server cannot send more than that.
// GitHub's raw CDN, for example, sends 16 KB records once a connection has carried ~56 KB, but an
// 8 KB range arrives as one 8,209 B record (measured with openssl -trace, 2026-10-05).
//
// Pure functions, no Arduino: host-tested in test/http_range.

#include <cstddef>
#include <string>

namespace crosspoint {
namespace http_range {

// --- request / response -------------------------------------------------------------------------

// The value of a Range request header for bytes first..last inclusive: "bytes=first-last".
std::string rangeHeaderValue(size_t first, size_t last);

// A parsed Content-Range response header (RFC 9110 14.4).
struct ContentRange {
  bool satisfied = false;   // "bytes first-last/..."; false for the 416 form "bytes */total"
  size_t first = 0;         // valid when satisfied
  size_t last = 0;          // valid when satisfied, inclusive
  bool totalKnown = false;  // false for ".../*"
  size_t total = 0;         // whole-resource size, valid when totalKnown
};

// Parses "bytes 0-8191/269830", "bytes 0-8191/*" and "bytes */269830" (unit case-insensitive,
// surrounding spaces allowed). False for anything else, including last < first, last >= total and
// numbers that do not fit; out is then left as default.
bool parseContentRange(const char* value, ContentRange& out);

// What a response to a Range request for bytes from requestedFirst means for the download.
enum class RangeReply {
  Partial,        // 206 for exactly the requested start: append the body
  WholeBody,      // 200: the server ignored Range and sends the whole resource from byte 0
  Unsatisfiable,  // 416: nothing at or after requestedFirst
  Mismatch,       // 206 without a usable Content-Range, or for a different start
  Error,          // any other status
};
RangeReply classifyReply(int status, bool haveContentRange, const ContentRange& range, size_t requestedFirst);

// --- chunk planning -----------------------------------------------------------------------------

struct Chunk {
  size_t first = 0;
  size_t last = 0;  // inclusive
  size_t length() const { return last - first + 1; }
};

// The next range to request from offset. False when the total is known and reached. When the
// total is unknown the range is a full chunk, and only a 416 for the next byte ends the transfer.
bool nextChunk(size_t offset, size_t chunkSize, bool totalKnown, size_t total, Chunk& out);

// Whether the transfer is complete at offset. Only a known total can say so: with an unknown one, a
// short or empty 206 proves nothing (a truncated reply looks the same), so the end is confirmed by a
// 416 for the byte after it.
bool transferComplete(size_t offset, bool totalKnown, size_t total);

// --- sizing from the heap -----------------------------------------------------------------------

// Largest first. 6 KB is the start: on the X3 the first 8 KB request of every download failed
// reading its record, while 6 KB ran cleanly (device round 2, 2026-10-05; see ChunkSizer). 3 KB
// fills the gap where 4 KB no longer fits a fragmented heap but 2 KB wastes it.
constexpr size_t CHUNK_LADDER[] = {6144, 4096, 3072, 2048, 1024};
constexpr size_t CHUNK_LADDER_SIZE = sizeof(CHUNK_LADDER) / sizeof(CHUNK_LADDER[0]);
constexpr size_t DEFAULT_CHUNK_BYTES = CHUNK_LADDER[0];
constexpr size_t MIN_CHUNK_BYTES = CHUNK_LADDER[CHUNK_LADDER_SIZE - 1];
constexpr size_t STREAM_MIN_LARGEST_BLOCK = 40 * 1024;
// GitHub's 206 response headers: 919 B on the device (the request trace's hdr=), 968 B by curl.
// Plus ~20 % slack for a longer ETag, request id or date.
constexpr size_t RESPONSE_HEADER_ALLOWANCE = 1152;
// The largest per-record expansion of the suites we negotiate: TLS 1.2 AES-GCM (8-byte explicit
// nonce + 16-byte tag). TLS 1.3 adds 17 (content type + tag).
constexpr size_t RECORD_EXPANSION = 24;
// Two full-MSS Wi-Fi receive buffers (1,664 B heap blocks each), carved out of the largest block
// while the record arrives. Not the whole 4-segment TCP window: other fragments take most of them
// (the X3 kept ~14.5 KB free in all), and the step-down after an out-of-memory read covers the rest.
constexpr size_t WIFI_MARGIN = 2 * 1664;
// Retry a stall (a failure that wrote nothing) this many times in a row; the next one ends it.
constexpr unsigned MAX_STALLED_RETRIES = 3;
// Reconnects per file, whatever progress they make: 20, plus one per 64 KB (a 1 MB font: 35). A
// reconnect (a full handshake, chain check included) costs ~0.7 s on the X3.
constexpr unsigned MAX_RECONNECTS_BASE = 20;
constexpr size_t BYTES_PER_EXTRA_RECONNECT = 64 * 1024;

// The C3 heap block a malloc(request) occupies: ESP-IDF's TLSF rounds every request above 64 B up
// to its second-level class (heap/tlsf mapping_search; 16 classes per power of two on pools up to
// 256 KB), after light heap poisoning has added 12 B. On the S3's PSRAM pool (32 classes) the real
// block is smaller, so this overestimates there, which is the safe side.
size_t heapBlockFor(size_t request);

// The record buffer a response of chunkSize body bytes can make wolfSSL allocate, in the worst
// case of headers and body sharing one record: body + header allowance + AEAD expansion, as a
// heap block.
size_t recordBlockFor(size_t chunkSize);

// What a request of chunkSize needs in the largest free block: its record block plus a Wi-Fi margin
// (Wi-Fi/lwIP receive buffers that may be carved out of the same block while the record arrives).
size_t chunkBudget(size_t chunkSize);

// The most reconnects one file may use: MAX_RECONNECTS_BASE plus one per
// BYTES_PER_EXTRA_RECONNECT of the file.
unsigned maxReconnectsFor(size_t fileBytes);

// Chunk only when memory is tight. Streaming means 16 KB records: a 17,408 B record block plus the
// receive buffers is ~24 KB, needed whole again for every record. 40 KB leaves room for one
// unrelated allocation to split the block between records. The S3 boards (PSRAM in the default
// heap) are always above it; the C3 boards with Wi-Fi up are below it.
bool shouldChunk(size_t largestFreeBlock);

// Chunk size for each request. The heap fragments as a download runs (record buffers, SD writes and
// Wi-Fi buffers interleave): on the X3 the largest block fell from 16 KB to 4.6-6 KB while the free
// total stayed near 14.5 KB, so the size is chosen again before every request from the largest
// block at that moment, never above a ceiling.
//
// The ceiling only comes down. A read that ran out of memory puts it one size below the size that
// failed, for the rest of the session: the caller keeps it across the files of one session, so the
// next file starts at the size known to work. There is no step back up. The largest block measured
// before a request cannot see the Wi-Fi receive buffers that arrive with the response: on the X3 a
// 17,396 B reading let 8 KB through every time, and every 8 KB read then failed (largest 5,876 B),
// costing a reconnect and the lost chunk each cycle. What a step up would win (8 KB instead of
// 6 KB chunks: ~25 % fewer requests of ~1 KB headers each) never paid for that, and for the same
// reason 8 KB is no longer on the ladder at all: every download now starts at 6 KB.
class ChunkSizer {
 public:
  // ceiling: the largest size this download may use, rounded down to a ladder size; e.g. what an
  // earlier file in the same session left.
  explicit ChunkSizer(size_t ceiling = DEFAULT_CHUNK_BYTES);
  // The size for the next request.
  size_t next(size_t largestFreeBlock);
  // The last request ran out of memory reading a record.
  void onOutOfMemory();
  size_t ceiling() const;

 private:
  size_t ceilingIndex_ = 0;  // index into CHUNK_LADDER: no size above it
  size_t lastIndex_ = 0;     // the size next() returned last
};

// Retries for one download. The main guard is the stall count: only failures that wrote nothing
// count, and only while they come in a row; any request that wrote new bytes resets it. A hard cap
// on reconnects bounds the time, scaled to the file so that a large file on a bad heap is not
// abandoned while it is still making progress.
class RetryBudget {
 public:
  // After a failed request that wrote new bytes (madeProgress) or none: whether to reconnect and go
  // on. fileBytes: the file's size when known, else the bytes written so far.
  bool reconnectAfterFailure(bool madeProgress, size_t fileBytes);
  // A request that completed with new bytes written.
  void onProgress() { stalledFailures_ = 0; }
  unsigned reconnects() const { return reconnects_; }

 private:
  unsigned stalledFailures_ = 0;
  unsigned reconnects_ = 0;
};

}  // namespace http_range
}  // namespace crosspoint
