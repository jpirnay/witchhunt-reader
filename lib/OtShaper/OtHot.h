#pragma once

// exp/shaper-iram (an experiment, not for merge): the shaper's hot code linked into IRAM, to learn whether the X3's
// shaping time is instruction fetch through the 16 KB cache that code and flash data share. OT_IRAM=1 places the small
// tier (OT_HOT: the lookup engine, table reads, digests, the pipeline driver), OT_IRAM=2 the large tier too (OT_WARM:
// normalization, the shapers' pauses, cmap, the reader's shaping glue). On the C3 IRAM is carved from the same SRAM as
// the heap. Off the device, or without OT_IRAM, both are empty.
#if defined(ESP_PLATFORM) && defined(OT_IRAM)
#include <esp_attr.h>
#endif

#if defined(ESP_PLATFORM) && defined(OT_IRAM) && OT_IRAM >= 1
#define OT_HOT IRAM_ATTR
#else
#define OT_HOT
#endif

#if defined(ESP_PLATFORM) && defined(OT_IRAM) && OT_IRAM >= 2
#define OT_WARM IRAM_ATTR
#else
#define OT_WARM
#endif
