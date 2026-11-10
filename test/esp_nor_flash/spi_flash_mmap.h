#pragma once
// The one call EspNorFlash makes of ESP-IDF's MMU API, for its host test: the free data pages, a fixed number here.

typedef enum { SPI_FLASH_MMAP_DATA = 0 } spi_flash_mmap_memory_t;

inline unsigned spi_flash_mmap_get_free_pages(spi_flash_mmap_memory_t) { return 32; }
