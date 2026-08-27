/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>

#include "ReaderUtils.h"
#include "XtcReaderMenuActivity.h"
#include "activities/Activity.h"

class XtcReaderActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;
  int pagesUntilFullRefresh = 0;
  ReaderUtils::InputDrainGuard inputDrainGuard;

  void renderPage();
  void openReaderMenu();
  void onReaderMenuConfirm(XtcReaderMenuActivity::MenuAction action);
  // Draws the shared reader status bar over the pre-rendered page. Unlike the reflowed
  // readers, XTC cannot reserve margin space for it, so the band is cleared to white first
  // and the bar is an opaque overlay. Honours the global status-bar settings throughout,
  // so turning every item off leaves the page untouched.
  void renderStatusBar() const;
  // True when logical row y falls inside a status-bar band. The XTH gray passes use this to
  // leave the band untouched; without it the gray planes would tint the overlay.
  bool isStatusBarRow(int y) const;
  void saveProgress() const;
  void loadProgress();

  // What the status bar last showed. A minute tick re-renders the whole page here — for XTH that
  // is four passes over an SD-streamed page — so it is only worth doing when the bar would
  // actually change. Same guard as LineReaderActivity, where the re-render is far cheaper.
  mutable int lastStatusBarPage = -1;
  mutable int lastStatusBarBattery = -1;
  mutable int lastStatusBarClockMinute = -1;

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc)
      : Activity("XtcReader", renderer, mappedInput), xtc(std::move(xtc)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool shouldSkipPeriodicUpdate() const override;
  void onButtonAction(CrossPointSettings::BUTTON_ACTION action) override;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
};
