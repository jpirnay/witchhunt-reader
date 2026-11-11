#pragma once
// HalCapabilities, for the test of how FlashArea picks its backend: only the board's answer to whether the PSRAM image
// is kept on the card, which the real header takes from the SDK's board profile.

namespace HalCapabilities {
inline bool g_flashAreaOnCard = false;
inline bool flashAreaOnCard() { return g_flashAreaOnCard; }
}  // namespace HalCapabilities
