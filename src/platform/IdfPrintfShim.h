#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdio>

// The prebuilt IDF libraries print through vprintf (liblog, under every ESP_LOGx), printf (heap_caps) and fprintf
// (gpio, vfs, esp_timer, sleep_modes, intr_alloc, esp_mmu_map). Those link newlib's stream formatter, _vfprintf_r,
// beside the _svfprintf_r that every snprintf already uses: the same engine twice, about 11 KB of flash on the X3.
// platformio.ini links the three names with -Wl,--wrap, so their calls land in this file, which formats with
// vsnprintf and writes the text to the same stream. The WiFi blobs' lib_printf already prints this way.
//
// The prebuilt libraries keep error lines only (CONFIG_LOG_MAXIMUM_LEVEL=1), so the calls are rare. A line longer than
// kLineBytes - 1 characters is cut. The buffer is on the caller's stack, any task that logs an IDF error (sys_evt has
// 2 KB); both formatters take a 1,152 B frame, so the line buffer and vsnprintf's ~160 B are what this adds.
namespace idf_printf_shim {

constexpr size_t kLineBytes = 128;

// Formats into a line buffer and writes it to `stream`. Returns the characters written, or vsnprintf's result when
// that is not positive.
int writeFormatted(FILE* stream, const char* format, va_list args);

}  // namespace idf_printf_shim
