#pragma once
// Shared image display-dimension computation (docs/parser-stage1-step5-design.md).
//
// Turns an image's INTRINSIC dimensions into on-page DISPLAY dimensions, honoring CSS width/height
// and clamping to the container/viewport while preserving aspect ratio. That math is
// settings-dependent (viewport, container inset, em size), so it is NOT baked into content.bin — it
// is Stage-2 layout. Both LayoutSink's block-image and float-image paths call it.
//
// Pure function: no walk/layout state, no GfxRenderer. Inputs are the intrinsic dims, the image's
// resolved CSS (width/height only — the rest is ignored), and the current viewport/container/em.

#include <cstdint>

#include "Epub/css/CssStyle.h"

namespace compiled {

struct ImageDisplaySize {
  int width = 0;
  int height = 0;
};

// Resolve an image's on-page display size:
//   - both CSS width+height: resolve both, clamp to container/viewport preserving ratio;
//   - CSS height only: derive width from aspect ratio, clamp;
//   - CSS width only: derive height from aspect ratio, clamp;
//   - neither: scale-to-fit the container box, never upscale.
// intrinsicW/H are the image's real pixel dims. containerWidth is viewportWidth minus the
// current block's horizontal inset (or viewportWidth when no inset). emSize is the body font
// ascender size (for em-unit CSS lengths).
ImageDisplaySize computeImageDisplaySize(int intrinsicW, int intrinsicH, const CssStyle& imgStyle, int viewportWidth,
                                         int viewportHeight, int containerWidth, float emSize);

}  // namespace compiled
