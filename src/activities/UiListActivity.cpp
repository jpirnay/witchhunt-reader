#include "UiListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

UiListActivity::UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity(name, renderer, mappedInput), UiAppHost(renderer) {}

void UiListActivity::onEnter() {
  Activity::onEnter();
  activeNav().reset();
  resetUi();
  app.on(ACTION_ROW, &UiListActivity::rowActionTrampoline, this);
  app.setScreen(&UiListActivity::screenTrampoline, this);
  requestUpdate();
}

void UiListActivity::onExit() {
  closeRouting();
  Activity::onExit();
}

void UiListActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<UiListActivity*>(user)->buildScreen(screen);
}

void UiListActivity::rowActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiListActivity*>(user);
  const int index = self->indexForActionValue(event.value);
  if (index < 0 || index >= self->listCount()) return;
  self->onRowAction(index);
}

ListRowTap::Result UiListActivity::selectListRow(const int index) {
  return ListRowTap::apply(index, listCount(), activeNav().selected);
}

void UiListActivity::onRowAction(const int index) {
  const auto result = listTapActivation.applyPreference(
      index, selectListRow(index), SETTINGS.touchListActivation == CrossPointSettings::TOUCH_LIST_ACTIVATE_IMMEDIATELY);
  if (result == ListRowTap::Result::Rejected) return;
  if (result == ListRowTap::Result::Selected) {
    moveSelectionTo(index);
    return;
  }
  activateIndex(index);
}

bool UiListActivity::handleButtons() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const int selected = activeNav().selected;
    if (selected >= 0 && selected < listCount()) activateIndex(selected);
    return true;
  }
  return false;
}

bool UiListActivity::routeListTouch() {
  const auto route = UiAppHost::routeTouch(mappedInput);
  if (route.routed && app.invalidated()) requestUpdate();
  return static_cast<bool>(route);
}

void UiListActivity::moveSelectionTo(const int index) {
  {
    RenderLock lock(*this);
    auto& currentNav = activeNav();
    currentNav.selected = index;
    currentNav.follow(listCount());
  }
  onSelectionChanged(index);
  requestUpdate();
}

void UiListActivity::loop() {
  if (handleCustomInput()) return;
  if (handleButtons()) return;
  if (routeListTouch()) return;

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    bool moved = false;
    {
      RenderLock lock(*this);
      auto& currentNav = activeNav();
      const int delta = swipe == MappedInputManager::SwipeDir::Up ? currentNav.pageRows() : -currentNav.pageRows();
      moved = currentNav.scrollBy(delta, listCount());
    }
    if (moved) requestUpdate();
    return;
  }

  navigateButtons();
}

void UiListActivity::navigateButtons() {
  const int count = listCount();
  auto& currentNav = activeNav();
  buttonNavigator.onNextRelease(
      [this, count, &currentNav] { moveSelectionTo(ButtonNavigator::nextIndex(currentNav.selected, count)); });
  buttonNavigator.onPreviousRelease(
      [this, count, &currentNav] { moveSelectionTo(ButtonNavigator::previousIndex(currentNav.selected, count)); });
  buttonNavigator.onNextContinuous([this, count, &currentNav] {
    moveSelectionTo(ButtonNavigator::nextPageIndex(currentNav.selected, count, currentNav.pageRows()));
  });
  buttonNavigator.onPreviousContinuous([this, count, &currentNav] {
    moveSelectionTo(ButtonNavigator::previousPageIndex(currentNav.selected, count, currentNav.pageRows()));
  });
}

void UiListActivity::syncListViewport(UiScreen& screen, fui::ListProps& props, const bool hasSubtitle) {
  int16_t rowHeight = screen.theme().rowHeight;
  if (!mappedInput.hasTouch()) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    rowHeight = static_cast<int16_t>(hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight);
    props.rowHeight = rowHeight;
  }
  activeNav().syncToProps(screen.body(), rowHeight, screen.theme().listRowGap, listCount(), props);
}

void UiListActivity::drawChrome() {
  const char* title = headerTitle();
  if (!title) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
}

void UiListActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void UiListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  drawChrome();
  renderUi();
  for (int pass = 0; activeNav().consumeRebuildNeeded() && pass < 8; ++pass) {
    renderer.clearScreen();
    drawChrome();
    renderUi();
  }
  drawFooter();
  renderer.displayBuffer();
}