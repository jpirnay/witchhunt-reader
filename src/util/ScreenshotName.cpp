#include "ScreenshotName.h"

#include <cstdio>
#include <string>

#include "StringUtils.h"

namespace screenshot_name {

namespace {

void buildPlain(const unsigned long stamp, char* out, const size_t outSize) {
  snprintf(out, outSize, "/screenshots/screenshot-%lu.bmp", stamp);
}

// Appends to out[0..used); false once the text would not fit.
bool append(char* out, const size_t outSize, size_t& used, const int written) {
  if (written < 0 || static_cast<size_t>(written) >= outSize - used) return false;
  used += static_cast<size_t>(written);
  return true;
}

}  // namespace

void build(const ScreenshotInfo& info, const unsigned long stamp, char* out, const size_t outSize) {
  if (outSize == 0) return;
  if (info.title.empty()) {
    buildPlain(stamp, out, outSize);
    return;
  }
  // Never empty: a title with nothing usable in it becomes "book".
  const std::string title = StringUtils::sanitizeFilename(info.title, kMaxTitleBytes);
  const char* t = title.c_str();

  size_t used = 0;
  bool fits = append(out, outSize, used, snprintf(out, outSize, "/screenshots/%s/%s", t, t));
  if (fits && info.chapter > 0) {
    fits = append(out, outSize, used, snprintf(out + used, outSize - used, "_ch%d", info.chapter));
  }
  if (fits && info.page > 0) {
    fits = append(out, outSize, used, snprintf(out + used, outSize - used, "_p%d", info.page));
  }
  if (fits && info.percent >= 0) {
    const int percent = info.percent > 100 ? 100 : info.percent;
    fits = append(out, outSize, used, snprintf(out + used, outSize - used, "_%dpct", percent));
  }
  if (fits) fits = append(out, outSize, used, snprintf(out + used, outSize - used, "_%lu.bmp", stamp));
  if (!fits) buildPlain(stamp, out, outSize);
}

}  // namespace screenshot_name
