#pragma once

#include <cstddef>
#include <cstdint>

// Flash a firmware image from an SD-card path into the next OTA app
// partition, then switch otadata so the X3/X4 stock bootloader picks it up
// on next boot. Mirrors the web flasher: raw esp_partition_erase_range +
// esp_partition_write + ota_boot::switchTo (no Arduino Update class, no
// esp_image_verify — those reject our patched image on X4 silicon).
//
// Both the SD update activity and the OTA path land here. OTA first
// downloads the firmware to an SD-card cache file, then calls this.
//
// The wrong-device guards (BAD_CHIP / WRONG_BOARD and runningPartitionChipId)
// are ported from crosspoint-reader/crosspoint-reader (MIT), PR #2880 by Uri
// Tauber (chip_id) and PR #2983 by Justin Mitchell (board tag).

namespace firmware_flash {

enum class Result {
  OK,
  OPEN_FAIL,
  TOO_SMALL,
  TOO_LARGE,
  BAD_MAGIC,
  BAD_CHIP,      // image chip_id doesn't match the running MCU family
  WRONG_BOARD,   // image carries a board tag naming a different board
  BAD_SEGMENTS,  // segment table malformed or runs past EOF
  BAD_CHECKSUM,  // ESP image XOR checksum mismatch
  BAD_SHA,       // SHA256 trailer mismatch (hash_appended images)
  BAD_SIZE,      // body+pad+sha length doesn't match file size
  NO_PARTITION,
  OOM,
  READ_FAIL,
  ERASE_FAIL,
  WRITE_FAIL,
  OTADATA_FAIL,
};

// Progress callback: called after every chunk write. `written`/`total` are bytes.
using ProgressCb = void (*)(size_t written, size_t total, void* ctx);

// Open `sdPath`, validate it looks like an ESP32 image, then stream it into the
// next OTA app partition with interleaved 64 KiB erase + sector writes. On
// success switches otadata via ota_boot::switchTo. Caller is responsible for
// ESP.restart() afterwards.
//
// `alreadyValidated` lets callers that have just run `validateImageFile()`
// themselves (e.g. SdFirmwareUpdateActivity, which validates before showing
// the user the confirmation prompt) skip the redundant second pass. Defaults
// to false so callers without prior validation keep the defense-in-depth check.
Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx, bool alreadyValidated = false);

// Full-image integrity check that mirrors the bootloader's verification:
// header magic, segment table walk, XOR checksum, and SHA256 trailer (when
// hash_appended == 1). Run this before flashing a candidate firmware so a
// truncated/corrupted .bin never reaches otadata.
//
// `partitionSize` is the size of the destination OTA partition; pass 0 to
// skip the size-fits-partition check. Streams the file in CHUNK-sized reads.
//
// Also rejects an image built for another device: chip_id against the running
// partition's own (BAD_CHIP), and the embedded board tag against this build's
// (WRONG_BOARD). See FirmwareBoardTag.h — an untagged image still passes.
Result validateImageFile(const char* sdPath, size_t partitionSize);

// Returns the chip_id (esp_image_header_t offset 12) of the currently-running
// app partition, or 0xFFFF if it cannot be read. The running image booted
// successfully, so its chip_id is authoritative for the current CPU — no chip
// enumeration needed. Cached after the first call.
uint16_t runningPartitionChipId();

const char* resultName(Result r);

}  // namespace firmware_flash
