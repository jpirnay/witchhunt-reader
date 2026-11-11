#pragma once

#include <esp_partition.h>
#include <sdkconfig.h>

#include "NorFlash.h"

/// The flash area on the `spiffs` partition: backend (a) of FlashArea (docs/design/flash-area.md), over the whole
/// partition, and the only code that calls the ESP-IDF partition API for it. Offsets are the partition's own, as every
/// consumer has always addressed it.
///
/// It keeps the handle of every consumer's mapping, so a consumer releases one by its pointer. Not thread-safe: the
/// consumers serialise among themselves, as they did on the partition.
class EspPartitionFlash final : public NorFlash {
 public:
  /// Finds the partition. False when the partition table has none.
  bool begin();

  uint32_t size() const override;
  bool read(uint32_t offset, void* dst, uint32_t len) const override;
  bool program(uint32_t offset, const void* src, uint32_t len) override;
  bool erase(uint32_t offset, uint32_t len) override;
  const uint8_t* map(uint32_t offset, uint32_t len) override;
  void unmap(const uint8_t* mapped) override;
  uint32_t freeMapPages() const override;

 private:
  // Every consumer's mappings at once: a .cpfont, the language slot, the glyph store's three (its index and log, its
  // SD-mode blocks, a style), the hyphenation store's two, and a spare.
  static constexpr int kMaxMaps = 8;

  struct Mapping {
    const uint8_t* ptr = nullptr;
    esp_partition_mmap_handle_t handle = 0;
  };

  bool inRange(const uint32_t offset, const uint32_t len) const {
    return part_ != nullptr && offset <= part_->size && len <= part_->size - offset;
  }
  bool write(uint32_t offset, const void* src, uint32_t len);

  const esp_partition_t* part_ = nullptr;
  Mapping maps_[kMaxMaps];
#if CONFIG_SPIRAM
  // ESP-IDF writes from external RAM 32 bytes at a time, each a whole flash operation; such sources are
  // copied through this internal buffer instead.
  uint8_t bounce_[512];
#endif
};
