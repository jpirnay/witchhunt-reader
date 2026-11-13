#pragma once

#include <cstddef>
#include <cstdint>

namespace FsHelpers {

// CRC-32 as zlib computes it (IEEE, reflected, 0xEDB88320), bit by bit: the card's slot files check their records
// with it (ReadingStatsSlotFile, RecentBooksFile), a few hundred bytes at a time, so no 1 KB table. `crc` carries a
// run on: crc32(b, n, crc32(a, m)) is the CRC of a then b.
inline uint32_t crc32(const void* data, const size_t size, uint32_t crc = 0) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

}  // namespace FsHelpers
