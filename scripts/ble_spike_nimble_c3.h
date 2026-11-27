// SPIKE ONLY: size NimBLE-Arduino's host like upstream CrossPoint's tuned C3 core (#3785's [ble_page_turner]
// block), without rebuilding the Arduino core. NimBLE-Arduino's nimconfig.h keeps any CONFIG_BT_NIMBLE_* that
// sdkconfig.h already defines, and the prebuilt one defines 3 connections, 24 ACL buffers and the peripheral role, so
// these are redefined after it. Included on the command line (-include). The controller's own settings are left at
// the prebuilt defaults (CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY unset: the ROM-assisted controller).
#pragma once
#include <sdkconfig.h>

// The legacy aliases go too: nimconfig_rename.h turns a defined CONFIG_NIMBLE_ROLE_X with no CONFIG_BT_NIMBLE_ROLE_X
// into an empty CONFIG_BT_NIMBLE_ROLE_X, and every `#if` on it then fails to parse.
#undef CONFIG_BT_NIMBLE_ROLE_PERIPHERAL
#undef CONFIG_NIMBLE_ROLE_PERIPHERAL
#define CONFIG_BT_NIMBLE_ROLE_PERIPHERAL_DISABLED 1
#undef CONFIG_BT_NIMBLE_ROLE_BROADCASTER
#undef CONFIG_NIMBLE_ROLE_BROADCASTER
#define CONFIG_BT_NIMBLE_ROLE_BROADCASTER_DISABLED 1
#undef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 1
#undef CONFIG_BT_NIMBLE_MAX_BONDS
#define CONFIG_BT_NIMBLE_MAX_BONDS 4
#undef CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU
#define CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU 23
#undef CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE
#define CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE 4096
#undef CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT
#define CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT 12
#undef CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT
#define CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT 12
#undef CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT
#define CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT 8
