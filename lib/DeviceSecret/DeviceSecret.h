#pragma once

// Ported from crosspoint-reader PR #3824 ("Add per-book content key storage and plugin isolation",
// Justin Mitchell). Kept where upstream keeps it (NVS namespace "devid", key "secret"), so a book
// key wrapped under CrossPoint opens here on the same device.

#include <cstdint>

// Random 32-byte per-device secret, created on first use and kept in NVS: it survives SD swaps and
// reformats (only a full flash erase replaces it) and never leaves the device. Feeds plugin device
// IDs and the book-key wrap; each use derives its own value so none reveals another. False when NVS
// is unavailable: an unstable secret is worse than none.
bool deviceSecret(uint8_t (&out)[32]);
