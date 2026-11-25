#pragma once

// witchhunt: deviation 21 - OT_HOT links a function into IRAM on the ESP32-C3. The C3 runs code and reads flash data
// through one 16 KB cache, and shaping is bound by fetching its own instructions: with OT_HOT on the 8.7 KB of the
// engine that executes the most instructions per byte (ranked by emulating the firmware's RV32 build over the Hindi and
// Arabic bench chapters: the table reads, digest tests, the lookup loop, the skipping iterator, coverage and class
// lookups), the X3 builds an Arabic chapter 24% and a Hindi one 22% faster (docs/text-shaping.md). IRAM is carved from
// the heap's SRAM on the C3, so it costs as much heap. The ESP32-S3 links it into IRAM too: its instruction cache is
// also 16 KB (8 ways of 32 B lines; data has a 32 KB cache of its own), filled from the same 80 MHz DIO flash on the
// Murphy and the X4 Pro, while the core runs at 240 MHz rather than 160, and its internal RAM is not the heap that
// binds (allocations over 4 KB go to PSRAM). Other targets and the host leave it empty.
#if defined(ESP_PLATFORM)
#include <sdkconfig.h>
#endif

#if defined(ESP_PLATFORM) && (defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3))
#include <esp_attr.h>
#define OT_HOT IRAM_ATTR
#else
#define OT_HOT
#endif

// OT_HOT for a header function the compiler emits out of line in several files, a template member such as
// FixedVector::push_back. Each copy is a COMDAT function. On the S3 its literal pool goes into a .literal.<name>
// section, which the IDF links into flash after the IRAM code that reads it, and the link fails ("dangerous relocation:
// l32r: literal placed after use"). It linked while -fipa-sra turned every copy into a local .isra clone; the build
// turns that off (-fno-ipa-sra, platformio.ini). The C3 has no literal pools and keeps such a function in IRAM; the S3
// runs it from flash.
#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32C3)
#define OT_HOT_HEADER IRAM_ATTR
#else
#define OT_HOT_HEADER
#endif
