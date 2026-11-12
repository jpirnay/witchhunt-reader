#pragma once

#include <string>

#include "ReaderBarsLayout.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"

// The reader bars: a top bar (back out of the book, its title, the page's star) and a tab bar (Contents, Progress,
// More) painted over the page the reader left on the panel, which stays readable between them. Opened by Confirm, a
// tap in the middle of the page, or a tap on the status bar (EpubReaderActivity::openReaderBars).
//
// Contents and More hand back to the reader, which opens its chapter list and its full menu: every reader feature
// stays one step past More. Progress opens in place, above the tab bar: where the book and the chapter stand, the time
// spent in the book, and previous chapter / Go to % / next chapter.
//
// The bars draw over the page and keep no copy of it: a C3 has no 48 KB to spare for one. So anything that would have
// to bring a covered part of the page back closes them instead, and the reader draws the page afresh: Back, a tap on
// the page, and Progress closing (its tab again, or Back while it is open).
//
// Keys work on every board, so the X3 and X4 reach all of it: Left/Right along a row, Up/Down between the top bar, the
// Progress buttons and the tabs, Confirm to act, Back to close. On a touch board the keys' cursor stays hidden until a
// key is pressed.
class ReaderBarsActivity final : public Activity, private UiAppHost {
 public:
  // What the reader fills in. The values are formatted; an empty timeValue leaves the time row out.
  struct Info {
    std::string title;
    bool starred = false;
    std::string bookValue;
    std::string chapterValue;
    std::string timeValue;
  };

  ReaderBarsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Info info);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // The bars paint only their two bands over the frame the reader prepared before pushing them; a busy indicator
  // synced in between would put another frame under them.
  bool suppressesBusyIndicator() const override { return true; }
  // For the serial CMD:STATE: where the keys' cursor is, whether Progress is open, the page's star.
  void appendDebugState(std::string& out) const override;

 private:
  using Tab = ReaderBarsLayout::Tab;
  using Focus = ReaderBarsLayout::Focus;

  Info info;
  bool starred = false;
  bool starToggled = false;
  bool progressOpen = false;
  Focus focus = Focus::None;
  Tab focusedTab = Tab::Contents;
  // The opening pass composites over the frame the reader prepared; every later one syncs first (render()).
  bool firstRender = true;
  // Set once in onEnter(): the faces do not change while the bars are open, and the loop task's tap test reads them.
  ReaderBarsLayout::Metrics metrics{};
  freeink::ui::Rect area{};
  freeink::ui::TabItem tabs[ReaderBarsLayout::kTabCount]{};

  ReaderBarsLayout::Model model() const;
  static void barsScreen(UiScreen& screen, void* user);
  void buildBars(UiScreen& screen);

  static void onExitEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onStarEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onTabEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onPrevChapterEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onGoToEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onNextChapterEvent(const freeink::ui::ActionEvent& event, void* user);

  void close(ReaderBarsResult::Action action);
  void toggleStar();
  void openTab(Tab tab);
  void activateFocus();
  void moveAlongRow(int step);
  void moveBetweenRows(int step);
};
