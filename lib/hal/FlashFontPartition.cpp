#include "FlashFontPartition.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_partition.h>

#include <algorithm>
#include <cstring>

namespace FlashFontPartition {

// .cpfont header constants (must match SdCardFont.cpp)
static constexpr char CPFONT_MAGIC[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
static constexpr uint16_t CPFONT_VERSION = 4;
static constexpr size_t CPFONT_HEADER_SIZE = 32;

static constexpr size_t SEC = 4096;   // SPI flash sector size
static constexpr size_t CHUNK = 4096; // SD read / flash write chunk

// Partition layout: the first 4 bytes store the .cpfont file size as a
// little-endian uint32_t (the "size prefix"). The .cpfont data begins at
// offset 4. This lets mmap() read the exact font size on any boot without
// parsing the full header or relying on a runtime-only variable.
static constexpr size_t SIZE_PREFIX = 4;

static esp_partition_mmap_handle_t s_mmapHandle = 0;
static const uint8_t* s_mmapPtr = nullptr;  // points to the size prefix
static size_t s_mapBytes = 0;               // total mapped bytes (prefix + font)

static const esp_partition_t* findPartition() {
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part) {
    LOG_ERR("FFP", "spiffs partition not found");
  }
  return part;
}

bool write(const char* sdPath) {
  if (s_mmapPtr) {
    LOG_ERR("FFP", "write: partition is currently mmap'd — call unmap() first");
    return false;
  }

  const esp_partition_t* part = findPartition();
  if (!part) return false;

  HalFile file;
  if (!Storage.openFileForRead("FFP", sdPath, file) || !file) {
    LOG_ERR("FFP", "write: failed to open %s", sdPath);
    return false;
  }

  const size_t fileSize = file.fileSize();
  if (fileSize == 0) {
    LOG_ERR("FFP", "write: empty file %s", sdPath);
    file.close();
    return false;
  }
  // Partition must fit the 4-byte size prefix + the font data.
  if (fileSize + SIZE_PREFIX > part->size) {
    LOG_ERR("FFP", "write: file %u B + prefix exceeds partition %u B",
            static_cast<unsigned>(fileSize), static_cast<unsigned>(part->size));
    file.close();
    return false;
  }

  LOG_INF("FFP", "Writing %s (%u B) to flash partition spiffs @0x%x", sdPath,
          static_cast<unsigned>(fileSize), static_cast<unsigned>(part->address));

  // Erase enough flash: SIZE_PREFIX + fileSize, rounded up to sector boundary.
  const size_t totalBytes = SIZE_PREFIX + fileSize;
  const size_t eraseTotal = (totalBytes + SEC - 1) & ~(SEC - 1);
  if (esp_partition_erase_range(part, 0, eraseTotal) != ESP_OK) {
    LOG_ERR("FFP", "initial erase failed");
    file.close();
    return false;
  }

  // Write the 4-byte little-endian font file size at partition offset 0.
  const uint32_t fs32 = static_cast<uint32_t>(fileSize);
  const uint8_t prefix[SIZE_PREFIX] = {
      static_cast<uint8_t>(fs32 & 0xFF),
      static_cast<uint8_t>((fs32 >> 8) & 0xFF),
      static_cast<uint8_t>((fs32 >> 16) & 0xFF),
      static_cast<uint8_t>((fs32 >> 24) & 0xFF),
  };
  if (esp_partition_write(part, 0, prefix, SIZE_PREFIX) != ESP_OK) {
    LOG_ERR("FFP", "prefix write failed");
    file.close();
    return false;
  }

  // Write the .cpfont data starting at partition offset SIZE_PREFIX.
  uint8_t buf[CHUNK];
  size_t pos = 0;
  while (pos < fileSize) {
    const size_t want = std::min<size_t>(CHUNK, fileSize - pos);
    const int got = file.read(buf, want);
    if (got <= 0 || static_cast<size_t>(got) != want) {
      LOG_ERR("FFP", "SD read @%u: got=%d want=%u", static_cast<unsigned>(pos), got,
              static_cast<unsigned>(want));
      file.close();
      return false;
    }
    if (esp_partition_write(part, SIZE_PREFIX + pos, buf, want) != ESP_OK) {
      LOG_ERR("FFP", "flash write @%u failed", static_cast<unsigned>(SIZE_PREFIX + pos));
      file.close();
      return false;
    }
    pos += want;
  }

  file.close();
  LOG_INF("FFP", "Write complete (%u B + %u B prefix)", static_cast<unsigned>(pos),
          static_cast<unsigned>(SIZE_PREFIX));
  return true;
}

bool mmap(const uint8_t** outPtr, size_t* outSize) {
  if (s_mmapPtr) {
    LOG_ERR("FFP", "mmap: already mapped — call unmap() first");
    return false;
  }

  const esp_partition_t* part = findPartition();
  if (!part) return false;

  // Read the 4-byte size prefix written by write() to get the exact font size.
  // This is reliable on any boot — no estimation, no header parsing.
  uint8_t prefix[SIZE_PREFIX];
  if (esp_partition_read(part, 0, prefix, SIZE_PREFIX) != ESP_OK) {
    LOG_ERR("FFP", "mmap: failed to read size prefix");
    return false;
  }
  const uint32_t fontSize = static_cast<uint32_t>(prefix[0]) |
                            (static_cast<uint32_t>(prefix[1]) << 8) |
                            (static_cast<uint32_t>(prefix[2]) << 16) |
                            (static_cast<uint32_t>(prefix[3]) << 24);
  if (fontSize == 0 || fontSize > part->size - SIZE_PREFIX) {
    LOG_ERR("FFP", "mmap: invalid size prefix %u (partition size %u)", fontSize,
            static_cast<unsigned>(part->size));
    return false;
  }

  // Map SIZE_PREFIX + fontSize bytes, rounded up to 64 KB (mmap alignment).
  size_t mapBytes = SIZE_PREFIX + fontSize;
  mapBytes = (mapBytes + 0xFFFF) & ~static_cast<size_t>(0xFFFF);
  if (mapBytes > part->size) mapBytes = part->size;

  const void* rawPtr = nullptr;
  if (esp_partition_mmap(part, 0, mapBytes, ESP_PARTITION_MMAP_DATA, &rawPtr, &s_mmapHandle) != ESP_OK) {
    LOG_ERR("FFP", "mmap failed (mapBytes=%u)", static_cast<unsigned>(mapBytes));
    return false;
  }

  s_mmapPtr = static_cast<const uint8_t*>(rawPtr);
  s_mapBytes = mapBytes;

  // Return a pointer to the .cpfont data (past the 4-byte prefix) and the
  // exact font file size. Callers (SdCardFont::loadFromMmap) see clean data.
  *outPtr = s_mmapPtr + SIZE_PREFIX;
  *outSize = fontSize;

  LOG_DBG("FFP", "mmap OK: font ptr=%p size=%u (map=%u)", *outPtr,
          static_cast<unsigned>(fontSize), static_cast<unsigned>(mapBytes));
  return true;
}

void unmap() {
  if (!s_mmapPtr) return;
  esp_partition_munmap(s_mmapHandle);
  s_mmapHandle = 0;
  s_mmapPtr = nullptr;
  s_mapBytes = 0;
  LOG_DBG("FFP", "unmap OK");
}

bool hasValidFont() {
  const esp_partition_t* part = findPartition();
  if (!part) return false;

  // Read size prefix + .cpfont global header together (SIZE_PREFIX + CPFONT_HEADER_SIZE bytes).
  uint8_t buf[SIZE_PREFIX + CPFONT_HEADER_SIZE];
  if (esp_partition_read(part, 0, buf, sizeof(buf)) != ESP_OK) return false;

  // Validate the size prefix is plausible.
  const uint32_t storedSize = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
                              (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24);
  if (storedSize == 0 || storedSize > part->size - SIZE_PREFIX) return false;

  // Validate .cpfont magic and version at offset SIZE_PREFIX.
  const uint8_t* hdr = buf + SIZE_PREFIX;
  if (memcmp(hdr, CPFONT_MAGIC, 8) != 0) return false;
  const uint16_t ver = static_cast<uint16_t>(hdr[8] | (hdr[9] << 8));
  return ver == CPFONT_VERSION;
}

bool isMapped() { return s_mmapPtr != nullptr; }

}  // namespace FlashFontPartition
