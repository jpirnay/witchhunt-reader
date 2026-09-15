#pragma once

#include "activities/UiListActivity.h"

// A UiListActivity whose list is split across a row of tabs at the top of the screen.
//
// The interaction model both tabbed screens already used, in one place:
//
//   * The tab bar is a focus position ABOVE row 0. activeNav().selected == -1 means the bar
//     holds focus; >= 0 means a list row does. tabsFocused() is the test.
//   * Up/Down steps through that combined range, so walking up off row 0 lands on the bar.
//   * Left/Right switches tabs from anywhere.
//   * Confirm on the bar advances to the next tab; on a row it activates the row.
//   * Back on a row returns focus to the bar; on the bar it leaves (onBackFromTabs()).
//   * A tap on a tab selects it; a tap on a row follows the usual ListRowTap rule.
//
// The two screens that grew this independently -- the settings screen and the EPUB reader menu
// -- encoded the focus position differently (one reserved index 0 for the bar and offset every
// row by one, the other used -1), which is why nothing was shared. This class settles on -1:
// it keeps list indices meaning what they mean everywhere else in the firmware, so subclasses
// index their own items without an offset.
//
// The selection itself still lives in activeNav(), which UiListActivity leaves virtual. A screen
// that wants each tab to remember its own scroll position overrides activeNav() with a per-tab
// ListNav; one that wants a single shared position does nothing.
class TabbedUiListActivity : public UiListActivity {
 protected:
  TabbedUiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity(name, renderer, mappedInput) {}

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
  // False for rows navigation must step over, such as a section separator. Default: all rows.
  [[nodiscard]] virtual bool isRowSelectable(int /*index*/) const { return true; }
  // Height the bar takes off the top of the screen.
  [[nodiscard]] virtual int16_t tabBarHeight() const { return 54; }
  // Last word on the composed props -- text style, icons, layout. The focus-dependent selected
  // styling is set before this runs and is meant to survive it.
  virtual void customizeTabBar(UiScreen& /*screen*/, freeink::ui::TabBarProps& /*props*/) {}

  // --- provided to the subclass ---------------------------------------------------------
  [[nodiscard]] int selectedTab() const { return selectedTabSlot; }
  // True while the bar, rather than a row, holds focus.
  // Not const: activeNav() is the virtual accessor UiListActivity exposes, and it hands back a
  // mutable reference so subclasses can steer it. Nothing needs to ask this from a const method.
  [[nodiscard]] bool tabsFocused() { return activeNav().selected < 0; }
  // Move to a tab and put focus on the bar. Out-of-range slots are ignored.
  void selectTab(int slot);
  // Take focus off the rows and back to the bar.
  void focusTabs();
  // Compose the bar and consume it from the top of the screen. Call FIRST from buildScreen(),
  // before laying out the list.
  void buildTabBar(UiScreen& screen);

  void onEnter() override;
  bool handleButtons() override;
  void navigateButtons() override;
  ListRowTap::Result selectListRow(int index) override;

 private:
  static void tabActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  // Step the selection one row, treating -1 (the bar) as the position before row 0.
  void stepSelection(bool forward);

  int selectedTabSlot = 0;
};
