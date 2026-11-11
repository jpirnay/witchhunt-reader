#pragma once

#include <cstdint>

// Dark Mode (CrossPointSettings::darkMode): the panel shows white on black. What it decides, apart from the code that
// acts on it so the host can test it (test/dark_mode).
//
// The frame buffers keep their colours and nothing draws differently: the display inverts each frame on its way to
// the panel (HalDisplay::setInverted), and pictures draw inverted so that they show as they are
// (GfxRenderer::keepsImagePolarity). Design after crosspoint-reader PR #2705 (Justin Mitchell).
namespace DarkMode {

// What a screen asks of the panel's polarity (Activity::outputPolarity).
enum class ScreenPolarity : uint8_t {
  FollowDarkMode,  // every ordinary screen
  Normal,          // a picture of its own: the sleep screens, the image viewer
  AsOnScreen,      // the frame already on screen, in the polarity it went out in: Quick Resume's page
};

// Whether the screen on top goes out inverted; `invertedNow` is how the frame on screen went out.
constexpr bool invertsOutput(const bool darkMode, const ScreenPolarity screen, const bool invertedNow) {
  switch (screen) {
    case ScreenPolarity::Normal:
      return false;
    case ScreenPolarity::AsOnScreen:
      return invertedNow;
    case ScreenPolarity::FollowDarkMode:
      break;
  }
  return darkMode;
}

// Whether text anti-aliasing the reader asks for reaches the panel. Inverted, the gray planes do not survive
// (HalDisplay::setInverted), so the gray passes would cost their render and show nothing; only a panel whose single
// push mirrors its greys keeps them (HalDisplay::invertsGrayFrame: the LilyGo T5S3).
constexpr bool showsTextAntiAliasing(const bool wanted, const bool outputInverted, const bool panelInvertsGrayFrame) {
  return wanted && (!outputInverted || panelInvertsGrayFrame);
}

// Quick Resume adds its moon icon to the page on screen. A HALF drives every pixel through its inverse, which on a
// dark page is a white flash across the whole screen; only the moon changes, so a FAST adds it. Light mode keeps its
// HALF. After crosspoint-reader 5bfaf7bdb (Justin Mitchell).
constexpr bool moonGoesOutFast(const bool outputInverted) { return outputInverted; }

// Quick Resume's saved frame comes back at wake before any screen has set the polarity, so it goes out the way boot
// sets it: inverted exactly when Dark Mode is on. A frame that went out the other way (the image viewer, which keeps
// normal polarity) is saved inverted, so it comes back as it was on the glass.
constexpr bool savedFrameInverts(const bool shownInverted, const bool darkMode) { return shownInverted != darkMode; }

// The ghosting budget, in pages or frames between ghost-clearing refreshes (Refresh Frequency). White text leaves more
// behind on black than black text on white, so Dark Mode clears twice as often: 15 becomes 8. Never (0) stays never.
constexpr int refreshFrequency(const int pages, const bool darkMode) {
  return darkMode && pages > 1 ? (pages + 1) / 2 : pages;
}

}  // namespace DarkMode
