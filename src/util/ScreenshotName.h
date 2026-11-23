#pragma once

#include <cstddef>

#include "ScreenshotInfo.h"

// The path a card screenshot is saved under. Ported from crosspoint-reader PR #1589 (Jon Stieglitz), which named a
// reader's screenshots after the book so a folder of them sorts itself; reworked onto StringUtils::sanitizeFilename
// (the OPDS downloads' rule: titles keep their spaces and their non-ASCII characters, as the card's long names allow).
namespace screenshot_name {

// The most bytes of the title a name takes, cut on a character boundary. The title appears twice (folder and file), so
// with the counters and the extension a path stays under 200 bytes, inside FAT's 255-character limit.
constexpr size_t kMaxTitleBytes = 60;

// "/screenshots/<title>/<title>_ch<C>_p<P>_<N>pct_<stamp>.bmp" for a screen that names a book: a part it does not
// know (chapter 0, page 0, percent below 0) is left out, and the percent is clamped to 0..100. With no title, or no
// room in `out` for the book's name, "/screenshots/screenshot-<stamp>.bmp". `stamp` keeps names unique (millis()).
// `out` is always NUL-terminated when outSize > 0.
void build(const ScreenshotInfo& info, unsigned long stamp, char* out, size_t outSize);

}  // namespace screenshot_name
