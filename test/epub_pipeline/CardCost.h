#pragma once
// What a stretch of the pipeline costs the SD card, from the host test counters: ZIP central-
// directory walks and the entries they read (ZIP_FILE_SCAN_STATS), and the storage shim's file
// calls, opens, bytes read and flushes. Take now() before and after, subtract.
#include <HalStorage.h>
#include <ZipFile.h>

#include <cstddef>
#include <cstdio>

namespace card_cost {

struct Counters {
  size_t walks = 0;
  size_t entries = 0;
  size_t calls = 0;
  size_t opens = 0;
  size_t bytesRead = 0;
  size_t flushes = 0;
};

inline Counters now() {
  return {zipScanStats().walks, zipScanStats().entries, FsFile::calls,
          FsFile::opens,        FsFile::bytesRead,      FsFile::flushes};
}

inline Counters operator-(const Counters& a, const Counters& b) {
  return {a.walks - b.walks, a.entries - b.entries,     a.calls - b.calls,
          a.opens - b.opens, a.bytesRead - b.bytesRead, a.flushes - b.flushes};
}

inline void reset() {
  zipScanStats() = {};
  FsFile::calls = 0;
  FsFile::opens = 0;
  FsFile::bytesRead = 0;
  FsFile::flushes = 0;
}

// On the X3 a card call costs about 17 us (427 s for 25.0 M of them, 2026-10-20).
inline double x3Seconds(const Counters& c) { return static_cast<double>(c.calls) * 17e-6; }

inline void print(const char* label, const char* step, const Counters& c, const double hostMs) {
  std::printf(
      "[ cardcost ] %-10s %-14s walks=%-4zu entriesRead=%-7zu fileCalls=%-8zu (~%.2f s on the X3) bytesRead=%-8zu "
      "opens=%-4zu flushes=%-4zu host=%.1f ms\n",
      label, step, c.walks, c.entries, c.calls, x3Seconds(c), c.bytesRead, c.opens, c.flushes, hostMs);
}

}  // namespace card_cost
