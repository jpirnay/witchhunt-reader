#pragma once

#include <string>

#include "../Activity.h"

class Bitmap;

struct BookOverlayInfo {
  std::string title;
  std::string author;
  std::string progressText;
  std::string chapterName;
  std::string progressSuffix;
};

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, const BookOverlayInfo& overlayInfo,
                               bool topAlignForCoverFit = false) const;
  void renderBlankSleepScreen() const;
  void renderOverlaySleepScreen() const;
  // Quick Resume: leaves the framebuffer (reader page) intact and overlays a small moon icon.
  void renderLastScreenSleepScreen() const;
  BookOverlayInfo getBookOverlayInfo(const std::string& bookPath) const;

  const bool fromTimeout = false;
};
