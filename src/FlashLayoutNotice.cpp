#include "FlashLayoutNotice.h"

#include <FlashArea.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "CrossPointState.h"

namespace FlashLayoutNotice {

bool due() { return dueFor(FlashArea::asExpected(), FlashArea::partitionBytes(), APP_STATE.flashLayoutNoticeFor); }

void markShown() {
  APP_STATE.flashLayoutNoticeFor = layoutMark(FlashArea::partitionBytes());
  // Not saved (no card): shown again at the next boot, which is the safe side.
  if (!APP_STATE.saveToFile()) LOG_ERR("FLN", "the notice could not be marked shown on the card");
}

std::string sizeText(const uint32_t bytes) {
  char text[16];
  if (bytes >= 1024u * 1024u) {
    snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else {
    snprintf(text, sizeof(text), "%u KB", static_cast<unsigned>(bytes / 1024));
  }
  return text;
}

std::string whatIsWrong() {
  // "0 KB" when the table has no `spiffs` partition at all: one string, so the Cyrillic packs keep their room under the
  // language slot's 15-bit offsets.
  const std::string have = sizeText(FlashArea::partitionBytes());
  const std::string needed = sizeText(FlashArea::kExpectedBytes);
  const char* format = tr(STR_FLASH_EMERGENCY_WHY);
  const int len = snprintf(nullptr, 0, format, have.c_str(), needed.c_str());
  if (len <= 0) return format;
  std::string text(static_cast<size_t>(len) + 1, '\0');
  snprintf(text.data(), text.size(), format, have.c_str(), needed.c_str());
  text.resize(static_cast<size_t>(len));
  return text;
}

}  // namespace FlashLayoutNotice
