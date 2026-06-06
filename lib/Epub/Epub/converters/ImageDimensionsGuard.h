#pragma once

#include <cstdint>

inline bool imageDimensionsWouldOverflow(int width, int height, int maxPixels) {
  if (width <= 0 || height <= 0) {
    return true;
  }

  const int64_t area = static_cast<int64_t>(width) * static_cast<int64_t>(height);
  return area > static_cast<int64_t>(maxPixels);
}
