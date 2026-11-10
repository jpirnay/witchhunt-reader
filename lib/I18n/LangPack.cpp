#include "LangPack.h"

#include <InflateReader.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

namespace langpack {

namespace {

constexpr char MAGIC[4] = {'C', 'P', 'L', 'G'};
// No pack comes near it (Russian, the largest, inflates to ~31 KB); a header that says more is not one.
constexpr uint32_t MAX_SECTION_BYTES = 1024 * 1024;

// CRC-32 as zlib computes it, chained from 0: what gen_i18n.py writes. ROM code on the device.
uint32_t crc32(const uint32_t crc, const void* data, const size_t len) {
  return esp_rom_crc32_le(crc, static_cast<const uint8_t*>(data), static_cast<uint32_t>(len));
}

uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
void put16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, const uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// The inflate's state and where its input comes from. The reader comes first: the uzlib callback gets
// the reader's state, at offset 0, and casts it back to this (InflateReader.h).
struct InflateCtx {
  InflateReader reader;
  const ByteSource* src = nullptr;
  uint8_t* buf = nullptr;
  uint32_t remaining = 0;
  uint32_t crc = 0;
  bool failed = false;
};

int readCallback(uzlib_uncomp* uncomp) {
  auto* ctx = reinterpret_cast<InflateCtx*>(uncomp);
  if (ctx->remaining == 0) return -1;
  const size_t want = std::min<size_t>(ctx->remaining, READ_BYTES);
  const int got = ctx->src->read(ctx->src->ctx, ctx->buf, want);
  if (got <= 0) {
    // A section shorter than its header says, or a read error: never "data", whatever uzlib makes of -1.
    ctx->failed = true;
    return -1;
  }
  ctx->remaining -= static_cast<uint32_t>(got);
  ctx->crc = crc32(ctx->crc, ctx->buf, static_cast<size_t>(got));
  uncomp->source = ctx->buf + 1;
  uncomp->source_limit = ctx->buf + got;
  return ctx->buf[0];
}

}  // namespace

bool parseHeader(const uint8_t* bytes, const size_t len, Header& out) {
  if (!bytes || len < HEADER_BYTES || memcmp(bytes, MAGIC, 4) != 0 || bytes[4] != FORMAT_VERSION) return false;
  Header h;
  h.entries = get16(bytes + 6);
  memcpy(h.code, bytes + 8, CODE_BYTES);
  h.payloadCrc = get32(bytes + 12);
  h.payloadLen = get32(bytes + 16);
  h.rawLen = get32(bytes + 20);
  h.extLen = get32(bytes + 24);
  h.extRawLen = get32(bytes + 28);
  h.extCrc = get32(bytes + 32);
  memcpy(h.name, bytes + 36, NAME_BYTES);
  // Two or three lower-case letters, NUL-padded.
  size_t codeLen = 0;
  while (codeLen < CODE_BYTES && h.code[codeLen] >= 'a' && h.code[codeLen] <= 'z') ++codeLen;
  if (codeLen < 2 || codeLen > 3) return false;
  for (size_t i = codeLen; i < CODE_BYTES; ++i) {
    if (h.code[i] != '\0') return false;
  }
  if (h.payloadLen == 0 || h.rawLen == 0 || h.payloadLen > MAX_SECTION_BYTES || h.rawLen > MAX_SECTION_BYTES ||
      h.extLen > MAX_SECTION_BYTES || h.extRawLen > MAX_SECTION_BYTES || (h.extLen == 0) != (h.extRawLen == 0)) {
    return false;
  }
  out = h;
  return true;
}

void encodeHeader(const Header& h, uint8_t (&out)[HEADER_BYTES]) {
  memset(out, 0, sizeof(out));
  memcpy(out, MAGIC, 4);
  out[4] = FORMAT_VERSION;
  put16(out + 6, h.entries);
  memcpy(out + 8, h.code, std::min(strlen(h.code), CODE_BYTES));
  put32(out + 12, h.payloadCrc);
  put32(out + 16, h.payloadLen);
  put32(out + 20, h.rawLen);
  put32(out + 24, h.extLen);
  put32(out + 28, h.extRawLen);
  put32(out + 32, h.extCrc);
  memcpy(out + 36, h.name, std::min(strlen(h.name), NAME_BYTES - 1));
}

uint32_t slotStamp(const Header& h, const uint32_t keyLayoutStamp) {
  // A CRC over the three, so no two of them cancel: a pack and a firmware whose differences happen to
  // line up must not read as the slot's.
  uint8_t bytes[16];
  put32(bytes, h.payloadCrc);
  put32(bytes + 4, h.extCrc);
  put32(bytes + 8, h.extRawLen);
  put32(bytes + 12, keyLayoutStamp);
  return crc32(0, bytes, sizeof(bytes));
}

uint16_t findKey(const uint32_t hash, const uint32_t* hashes, const uint16_t* byHash, const uint16_t count) {
  size_t lo = 0;
  size_t hi = count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const uint32_t at = hashes[byHash[mid]];
    if (at == hash) return byHash[mid];
    if (at < hash) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return NO_KEY;
}

bool inflateSection(const ByteSource& src, const uint32_t len, const uint32_t rawLen, const uint32_t crc, uint8_t* ring,
                    uint8_t* io, const ByteSink& sink) {
  if (!src.read || !sink.write || !ring || !io || len == 0) return false;
  // The inflate's state is ~1.3 KB of trees: on the heap for the call, not on the caller's stack.
  const std::unique_ptr<InflateCtx> ctx(new (std::nothrow) InflateCtx());
  if (!ctx || !ctx->reader.initWithExternalRing(ring, WINDOW_BYTES)) return false;
  ctx->src = &src;
  ctx->buf = io;
  ctx->remaining = len;
  ctx->reader.setReadCallback(readCallback);
  uint8_t* out = io + READ_BYTES;

  uint32_t produced = 0;
  for (;;) {
    size_t got = 0;
    const InflateStatus st = ctx->reader.readAtMost(out, OUT_BYTES, &got);
    if (st == InflateStatus::Error || ctx->failed) return false;
    if (got > 0) {
      if (produced + got > rawLen) return false;
      produced += static_cast<uint32_t>(got);
      if (!sink.write(sink.ctx, out, got)) return false;
    }
    if (st == InflateStatus::Done) break;
    if (got == 0) return false;  // no progress and no end: a stream that stalls is not a pack's
  }
  // The CRC is over the section as stored, so whatever the inflate did not need is read too.
  while (ctx->remaining > 0) {
    const size_t want = std::min<size_t>(ctx->remaining, READ_BYTES);
    const int got = src.read(src.ctx, io, want);
    if (got <= 0) return false;
    ctx->remaining -= static_cast<uint32_t>(got);
    ctx->crc = crc32(ctx->crc, io, static_cast<size_t>(got));
  }
  return produced == rawLen && ctx->crc == crc;
}

bool EntryParser::feed(const uint8_t* data, const size_t len) {
  size_t i = 0;
  while (i < len) {
    if (hashBytes_ < 4) {
      hash_ |= static_cast<uint32_t>(data[i++]) << (8 * hashBytes_);
      if (++hashBytes_ == 4 && !sink_.begin(hash_)) return false;
      continue;
    }
    const auto* start = data + i;
    const auto* nul = static_cast<const uint8_t*>(memchr(start, 0, len - i));
    const size_t n = nul ? static_cast<size_t>(nul - start) : len - i;
    if (n > 0 && !sink_.bytes(reinterpret_cast<const char*>(start), n)) return false;
    i += n;
    if (nul) {
      ++i;
      if (!sink_.end()) return false;
      ++entries_;
      hash_ = 0;
      hashBytes_ = 0;
    }
  }
  return true;
}

}  // namespace langpack
