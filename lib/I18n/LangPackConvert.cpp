#include "LangPackConvert.h"

#include <algorithm>
#include <cstring>

namespace langpack {

namespace {
// Bit 15 of an old table entry: the key read as English, the low bits its offset in English.
constexpr uint16_t OLD_ENGLISH = 0x8000;
}  // namespace

bool oldSlotSound(const OldSlot& s) {
  if (!s.table || !s.data || s.keyCount == 0 || s.dataLen == 0) return false;
  uint32_t at = 0;
  int32_t lastEnglish = -1;
  for (uint16_t key = 0; key < s.keyCount; ++key) {
    if (at >= s.dataLen) return false;  // fewer entries than keys
    const uint16_t entry = s.table[key];
    const auto* nul = static_cast<const char*>(memchr(s.data + at, 0, s.dataLen - at));
    if (!nul) return false;  // the last entry runs past the data
    const auto len = static_cast<uint32_t>(nul - (s.data + at));
    if (entry & OLD_ENGLISH) {
      const auto english = static_cast<int32_t>(entry & ~OLD_ENGLISH);
      if (len != 0 || english <= lastEnglish) return false;
      lastEnglish = english;
    } else if (entry != at || len == 0) {
      return false;
    }
    at += len + 1;
  }
  return at == s.dataLen;  // and no more entries than keys
}

PackWriter::PackWriter(const Out& out, uint8_t* block, const size_t blockBytes)
    : out_(out), block_(block), blockBytes_(std::min<size_t>(blockBytes, 65535)) {}

bool PackWriter::out(const void* data, const size_t len) {
  if (failed_ || !out_.write(out_.ctx, data, len)) {
    failed_ = true;
    return false;
  }
  return true;
}

bool PackWriter::begin(const char* code, const char* name) {
  header_ = Header{};
  failed_ = !block_ || blockBytes_ == 0 || !code;
  fill_ = 0;
  if (failed_) return false;
  for (size_t i = 0; i < CODE_BYTES && code[i]; ++i) header_.code[i] = code[i];
  if (name) {
    for (size_t i = 0; i < NAME_BYTES - 1 && name[i]; ++i) header_.name[i] = name[i];
  }
  // Room for the header, written for real by finish(). Zeros until then: a file cut short there is no pack.
  uint8_t zeros[HEADER_BYTES] = {};
  return out(zeros, sizeof(zeros));
}

bool PackWriter::put(const void* data, size_t len) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  while (len > 0) {
    const size_t take = std::min(len, blockBytes_ - fill_);
    memcpy(block_ + fill_, bytes, take);
    fill_ += take;
    bytes += take;
    len -= take;
    header_.rawLen += static_cast<uint32_t>(take);
    if (fill_ == blockBytes_ && !emit(false)) return false;
  }
  return true;
}

// One stored block: BFINAL and BTYPE 00 in a byte of its own, LEN, NLEN, then the bytes. Each block ends
// on a byte boundary, so the next one's header byte starts on one as a stored block's must.
bool PackWriter::emit(const bool final) {
  const auto len = static_cast<uint16_t>(fill_);
  uint8_t head[5] = {static_cast<uint8_t>(final ? 1 : 0), static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8),
                     static_cast<uint8_t>(~len), static_cast<uint8_t>(~len >> 8)};
  if (!out(head, sizeof(head)) || (fill_ > 0 && !out(block_, fill_))) return false;
  header_.payloadCrc = crc32(crc32(header_.payloadCrc, head, sizeof(head)), block_, fill_);
  header_.payloadLen += static_cast<uint32_t>(sizeof(head) + fill_);
  fill_ = 0;
  return true;
}

bool PackWriter::add(const uint32_t hash, const char* value) {
  if (failed_ || !value || header_.entries == 0xFFFF) return false;
  uint8_t h[4];
  put32(h, hash);
  if (!put(h, sizeof(h)) || !put(value, strlen(value) + 1)) return false;
  ++header_.entries;
  return true;
}

bool PackWriter::finish() {
  // The last block, empty when the payload ended on a block's end.
  if (failed_ || !emit(true)) return false;
  uint8_t bytes[HEADER_BYTES];
  encodeHeader(header_, bytes);
  if (!out_.rewind(out_.ctx)) {
    failed_ = true;
    return false;
  }
  return out(bytes, sizeof(bytes));
}

bool convertOldSlot(const OldSlot& s, const uint16_t* ids, const uint32_t* hashes, const uint16_t keyCount,
                    PackWriter& writer) {
  for (uint16_t key = 0; key < s.keyCount; ++key) {
    const uint16_t entry = s.table[key];
    if (entry & OLD_ENGLISH) continue;
    const uint16_t id = ids[key];
    if (id == NO_KEY || id >= keyCount) continue;
    if (!writer.add(hashes[id], s.data + entry)) return false;
  }
  return true;
}

}  // namespace langpack
