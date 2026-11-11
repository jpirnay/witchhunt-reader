#pragma once
// ESP-IDF's address test, for EspPartitionFlash built as on a board with PSRAM (CONFIG_SPIRAM): on the host every
// buffer is internal RAM, so a program never goes through the PSRAM bounce buffer.

inline bool esp_ptr_internal(const void*) { return true; }
