#include "EspPartitionFlash.h"

#include <Logging.h>
#include <spi_flash_mmap.h>

#include <cstring>

#if CONFIG_SPIRAM
#include <esp_memory_utils.h>
#endif

bool EspPartitionFlash::begin() {
  part_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part_) LOG_ERR("FLA", "spiffs partition not found");
  return part_ != nullptr;
}

uint32_t EspPartitionFlash::size() const { return part_ ? static_cast<uint32_t>(part_->size) : 0; }

bool EspPartitionFlash::read(const uint32_t offset, void* dst, const uint32_t len) const {
  if (!inRange(offset, len)) return false;
  return esp_partition_read(part_, offset, dst, len) == ESP_OK;
}

bool EspPartitionFlash::write(const uint32_t offset, const void* src, const uint32_t len) {
  const esp_err_t err = esp_partition_write(part_, offset, src, len);
  if (err != ESP_OK) {
    LOG_ERR("FLA", "program of %u B at %u failed: %d", static_cast<unsigned>(len), static_cast<unsigned>(offset), err);
    return false;
  }
  return true;
}

bool EspPartitionFlash::program(const uint32_t offset, const void* src, const uint32_t len) {
  if (!inRange(offset, len)) return false;
#if CONFIG_SPIRAM
  if (!esp_ptr_internal(src)) {
    const auto* bytes = static_cast<const uint8_t*>(src);
    for (uint32_t done = 0; done < len;) {
      const uint32_t chunk = len - done < sizeof bounce_ ? len - done : static_cast<uint32_t>(sizeof bounce_);
      std::memcpy(bounce_, bytes + done, chunk);
      if (!write(offset + done, bounce_, chunk)) return false;
      done += chunk;
    }
    return true;
  }
#endif
  return write(offset, src, len);
}

bool EspPartitionFlash::erase(const uint32_t offset, const uint32_t len) {
  if (!inRange(offset, len)) return false;
  const esp_err_t err = esp_partition_erase_range(part_, offset, len);
  if (err != ESP_OK) {
    LOG_ERR("FLA", "erase of %u B at %u failed: %d", static_cast<unsigned>(len), static_cast<unsigned>(offset), err);
    return false;
  }
  return true;
}

const uint8_t* EspPartitionFlash::map(const uint32_t offset, const uint32_t len) {
  if (len == 0 || !inRange(offset, len)) return nullptr;
  for (Mapping& m : maps_) {
    if (m.ptr != nullptr) continue;
    // esp_partition_mmap aligns the offset down to its page itself and returns a pointer advanced to `offset`.
    const void* ptr = nullptr;
    const esp_err_t err = esp_partition_mmap(part_, offset, len, ESP_PARTITION_MMAP_DATA, &ptr, &m.handle);
    if (err != ESP_OK) {
      LOG_ERR("FLA", "map of %u B at %u failed: %d (%u pages free)", static_cast<unsigned>(len),
              static_cast<unsigned>(offset), err, static_cast<unsigned>(freeMapPages()));
      m = Mapping{};
      return nullptr;
    }
    m.ptr = static_cast<const uint8_t*>(ptr);
    return m.ptr;
  }
  LOG_ERR("FLA", "map refused: all %d mappings are in use", kMaxMaps);
  return nullptr;
}

void EspPartitionFlash::unmap(const uint8_t* mapped) {
  if (mapped == nullptr) return;
  for (Mapping& m : maps_) {
    if (m.ptr != mapped) continue;
    esp_partition_munmap(m.handle);
    m = Mapping{};
    return;
  }
}

uint32_t EspPartitionFlash::freeMapPages() const {
  return static_cast<uint32_t>(spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA));
}
