#pragma once

// Host-test shim for the SDK's BoardConfig.h.
//
// The real header pulls Arduino.h and ESP-IDF drivers, which don't build on the
// host. Everything the code under test needs from it is the normalization of
// the -DFREEINK_DEVICE_* selection flags to 0/1, so that is all this provides.
// Keep in sync with the "Normalize device flags" block of
// freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h.

#ifndef FREEINK_DEVICE_X3
#define FREEINK_DEVICE_X3 0
#endif
#ifndef FREEINK_DEVICE_X4
#define FREEINK_DEVICE_X4 0
#endif
#ifndef FREEINK_DEVICE_X4PRO
#define FREEINK_DEVICE_X4PRO 0
#endif
#ifndef FREEINK_DEVICE_X4CLASSIC
#define FREEINK_DEVICE_X4CLASSIC 0
#endif
#ifndef FREEINK_DEVICE_LILYGO
#define FREEINK_DEVICE_LILYGO 0
#endif
#ifndef FREEINK_DEVICE_PAPERMONO
#define FREEINK_DEVICE_PAPERMONO 0
#endif
#ifndef FREEINK_DEVICE_STICKY
#define FREEINK_DEVICE_STICKY 0
#endif
#ifndef FREEINK_DEVICE_M5PAPER
#define FREEINK_DEVICE_M5PAPER 0
#endif
#ifndef FREEINK_DEVICE_M5
#define FREEINK_DEVICE_M5 0
#endif
#ifndef FREEINK_DEVICE_MURPHY
#define FREEINK_DEVICE_MURPHY 0
#endif
#ifndef FREEINK_DEVICE_DELINK
#define FREEINK_DEVICE_DELINK 0
#endif
