#include "FlashArea.h"

#include <Logging.h>

#include "EspPartitionFlash.h"
#include "FlashFontPartition.h"

#if __has_include(<sdkconfig.h>)
#include <sdkconfig.h>
#endif

// How a board gets its area (docs/design/flash-area.md, "How a board chooses"). FLASH_AREA_PSRAM_IMAGE: 0, the
// `spiffs` partition or none; 1, the PSRAM image when the partition table has no usable `spiffs` partition (an X4 Pro
// that kept its stock table, a board such as the Onyx Picco), by default on every build with PSRAM; 2, the PSRAM image
// even when there is a partition, to try the backend on an S3 board. FLASH_AREA_PSRAM_BYTES: the image's size, by
// default the partition's, so every layout constant holds. The image is kept on the card as /.crosspoint/flash-area.img
// (CardImageFile): sync() writes what changed, and FlashArea::sync() is called on every planned restart and before
// deep sleep, so the language slot, the font cache and the glyph store outlive them.
#ifndef FLASH_AREA_PSRAM_IMAGE
#if CONFIG_SPIRAM
#define FLASH_AREA_PSRAM_IMAGE 1
#else
#define FLASH_AREA_PSRAM_IMAGE 0
#endif
#endif

#if FLASH_AREA_PSRAM_IMAGE
#include <esp_heap_caps.h>

#include "PsramImageFlash.h"

#ifndef FLASH_AREA_PSRAM_BYTES
#define FLASH_AREA_PSRAM_BYTES 0x360000
#endif
#include "CardImageFile.h"
#endif

namespace FlashArea {

namespace {

EspPartitionFlash partitionArea;
NorFlash* chosen = nullptr;
Backend chosenBackend = Backend::None;
uint32_t foundPartitionBytes = 0;
bool picked = false;

// A partition no larger than the language slot holds none (FlashLangPartition reads such an area as having no slot),
// and nothing else of the area either. The table CrossPoint's web flasher calls the X4 Pro's factory-compatible layout
// has such a one: an 80 KB `spiffs` partition at 0xfd0000 (a retail X4 Pro on Licorice OS has an 80 KB `littlefs` one
// there). 2.40's 64 KB slot fitted into it; the 128 KB slot since 2.41RC does not (issue #733).
bool partitionUsable() {
  if (!partitionArea.begin()) {
    LOG_ERR("FLA",
            "the partition table has no `spiffs` partition: it is another firmware's table (an install that "
            "wrote only the app)");
    return false;
  }
  foundPartitionBytes = partitionArea.size();
  if (partitionArea.size() <= FlashFontPartition::LANG_RESERVED_BYTES) {
    LOG_ERR("FLA", "the `spiffs` partition is %u KB: too small for the flash area (more than %u KB)",
            static_cast<unsigned>(partitionArea.size() / 1024),
            static_cast<unsigned>(FlashFontPartition::LANG_RESERVED_BYTES / 1024));
    return false;
  }
  return true;
}

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
  LOG_INF("FLA", "PSRAM image: %u KB taken, %u KB of PSRAM left (largest block %u KB)",
          static_cast<unsigned>(kBytes / 1024),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
  static CardImageFile file("/.crosspoint/flash-area.img");
  static PsramImageFlash area(image, kBytes, file);
  if (!area.begin()) {
    heap_caps_free(image);
    return nullptr;
  }
  return &area;
}
#endif

NorFlash* pick() {
  // The partition is looked at first in every build, so partitionBytes() says what the table holds.
  const bool usable = partitionUsable();
#if FLASH_AREA_PSRAM_IMAGE == 2
  if (NorFlash* const image = psramImage()) {
    LOG_INF("FLA",
            "the flash area is a PSRAM image: a test build (FLASH_AREA_PSRAM_IMAGE=2) leaves the partition alone");
    chosenBackend = Backend::PsramImage;
    return image;
  }
#endif
  if (usable) {
    chosenBackend = Backend::Partition;
    return &partitionArea;
  }
#if FLASH_AREA_PSRAM_IMAGE == 1
  if (NorFlash* const image = psramImage()) {
    LOG_INF("FLA", "the flash area is a PSRAM image instead, kept on the card as /.crosspoint/flash-area.img");
    chosenBackend = Backend::PsramImage;
    return image;
  }
#endif
  LOG_ERR("FLA",
          "no flash area: the interface stays English, TrueType fonts cannot load, .cpfont fonts are read from "
          "the card. A flasher that writes the partition table gives the reader its `spiffs` partition");
  return nullptr;
}

}  // namespace

NorFlash* get() {
  if (!picked) {
    chosen = pick();
    picked = true;
  }
  return chosen;
}

Backend backend() {
  get();
  return chosenBackend;
}

uint32_t partitionBytes() {
  get();
  return foundPartitionBytes;
}

bool sync() {
  // Before the choice there is nothing to keep, and a sync must not make the choice (it may run on a restart path
  // that comes before the card is mounted).
  if (!picked || !chosen) return true;
  return chosen->sync();
}

}  // namespace FlashArea
