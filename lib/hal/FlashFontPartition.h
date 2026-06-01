#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_partition.h>

// Raw flash partition font cache.
//
// The unused `spiffs` partition (3.4 MB at 0xc90000) stores a .cpfont file
// so font metadata can be mmap'd directly from flash — zero SRAM cost for
// fullIntervals / kern / ligature tables during the draw phase.
//
// Write path (on font selection):
//   FlashFontPartition::write(sdPath)   → erase + write .cpfont from SD
//   FlashFontPartition::mmap(&ptr, &sz) → map partition, return const pointer
//
// Read path (on boot if partition is valid):
//   FlashFontPartition::mmap(&ptr, &sz) → map partition, return const pointer
//   SdCardFont::loadFromMmap(ptr, sz)   → parse header / intervals from flash
//
// unmap() must be called before write() if already mapped.
// The mmap handle is stored internally; call unmap() on font change.

namespace FlashFontPartition {

// Write the .cpfont file at `sdPath` to the flash partition.
// Erases and rewrites the partition in 4 KB sector blocks.
// Returns true on success.
// Pre-condition: partition must not be currently mmap'd — call unmap() first.
bool write(const char* sdPath);

// Memory-map the partition and return a pointer + byte size.
// `outPtr` receives the mmap'd base address; `outSize` receives the mapped size.
// Returns true on success. The map is valid until unmap() is called.
// Calling mmap() twice without unmap() is an error (returns false).
bool mmap(const uint8_t** outPtr, size_t* outSize);

// Release the current mmap. No-op if not mapped.
void unmap();

// Returns true if the partition currently contains a valid .cpfont file.
// Checks magic bytes and version only (fast).
bool hasValidFont();

// Returns true if the partition is currently mmap'd.
bool isMapped();

}  // namespace FlashFontPartition
