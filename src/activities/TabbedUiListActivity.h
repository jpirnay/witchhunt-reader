#pragma once

#include <vector>

#include "activities/ListTabBar.h"
#include "activities/UiListActivity.h"

// A UiListActivity whose list is split across a row of tabs at the top of the screen.
//
//   * The tab bar is a focus position ABOVE row 0. The list button scheme sees it as position 0 and
//     row i as position i + 1; activeNav().selected == -1 means the bar holds focus.
//   * Up/Down step through bar and rows as one range; a long Up/Down switches to the previous / next
//     tab (spec R4). Left/Right step too, and a long Left/Right pages.
//   * Confirm on the bar advances to the next tab; on a row it activates the row.
//   * Back on a row returns focus to the bar; on the bar it leaves (onBackFromTabs()).
//   * A tap on a tab selects it; a tap on a row follows the usual ListRowTap rule.
//   * Each tab keeps its own ListNav. A long Up/Down opens the new tab where it was left: on the
//     row, or on its bar if the bar held focus there (the first visit, too). Confirm on the bar and
//     a tap on a tab land on the new tab's bar; focusing a tab's bar returns its list to the top.
//
// Per-tab ListNav storage adapted from upstream crosspoint-reader's UiTabListActivity
// (develop @ cdac66ffe, src/activities/UiTabListActivity.cpp).
class TabbedUiListActivity : public UiListActivity {
 protected:
  TabbedUiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput);

  // Most tabs any one screen may show. Only bounds the stack array buildTabBar() composes from.
  static constexpr int MAX_TABS = 8;

  // --- supplied by the subclass ---------------------------------------------------------
  [[nodiscard]] virtual int tabCount() const = 0;
  [[nodiscard]] virtual const char* tabLabel(int slot) const = 0;
  // The selected tab changed: point listCount()/buildScreen() at the new tab's rows. Called
  // before the repaint.
  virtual void onTabSelected(int /*slot*/) {}
  // Back pressed while the bar holds focus. Default leaves the screen.
  virtual void onBackFromTabs() { finish(); }
  // Height the bar takes off the top of the screen.
  [[nodiscard]] virtual int16_t tabBarHeight() const { return ListTabBar::HEIGHT; }
  // Last word on the composed props -- text style, icons, layout. The focus-dependent selected
  // styling is set before this runs and is meant to survive it.
  virtual void customizeTabBar(UiScreen& /*screen*/, freeink::ui::TabBarProps& /*props*/) {}

  // --- provided to the subclass ---------------------------------------------------------
  [[nodiscard]] int selectedTab() const { return selectedTabSlot; }
  // True while the bar, rather than a row, holds focus.
  [[nodiscard]] bool tabsFocused() { return activeNav().selected < 0; }
  // Move to a tab with focus on its bar (a tap on a tab, Confirm on the bar). Out-of-range slots
  // are ignored.
  void selectTab(int slot);
  // Take focus off the rows and back to the bar.
  void focusTabs();
  // Compose the bar and consume it from the top of the screen. Call FIRST from buildScreen(),
  // before laying out the list.
  void buildTabBar(UiScreen& screen);

  void onEnter() override;
  freeink::ui::ListNav& activeNav() override;
  ListRowTap::Result selectListRow(int index) override;

  // The bar is position 0, row i is position i + 1.
  [[nodiscard]] int positionCount() const override { return listCount() + 1; }
  [[nodiscard]] int selectedPosition() const override;
  void selectPosition(int position) override;
  void showPositionPage(int position, int topPosition) override;
  [[nodiscard]] ListWindow positionWindow() const override;
  // The bar: it stays on the first screen and is not a line of a page.
  [[nodiscard]] int leadPositions() const override { return 1; }
  [[nodiscard]] bool isPositionSelectable(int position) const override;
  void activatePosition(int position, bool longPress) override;
  void backFromPosition(int position) override;
  void switchTab(int direction) override;

 private:
  static void tabActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  // Make `slot` the active tab; `barFocus` puts the focus on its bar, otherwise the tab opens
  // where it was left.
  void enterTab(int slot, bool barFocus);

  int selectedTabSlot = 0;
  std::vector<freeink::ui::ListNav> tabNavs;
};
