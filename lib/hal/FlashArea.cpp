#include "FlashArea.h"

#include "EspPartitionFlash.h"

// How a board gets its area (docs/design/flash-area.md, "How a board chooses"). FLASH_AREA_PSRAM_IMAGE: unset or 0,
// the `spiffs` partition or none, as every board ships; 1, the PSRAM image when the partition table has no `spiffs`
// partition (a board such as the Onyx Picco); 2, the PSRAM image even when it has one, to try the backend on an S3
// board. FLASH_AREA_PSRAM_BYTES: the image's size, by default the partition's, so every layout constant holds.
#ifndef FLASH_AREA_PSRAM_IMAGE
#define FLASH_AREA_PSRAM_IMAGE 0
#endif

#if FLASH_AREA_PSRAM_IMAGE
#include <Logging.h>
#include <esp_heap_caps.h>

#include "CardImageFile.h"
#include "PsramImageFlash.h"

#ifndef FLASH_AREA_PSRAM_BYTES
#define FLASH_AREA_PSRAM_BYTES 0x360000
#endif
#endif

namespace FlashArea {

namespace {

EspPartitionFlash partitionArea;
NorFlash* chosen = nullptr;
bool picked = false;

#if FLASH_AREA_PSRAM_IMAGE
// Null without PSRAM to hold the image: the chip has none, or not that much free.
NorFlash* psramImage() {
  constexpr uint32_t kBytes = FLASH_AREA_PSRAM_BYTES;
  // The area for the whole run, never freed: every consumer maps into it.
  auto* const image = static_cast<uint8_t*>(heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!image) {
    LOG_ERR("FLA", "no PSRAM for a %u KB flash area image", static_cast<unsigned>(kBytes / 1024));
    return nullptr;
  }
  static CardImageFile file("/.crosspoint/flash-area.img");
  static PsramImageFlash area(image, kBytes, &file);
  if (!area.begin()) {
    heap_caps_free(image);
    return nullptr;
  }
  return &area;
}
#endif

NorFlash* pick() {
#if FLASH_AREA_PSRAM_IMAGE == 2
  if (NorFlash* const image = psramImage()) return image;
#endif
  if (partitionArea.begin()) return &partitionArea;
#if FLASH_AREA_PSRAM_IMAGE == 1
  return psramImage();
#else
  return nullptr;
#endif
}

}  // namespace

NorFlash* get() {
  if (!picked) {
    chosen = pick();
    picked = true;
  }
  return chosen;
}

}  // namespace FlashArea
