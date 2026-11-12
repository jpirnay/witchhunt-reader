#include "ReaderBarsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <utility>

#include "MappedInputManager.h"
#include "activities/ListTabBar.h"
#include "components/UIThemeTokens.h"
#include "util/SerialCommandGrammar.h"

namespace fui = freeink::ui;
using namespace ReaderBarsLayout;

ReaderBarsActivity::ReaderBarsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Info info)
    : Activity("ReaderBars", renderer, mappedInput), UiAppHost(renderer), info(std::move(info)) {}

void ReaderBarsActivity::onEnter() {
  Activity::onEnter();
  starred = info.starred;
  // Keys show their cursor from the start where they are the only way in; a touch board waits for a key.
  focus = mappedInput.hasTouch() ? Focus::None : Focus::Tabs;

  resetUi();
  metrics = ReaderBarsLayout::metricsFor(uiTarget, uiThemeTokens(uiTarget), ListTabBar::HEIGHT, mappedInput.hasTouch());
  area = fui::Rect{0, 0, static_cast<int16_t>(renderer.getScreenWidth()),
                   static_cast<int16_t>(renderer.getScreenHeight())};
  app.on(ACTION_EXIT, &ReaderBarsActivity::onExitEvent, this);
  app.on(ACTION_STAR, &ReaderBarsActivity::onStarEvent, this);
  app.on(ACTION_TAB, &ReaderBarsActivity::onTabEvent, this);
  app.on(ACTION_PREV_CHAPTER, &ReaderBarsActivity::onPrevChapterEvent, this);
  app.on(ACTION_GO_TO, &ReaderBarsActivity::onGoToEvent, this);
  app.on(ACTION_NEXT_CHAPTER, &ReaderBarsActivity::onNextChapterEvent, this);
  app.setScreen(&ReaderBarsActivity::barsScreen, this);
  requestUpdate();
}

void ReaderBarsActivity::appendDebugState(std::string& out) const {
  static constexpr const char* kFocus[] = {"none", "exit", "star", "tabs", "prev", "goto", "next"};
  static constexpr const char* kTab[] = {"contents", "progress", "more"};
  SerialCommandGrammar::appendField(out, "focus", kFocus[static_cast<uint8_t>(focus)]);
  SerialCommandGrammar::appendField(out, "tab", kTab[static_cast<uint8_t>(focusedTab)]);
  SerialCommandGrammar::appendField(out, "progress", progressOpen ? 1L : 0L);
  SerialCommandGrammar::appendField(out, "starred", starred ? 1L : 0L);
}

void ReaderBarsActivity::onExit() {
  closeRouting();
  Activity::onExit();
}

ReaderBarsLayout::Model ReaderBarsActivity::model() const {
  ReaderBarsLayout::Model model;
  model.title = info.title.c_str();
  model.starred = starred;
  model.progressOpen = progressOpen;
  model.focus = focus;
  model.focusedTab = focusedTab;
  model.bookLabel = tr(STR_BOOK);
  model.bookValue = info.bookValue.c_str();
  model.chapterLabel = tr(STR_CHAPTER);
  model.chapterValue = info.chapterValue.empty() ? nullptr : info.chapterValue.c_str();
  model.timeLabel = tr(STR_READING_STATS_TOTAL_TIME);
  model.timeValue = info.timeValue.empty() ? nullptr : info.timeValue.c_str();
  model.goToLabel = tr(STR_GO_TO_PERCENT);
  return model;
}

void ReaderBarsActivity::barsScreen(UiScreen& screen, void* user) {
  static_cast<ReaderBarsActivity*>(user)->buildBars(screen);
}

void ReaderBarsActivity::buildBars(UiScreen& screen) {
  const char* labels[kTabCount] = {tr(STR_CONTENTS), tr(STR_READING_STATS_PROGRESS), tr(STR_MORE)};
  for (uint8_t i = 0; i < kTabCount; ++i) {
    tabs[i].label = labels[i];
    tabs[i].value = static_cast<int16_t>(i);
    // The open panel's tab, or the one under the keys' cursor.
    tabs[i].selected = (progressOpen && static_cast<Tab>(i) == Tab::Progress) ||
                       (focus == Focus::Tabs && static_cast<Tab>(i) == focusedTab);
  }
  const fui::TabBarProps props = ListTabBar::props(screen, tabs, kTabCount, ACTION_TAB, focus == Focus::Tabs);
  ReaderBarsLayout::build(screen, model(), metrics, props);
}

void ReaderBarsActivity::render(RenderLock&&) {
  // The bars cover only their bands and leave the page between them standing, so the rest of the frame must hold what
  // is on the panel. NOT on the opening pass: EpubReaderActivity::openReaderBars() put the page there, and a sync
  // would copy whatever the secondary buffer held instead (a pre-rendered next page, or nothing while it is lent).
  // Every later pass needs it: displayBuffer() below ends in a swap, so the write buffer is two frames behind.
  if (!firstRender) renderer.syncWriteBufferFromDisplayed();
  renderUi();
  renderer.displayBuffer();
  firstRender = false;
}

void ReaderBarsActivity::close(const ReaderBarsResult::Action action) {
  ReaderBarsResult result;
  result.action = action;
  result.starToggled = starToggled;
  setResult(ActivityResult(result));
  finish();
}

void ReaderBarsActivity::toggleStar() {
  {
    RenderLock lock;
    starred = !starred;
    starToggled = !starToggled;
  }
  requestUpdate();
}

void ReaderBarsActivity::openTab(const Tab tab) {
  switch (tab) {
    case Tab::Contents:
      close(ReaderBarsResult::Action::Contents);
      return;
    case Tab::More:
      close(ReaderBarsResult::Action::More);
      return;
    case Tab::Progress:
      // Progress closing would have to bring back the page it covered: the reader draws it instead.
      if (progressOpen) {
        close(ReaderBarsResult::Action::None);
        return;
      }
      {
        RenderLock lock;
        progressOpen = true;
        if (focus != Focus::None) focus = Focus::GoTo;
      }
      requestUpdate();
      return;
    case Tab::Count:
      return;
  }
}

void ReaderBarsActivity::onExitEvent(const fui::ActionEvent&, void* user) {
  static_cast<ReaderBarsActivity*>(user)->close(ReaderBarsResult::Action::Exit);
}

void ReaderBarsActivity::onStarEvent(const fui::ActionEvent&, void* user) {
  static_cast<ReaderBarsActivity*>(user)->toggleStar();
}

void ReaderBarsActivity::onTabEvent(const fui::ActionEvent& event, void* user) {
  if (event.value < 0 || event.value >= kTabCount) return;
  static_cast<ReaderBarsActivity*>(user)->openTab(static_cast<Tab>(event.value));
}

void ReaderBarsActivity::onPrevChapterEvent(const fui::ActionEvent&, void* user) {
  static_cast<ReaderBarsActivity*>(user)->close(ReaderBarsResult::Action::PrevChapter);
}

void ReaderBarsActivity::onGoToEvent(const fui::ActionEvent&, void* user) {
  static_cast<ReaderBarsActivity*>(user)->close(ReaderBarsResult::Action::GoToPercent);
}

void ReaderBarsActivity::onNextChapterEvent(const fui::ActionEvent&, void* user) {
  static_cast<ReaderBarsActivity*>(user)->close(ReaderBarsResult::Action::NextChapter);
}

void ReaderBarsActivity::activateFocus() {
  switch (focus) {
    case Focus::None:
      return;
    case Focus::Exit:
      close(ReaderBarsResult::Action::Exit);
      return;
    case Focus::Star:
      toggleStar();
      return;
    case Focus::Tabs:
      openTab(focusedTab);
      return;
    case Focus::PrevChapter:
      close(ReaderBarsResult::Action::PrevChapter);
      return;
    case Focus::GoTo:
      close(ReaderBarsResult::Action::GoToPercent);
      return;
    case Focus::NextChapter:
      close(ReaderBarsResult::Action::NextChapter);
      return;
  }
}

// Left/Right: along the row the cursor is on, stopping at its ends.
void ReaderBarsActivity::moveAlongRow(const int step) {
  RenderLock lock;
  switch (focus) {
    case Focus::None:
      focus = Focus::Tabs;
      return;
    case Focus::Exit:
    case Focus::Star:
      focus = step < 0 ? Focus::Exit : Focus::Star;
      return;
    case Focus::Tabs: {
      const int next = static_cast<int>(focusedTab) + step;
      if (next >= 0 && next < kTabCount) focusedTab = static_cast<Tab>(next);
      return;
    }
    case Focus::PrevChapter:
      focus = step < 0 ? Focus::PrevChapter : Focus::GoTo;
      return;
    case Focus::GoTo:
      focus = step < 0 ? Focus::PrevChapter : Focus::NextChapter;
      return;
    case Focus::NextChapter:
      focus = step < 0 ? Focus::GoTo : Focus::NextChapter;
      return;
  }
}

// Up/Down: between the rows as they lie on screen, the top bar, the Progress buttons while Progress is open, and the
// tabs. A row is entered on its middle control: the star, Go to %, the tab the cursor last left.
void ReaderBarsActivity::moveBetweenRows(const int step) {
  RenderLock lock;
  enum class Row : uint8_t { Top, Buttons, Tabs };
  Row row = Row::Tabs;
  switch (focus) {
    case Focus::None:
      focus = Focus::Tabs;
      return;
    case Focus::Exit:
    case Focus::Star:
      row = Row::Top;
      break;
    case Focus::PrevChapter:
    case Focus::GoTo:
    case Focus::NextChapter:
      row = Row::Buttons;
      break;
    case Focus::Tabs:
      row = Row::Tabs;
      break;
  }
  Row target = row;
  if (step < 0) {
    if (row == Row::Tabs) target = progressOpen ? Row::Buttons : Row::Top;
    if (row == Row::Buttons) target = Row::Top;
  } else {
    if (row == Row::Top) target = progressOpen ? Row::Buttons : Row::Tabs;
    if (row == Row::Buttons) target = Row::Tabs;
  }
  if (target == row) return;
  switch (target) {
    case Row::Top:
      focus = Focus::Star;
      return;
    case Row::Buttons:
      focus = Focus::GoTo;
      return;
    case Row::Tabs:
      focus = Focus::Tabs;
      return;
  }
}

void ReaderBarsActivity::loop() {
  const auto touch = routeTouch(mappedInput);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    // A handled event may have closed the bars already.
    if (touch) return;
  }
  // A tap on the page between the bars puts the reader back; one on a bar but off its controls does nothing.
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    bool onBars = false;
    {
      RenderLock lock;
      onBars = ReaderBarsLayout::onBars(area, model(), metrics, x, y);
    }
    if (!onBars) {
      close(ReaderBarsResult::Action::None);
      return;
    }
  }
  if (mappedInput.wasHomeGesture() || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close(ReaderBarsResult::Action::None);
    return;
  }

  // The first key on a touch board only shows the cursor.
  const bool cursorHidden = focus == Focus::None;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (cursorHidden) {
      moveAlongRow(0);
      requestUpdate();
      return;
    }
    activateFocus();
    return;
  }
  int along = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) along = -1;
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) along = 1;
  if (along != 0) {
    moveAlongRow(cursorHidden ? 0 : along);
    requestUpdate();
    return;
  }
  int between = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) between = -1;
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) between = 1;
  if (between != 0) {
    moveBetweenRows(cursorHidden ? 0 : between);
    requestUpdate();
  }
}
