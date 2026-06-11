#include "InflateReader.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace {
constexpr size_t INFLATE_DICT_SIZE = 32768;
}

// Guarantee the cast pattern in the header comment is valid.
static_assert(std::is_standard_layout<InflateReader>::value,
              "InflateReader must be standard-layout for the uzlib callback cast to work");

InflateReader::~InflateReader() { deinit(); }

bool InflateReader::init(const bool streaming, const size_t expectedOutputSize) {
  deinit();  // free any previously allocated ring buffer and reset state

  size_t dictSize = 0;
  if (streaming) {
    // Total output ≤ ring size guarantees no back-reference can outrun the ring, so a
    // known-small stream gets a correspondingly small ring. The 512 B floor guards
    // degenerate sizes (0 = unknown-empty entries) without meaningfully costing heap.
    dictSize = (expectedOutputSize == 0) ? INFLATE_DICT_SIZE
                                         : std::min(INFLATE_DICT_SIZE, std::max<size_t>(expectedOutputSize, 512));
    ringBuffer = static_cast<uint8_t*>(malloc(dictSize));
    if (!ringBuffer) return false;
    memset(ringBuffer, 0, dictSize);
  }

  uzlib_uncompress_init(&decomp, ringBuffer, ringBuffer ? dictSize : 0);
  return true;
}

void InflateReader::deinit() {
  if (ringBuffer) {
    free(ringBuffer);
    ringBuffer = nullptr;
  }
  memset(&decomp, 0, sizeof(decomp));
}

void InflateReader::setSource(const uint8_t* src, size_t len) {
  decomp.source = src;
  decomp.source_limit = src + len;
}

void InflateReader::setReadCallback(int (*cb)(struct uzlib_uncomp*)) { decomp.source_read_cb = cb; }

void InflateReader::skipZlibHeader() {
  uzlib_get_byte(&decomp);
  uzlib_get_byte(&decomp);
}

bool InflateReader::read(uint8_t* dest, size_t len) {
  if (!ringBuffer) {
    // One-shot mode: back-references use absolute offset from dest_start.
    // Valid only when read() is called once with the full output buffer.
    decomp.dest_start = dest;
  }
  decomp.dest = dest;
  decomp.dest_limit = dest + len;

  const int res = uzlib_uncompress(&decomp);
  if (res < 0) return false;
  return decomp.dest == decomp.dest_limit;
}

InflateStatus InflateReader::readAtMost(uint8_t* dest, size_t maxLen, size_t* produced) {
  if (!ringBuffer) {
    // One-shot mode: back-references use absolute offset from dest_start.
    // Valid only when readAtMost() is called once with the full output buffer.
    decomp.dest_start = dest;
  }
  decomp.dest = dest;
  decomp.dest_limit = dest + maxLen;

  const int res = uzlib_uncompress(&decomp);
  *produced = static_cast<size_t>(decomp.dest - dest);

  if (res == TINF_DONE) return InflateStatus::Done;
  if (res < 0) return InflateStatus::Error;
  return InflateStatus::Ok;
}
