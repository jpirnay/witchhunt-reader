#pragma once

#include <cstdint>

// The tone every reader picture takes on its way to a panel's own gray canvas (ImageBlock::renderGray8), and so what
// its gray cache holds.
namespace picture_tone {

// v' = 255 * (v / 255)^0.28, rounded. The driver aims its levels at equal steps of drive, not of reflectance, and a
// picture comes out far too dark without this; the exponent was chosen by eye on the T5S3 over 0.40 and 0.55 on real
// photos. Pictures only: text, rules and the sleep screen take none.
inline constexpr uint8_t kTable[256] = {
    0,   54,  66,  74,  80,  85,  89,  93,  97,  100, 103, 106, 108, 111, 113, 115, 117, 119, 121, 123, 125, 127,
    128, 130, 132, 133, 135, 136, 137, 139, 140, 141, 143, 144, 145, 146, 147, 149, 150, 151, 152, 153, 154, 155,
    156, 157, 158, 159, 160, 161, 162, 162, 163, 164, 165, 166, 167, 168, 168, 169, 170, 171, 172, 172, 173, 174,
    175, 175, 176, 177, 178, 178, 179, 180, 180, 181, 182, 182, 183, 184, 184, 185, 186, 186, 187, 187, 188, 189,
    189, 190, 191, 191, 192, 192, 193, 193, 194, 195, 195, 196, 196, 197, 197, 198, 198, 199, 199, 200, 200, 201,
    202, 202, 203, 203, 204, 204, 205, 205, 206, 206, 206, 207, 207, 208, 208, 209, 209, 210, 210, 211, 211, 212,
    212, 213, 213, 213, 214, 214, 215, 215, 216, 216, 216, 217, 217, 218, 218, 219, 219, 219, 220, 220, 221, 221,
    221, 222, 222, 223, 223, 223, 224, 224, 225, 225, 225, 226, 226, 227, 227, 227, 228, 228, 228, 229, 229, 229,
    230, 230, 231, 231, 231, 232, 232, 232, 233, 233, 233, 234, 234, 234, 235, 235, 236, 236, 236, 237, 237, 237,
    238, 238, 238, 239, 239, 239, 240, 240, 240, 241, 241, 241, 242, 242, 242, 242, 243, 243, 243, 244, 244, 244,
    245, 245, 245, 246, 246, 246, 247, 247, 247, 247, 248, 248, 248, 249, 249, 249, 250, 250, 250, 250, 251, 251,
    251, 252, 252, 252, 252, 253, 253, 253, 254, 254, 254, 254, 255, 255,
};

// FNV-1a over the table, stamped into every gray cache: a cache made through another tone is never read.
constexpr uint32_t fingerprint(const uint8_t (&table)[256]) {
  uint32_t hash = 2166136261u;
  for (const uint8_t v : table) hash = (hash ^ v) * 16777619u;
  return hash;
}
inline constexpr uint32_t kFingerprint = fingerprint(kTable);

}  // namespace picture_tone
