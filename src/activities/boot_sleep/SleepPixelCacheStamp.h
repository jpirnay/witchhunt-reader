#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

// The source a sleep PNG's pixel cache (".<image>.png.f<filter>.pxc", SleepActivity.cpp) was decoded from, kept after
// the cache's pixel rows. The cache is named after the image's path and was checked only against its geometry, so a
// new PNG of the same dimensions at the same path -- a picture replaced over USB, WebDAV, the web page or by a plugin
// -- replayed the old picture. crosspoint-reader PR #4542 (Justin Mitchell, @itsthisjustin) fixed the same for its
// converted sleep BMPs by deleting them when the device replaces a source; a stamp also covers a file replaced while
// the card was in a computer.
//
// The stamp is the source's byte size and FAT modify date and time. A file the reader writes with its clock unset is
// dated 1980-01-01 (HalStorage's date callback), so two such PNGs of the same byte size still pass for one another.
//
// After the rows, not in the header: the .pxc header (PixelCache::PXC_MAGIC) is every EPUB image cache's too, and
// ImageBlock reads those at fixed offsets. A cache with no stamp -- written before it existed, or cut off between the
// decode and the stamp -- ends at its rows and never matches.
//
// Host-tested in test/sleep_pixel_cache_stamp.
namespace sleep_pixel_cache {

struct SourceStamp {
  uint32_t size = 0;
  uint16_t date = 0;  // FAT modify date, 0 when the card gives none
  uint16_t time = 0;  // FAT modify time
};

// "SPS2", then size, date and time, little-endian as the chip and the host store them.
//
// The magic's digit is also the version of what a sleep cache holds, the one the .pxc header cannot carry: bump it when
// that changes, and every cache of an earlier one is deleted on its next read and decoded again. 2: Equalize on the X4
// Pro and the Picco had cached the 16-level curve dithered to four levels, which bands (SleepEqualizeBlend.h).
constexpr size_t kBytes = 12;
constexpr char kMagic[4] = {'S', 'P', 'S', '2'};

inline void encode(const SourceStamp& stamp, uint8_t* out) {
  memcpy(out, kMagic, sizeof(kMagic));
  memcpy(out + 4, &stamp.size, sizeof(stamp.size));
  memcpy(out + 8, &stamp.date, sizeof(stamp.date));
  memcpy(out + 10, &stamp.time, sizeof(stamp.time));
}

// The stamp of an open source file.
inline SourceStamp stampOf(HalFile& source) {
  SourceStamp stamp;
  stamp.size = static_cast<uint32_t>(source.size());
  if (!source.getModifyDateTime(&stamp.date, &stamp.time)) {
    stamp.date = 0;
    stamp.time = 0;
  }
  return stamp;
}

// Appends the stamp to a cache the decode has just written: exactly `pixelBytes` long (header and rows). False, with
// nothing written, for a file of any other length.
inline bool append(HalFile& cache, const size_t pixelBytes, const SourceStamp& stamp) {
  if (cache.size() != pixelBytes || !cache.seekSet(pixelBytes)) return false;
  uint8_t bytes[kBytes];
  encode(stamp, bytes);
  return cache.write(bytes, kBytes) == kBytes;
}

// True when `cache` is `pixelBytes` followed by a stamp, and the stamp is `stamp`. Leaves the read position at
// `resumeAt`, where the caller reads on.
inline bool matches(HalFile& cache, const size_t pixelBytes, const SourceStamp& stamp, const size_t resumeAt) {
  if (cache.size() != pixelBytes + kBytes || !cache.seekSet(pixelBytes)) return false;
  uint8_t stored[kBytes];
  if (cache.read(stored, kBytes) != static_cast<int>(kBytes)) return false;
  uint8_t expected[kBytes];
  encode(stamp, expected);
  return memcmp(stored, expected, kBytes) == 0 && cache.seekSet(resumeAt);
}

}  // namespace sleep_pixel_cache
