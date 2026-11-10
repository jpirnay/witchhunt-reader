#include "HyphenationPack.h"

#include <cstring>

// The CPHY v1 layout is crosspoint-reader PR #3706's (Uri Tauber); see HyphenationPack.h.
namespace hyphpack {

namespace {

constexpr char kMagic[4] = {'C', 'P', 'H', 'Y'};

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void writeLe32(const uint32_t v, uint8_t* p) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

bool isLowerAscii(const uint8_t c) { return c >= 'a' && c <= 'z'; }

}  // namespace

bool parseHeader(const uint8_t* bytes, Header& out) {
  if (!bytes || std::memcmp(bytes, kMagic, sizeof(kMagic)) != 0 || bytes[4] != kVersion) return false;
  if (!isLowerAscii(bytes[5]) || !isLowerAscii(bytes[6])) return false;
  if (bytes[9] != 0 || bytes[10] != 0 || bytes[11] != 0) return false;  // flags, reserved
  Header h;
  h.code[0] = static_cast<char>(bytes[5]);
  h.code[1] = static_cast<char>(bytes[6]);
  h.minPrefix = bytes[7];
  h.minSuffix = bytes[8];
  h.rootOffset = readLe32(bytes + 12);
  h.payloadSize = readLe32(bytes + 16);
  h.crc32 = readLe32(bytes + 20);
  if (h.payloadSize == 0 || h.payloadSize > kMaxPayloadBytes || h.rootOffset >= h.payloadSize) return false;
  out = h;
  return true;
}

void writeHeader(const Header& h, uint8_t* out) {
  std::memcpy(out, kMagic, sizeof(kMagic));
  out[4] = kVersion;
  out[5] = static_cast<uint8_t>(h.code[0]);
  out[6] = static_cast<uint8_t>(h.code[1]);
  out[7] = h.minPrefix;
  out[8] = h.minSuffix;
  out[9] = 0;
  out[10] = 0;
  out[11] = 0;
  writeLe32(h.rootOffset, out + 12);
  writeLe32(h.payloadSize, out + 16);
  writeLe32(h.crc32, out + 20);
}

}  // namespace hyphpack
