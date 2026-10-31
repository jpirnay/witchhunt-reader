#pragma once

#include <Epub/Page.h>

namespace ReaderPageLayout {

// How far down a page made only of images is moved to sit centred in the viewport; 0 when the page has text, no image,
// or an image as tall as the viewport. 0 on a vertical page too: its elements are placed in the turned frame, where
// the image's height is along the page's width, so a vertical centring offset would push the picture off the page.
// The parser centres a vertical page across its block extent when it emits the page.
inline int imageOnlyPageYOffset(const Page& page, const int viewportHeight) {
  if (viewportHeight <= 0 || page.isVertical()) {
    return 0;
  }
  return page.imageOnlyCentringOffset(viewportHeight);
}

}  // namespace ReaderPageLayout
