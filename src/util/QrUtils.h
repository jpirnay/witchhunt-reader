#pragma once

#include <GfxRenderer.h>

#include <string>

#include "components/themes/BaseTheme.h"

namespace QrUtils {

// Maximum QR capacity in bytes (Version 40, ECC_LOW, byte mode).
static constexpr size_t MAX_QR_CAPACITY = 2953;

// Renders a QR code with the given text payload within the specified bounding box.
// Returns true if the payload was truncated to fit the QR capacity.
bool drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload);

// True on a screen too small for a QR page's usual layout, whose header, sub-header and spacing
// alone leave no room for a code. Asked of the panel rather than the board: Murphy's 240 px short
// side is the only one today, and the next small panel should not need its own branch.
bool compactLayout(const GfxRenderer& renderer);
// The gap between the lines of a compact QR page.
int compactSpacing();

}  // namespace QrUtils
