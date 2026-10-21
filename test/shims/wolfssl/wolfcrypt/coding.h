#pragma once
// Host stand-in for wolfCrypt's Base64_Decode, which the firmware links for TLS: a plain RFC 4648
// decoder with wolfCrypt's contract (0 on success, *outLen in: room, out: bytes written).

#include <cstddef>
#include <cstdint>

using byte = uint8_t;
using word32 = uint32_t;

inline int Base64_Decode(const byte* in, const word32 inLen, byte* out, word32* outLen) {
  const auto value = [](const byte c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  if (inLen % 4 != 0) return -1;
  word32 written = 0;
  for (word32 i = 0; i < inLen; i += 4) {
    int quad[4];
    int pad = 0;
    for (int j = 0; j < 4; j++) {
      const byte c = in[i + j];
      if (c == '=' && i + 4 == inLen && j >= 2) {
        quad[j] = 0;
        pad++;
      } else {
        if (pad > 0) return -1;
        quad[j] = value(c);
        if (quad[j] < 0) return -1;
      }
    }
    const uint32_t bits = (quad[0] << 18) | (quad[1] << 12) | (quad[2] << 6) | quad[3];
    const int bytes = 3 - pad;
    if (written + bytes > *outLen) return -1;
    for (int j = 0; j < bytes; j++) out[written++] = static_cast<byte>(bits >> (16 - 8 * j));
  }
  *outLen = written;
  return 0;
}
