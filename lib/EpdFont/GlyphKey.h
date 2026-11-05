// New in the Writing modes step 2 work (deviation 5); ours, not from upstream.
#pragma once
#include <cstdint>

// Writing modes step 2, deviation 5: a key from kBase names a TrueType font's glyph by its ID, past Unicode, so no
// text ever reads as a glyph.
namespace glyph_key {
constexpr uint32_t kBase = 0x110000;
constexpr uint32_t kLast = kBase + 0xFFFF;
constexpr bool is(uint32_t cp) { return cp >= kBase && cp <= kLast; }
constexpr uint32_t of(uint16_t glyph) { return kBase + glyph; }
constexpr uint16_t glyphOf(uint32_t key) { return static_cast<uint16_t>(key - kBase); }
}  // namespace glyph_key
