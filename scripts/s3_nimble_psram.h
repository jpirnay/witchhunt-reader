// Ported from CrossPoint Reader #3785 (Tuan Q. Nguyen, with Justin Mitchell), scripts/s3_nimble_psram.h; its C61
// profile left out.
//
// Boards with PSRAM (BOARD_HAS_PSRAM): NimBLE's host (mbuf and HCI pools, connection and GATT structures) allocated
// from PSRAM instead of internal RAM. CrossPoint measured on an X4 Pro (13/10/2026) that the stack then takes 46,508 B
// of internal RAM instead of 68,152 B; we measured 46,568 to 46,648 B on the X4 Pro, T5S3 and Picco (2026-11-28,
// src/BleRadioPolicy.h). The controller (libbtdm) and the task stacks stay in internal RAM whatever this says.
//
// Included on the command line (-include, [ble_s3] in platformio.ini), so it lands after the prebuilt sdkconfig.h,
// which sets CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL; the host source is compiled with the firmware, so the change
// takes effect.
#pragma once
#include <sdkconfig.h>

#ifdef BOARD_HAS_PSRAM
#undef CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL
#define CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL 1
#endif
