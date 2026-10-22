#include "HttpRange.h"

#include <cctype>
#include <cstdint>

namespace crosspoint {
namespace http_range {

namespace {

// Reads one decimal number. False when there is no digit or the value does not fit in size_t.
bool parseNumber(const char*& p, size_t& out) {
  if (*p < '0' || *p > '9') return false;
  size_t value = 0;
  while (*p >= '0' && *p <= '9') {
    const size_t digit = static_cast<size_t>(*p - '0');
    if (value > (SIZE_MAX - digit) / 10) return false;
    value = value * 10 + digit;
    ++p;
  }
  out = value;
  return true;
}

void skipSpaces(const char*& p) {
  while (*p == ' ' || *p == '\t') ++p;
}

// ESP-IDF TLSF on the C3: 4-byte alignment, 16 second-level classes per power of two (pools up to
// 256 KB, tlsf.c control_construct), rounding from 64 B up (small_block_size = 1 << (4 + 2)).
constexpr size_t HEAP_ALIGN = 4;
constexpr unsigned TLSF_SL_LOG2 = 4;
constexpr size_t TLSF_SMALL_BLOCK = 64;
// CONFIG_HEAP_POISONING_LIGHT: 8-byte head + 4-byte tail canary per allocation.
constexpr size_t HEAP_POISON_BYTES = 12;

unsigned highestBit(size_t value) {
  unsigned bit = 0;
  while (value >>= 1) ++bit;
  return bit;
}

}  // namespace

std::string rangeHeaderValue(size_t first, size_t last) {
  return "bytes=" + std::to_string(first) + "-" + std::to_string(last);
}

bool parseContentRange(const char* value, ContentRange& out) {
  out = ContentRange{};
  if (value == nullptr) return false;
  const char* p = value;
  skipSpaces(p);
  // The unit, case-insensitively. The comparison stops at the first mismatch, so a short string
  // ends it at its terminator.
  static constexpr char UNIT[] = "bytes";
  for (size_t i = 0; i + 1 < sizeof(UNIT); ++i) {
    if (std::tolower(static_cast<unsigned char>(p[i])) != UNIT[i]) return false;
  }
  p += sizeof(UNIT) - 1;
  if (*p != ' ') return false;
  skipSpaces(p);

  ContentRange range;
  if (*p == '*') {
    ++p;
  } else {
    if (!parseNumber(p, range.first) || *p != '-') return false;
    ++p;
    if (!parseNumber(p, range.last) || range.last < range.first) return false;
    range.satisfied = true;
  }
  if (*p != '/') return false;
  ++p;
  if (*p == '*') {
    if (!range.satisfied) return false;  // "*/*" says nothing at all
    ++p;
  } else {
    if (!parseNumber(p, range.total)) return false;
    range.totalKnown = true;
    if (range.satisfied && range.last >= range.total) return false;
  }
  skipSpaces(p);
  if (*p != '\0') return false;
  out = range;
  return true;
}

RangeReply classifyReply(int status, bool haveContentRange, const ContentRange& range, size_t requestedFirst) {
  switch (status) {
    case 206:
      return haveContentRange && range.satisfied && range.first == requestedFirst ? RangeReply::Partial
                                                                                  : RangeReply::Mismatch;
    case 200:
      return RangeReply::WholeBody;
    case 416:
      return RangeReply::Unsatisfiable;
    default:
      return RangeReply::Error;
  }
}

bool nextChunk(size_t offset, size_t chunkSize, bool totalKnown, size_t total, Chunk& out) {
  if (chunkSize == 0) return false;
  if (totalKnown && offset >= total) return false;
  size_t last = offset > SIZE_MAX - (chunkSize - 1) ? SIZE_MAX : offset + (chunkSize - 1);
  if (totalKnown && last >= total) last = total - 1;
  out.first = offset;
  out.last = last;
  return true;
}

bool transferComplete(size_t offset, bool totalKnown, size_t total) { return totalKnown && offset >= total; }

size_t heapBlockFor(size_t request) {
  size_t size = (request + HEAP_POISON_BYTES + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1);
  if (size >= TLSF_SMALL_BLOCK) {
    const size_t round = static_cast<size_t>(1) << (highestBit(size) - TLSF_SL_LOG2);
    size = (size + round - 1) & ~(round - 1);
  }
  return size;
}

size_t recordBlockFor(size_t chunkSize) {
  return heapBlockFor(chunkSize + RESPONSE_HEADER_ALLOWANCE + RECORD_EXPANSION);
}

size_t chunkBudget(size_t chunkSize) { return recordBlockFor(chunkSize) + WIFI_MARGIN; }

namespace {
size_t ladderIndexFor(size_t largestFreeBlock) {
  for (size_t i = 0; i < CHUNK_LADDER_SIZE; ++i) {
    if (chunkBudget(CHUNK_LADDER[i]) <= largestFreeBlock) return i;
  }
  return CHUNK_LADDER_SIZE - 1;
}
}  // namespace

ChunkSizer::ChunkSizer(size_t ceiling) : ceilingIndex_(CHUNK_LADDER_SIZE - 1) {
  for (size_t i = 0; i < CHUNK_LADDER_SIZE; ++i) {
    if (CHUNK_LADDER[i] <= ceiling) {
      ceilingIndex_ = i;
      break;
    }
  }
  lastIndex_ = ceilingIndex_;
}

size_t ChunkSizer::next(size_t largestFreeBlock) {
  const size_t fits = ladderIndexFor(largestFreeBlock);
  lastIndex_ = fits > ceilingIndex_ ? fits : ceilingIndex_;
  return CHUNK_LADDER[lastIndex_];
}

void ChunkSizer::onOutOfMemory() {
  const size_t below = lastIndex_ + 1 < CHUNK_LADDER_SIZE ? lastIndex_ + 1 : CHUNK_LADDER_SIZE - 1;
  if (below > ceilingIndex_) ceilingIndex_ = below;
}

size_t ChunkSizer::ceiling() const { return CHUNK_LADDER[ceilingIndex_]; }

unsigned maxReconnectsFor(size_t fileBytes) {
  return MAX_RECONNECTS_BASE + static_cast<unsigned>(fileBytes / BYTES_PER_EXTRA_RECONNECT);
}

bool RetryBudget::reconnectAfterFailure(bool madeProgress, size_t fileBytes) {
  stalledFailures_ = madeProgress ? 0 : stalledFailures_ + 1;
  if (stalledFailures_ > MAX_STALLED_RETRIES || reconnects_ >= maxReconnectsFor(fileBytes)) return false;
  ++reconnects_;
  return true;
}

bool shouldChunk(size_t largestFreeBlock) { return largestFreeBlock < STREAM_MIN_LARGEST_BLOCK; }

}  // namespace http_range
}  // namespace crosspoint
