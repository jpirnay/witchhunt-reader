#pragma once

#include <Epub/Page.h>

#include <algorithm>
#include <cstdint>
#include <memory>

namespace ReaderPageLayout {

// How far down a page made only of images is moved to sit centred in the viewport; 0 when the page has text, no image,
// or an image as tall as the viewport. 0 on a vertical page too: its elements are placed in the turned frame, where
// the image's height is along the page's width, so a vertical centring offset would push the picture off the page.
inline int imageOnlyPageYOffset(const Page& page, const int viewportHeight) {
  if (viewportHeight <= 0 || page.elements.empty() || page.isVertical()) {
    return 0;
  }

  const bool imageOnlyPage = std::all_of(
      page.elements.begin(), page.elements.end(),
      [](const std::unique_ptr<PageElement>& element) { return element && element->getTag() == TAG_PageImage; });
  if (!imageOnlyPage) {
    return 0;
  }

  int16_t imgX, imgY, imgW, imgH;
  if (!page.getImageBoundingBox(imgX, imgY, imgW, imgH) || imgH >= viewportHeight) {
    return 0;
  }

  const int centeredTop = (viewportHeight - imgH) / 2;
  return std::max(0, centeredTop - static_cast<int>(imgY));
}

}  // namespace ReaderPageLayout
