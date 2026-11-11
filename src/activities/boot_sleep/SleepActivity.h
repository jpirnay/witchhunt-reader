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
  // renderOverlaySleepScreen() and the Quick Resume path both draw over the frame already in the
  // write buffer rather than clearing it, so anything that repaints just before the transition
  // ends up underneath the sleep cover.
  bool suppressesBusyIndicator() const override { return true; }
  // Sleep screens are out of Dark Mode's reach and show as they always have. Quick Resume is not one: it leaves the
  // page on screen as it is, in the polarity it went out in.
  DarkMode::ScreenPolarity outputPolarity() const override {
    return keepsLastScreen() ? DarkMode::ScreenPolarity::AsOnScreen : DarkMode::ScreenPolarity::Normal;
  }

 private:
  // Quick Resume: the page on screen stays, with a moon icon over it.
  bool keepsLastScreen() const;
  void renderSleepScreen();
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, const BookOverlayInfo& overlayInfo) const;
  void renderBlankSleepScreen() const;
  void renderOverlaySleepScreen() const;
  // Quick Resume: leaves the framebuffer (reader page) intact and overlays a small moon icon.
  void renderLastScreenSleepScreen() const;
  BookOverlayInfo getBookOverlayInfo(const std::string& bookPath) const;

  const bool fromTimeout = false;
};
