#pragma once

#include <functional>

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

enum class SettingAction;

class UiListActivity : public Activity, protected UiAppHost {
 public:
  void onEnter() override;
  // Stops touch routing before the activity goes away. Every screen here builds its UI in
  // onEnter() (resetUi() re-arms routing), so closing it on the way out is always right --
  // and doing it here means a subclass cannot forget. Two did it by hand; the rest did not.
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // What a tap on a row means. Overriding it here is what puts the FreeInkUI path and the
  // legacy ListTouchBand path on the same rule: onRowAction() used to apply ListRowTap
  // inline, so a subclass could override this (as MenuListActivity does, to decline
  // separators) and never be consulted for a tap it actually receives.
  ListRowTap::Result selectListRow(int index) override;

 protected:
  bool tryOpenSliderFor(SettingAction action, std::function<void()> onDone = {});

  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  static constexpr freeink::ui::ActionId ACTION_USER = 2;

  UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput);

  virtual int listCount() const = 0;
  virtual void buildScreen(UiScreen& screen) = 0;
  virtual void activateIndex(int index) = 0;
  virtual freeink::ui::ListNav& activeNav() { return nav; }
  virtual bool handleCustomInput() { return false; }
  virtual bool handleButtons();
  virtual void onBackButton() { finish(); }
  virtual void onSelectionChanged(int /*index*/) {}
  // Runs once the UI has been laid out and drawn, before the footer and the
  // buffer swap. For anything that has to patch the framebuffer using a
  // rectangle only the layout pass knows -- see FontSelectionActivity, which
  // blits a cached preview strip here.
  virtual void afterUiRender() {}
  virtual int indexForActionValue(int16_t value) const { return value; }
  virtual const char* headerTitle() const { return nullptr; }
  virtual void drawChrome();
  virtual void drawFooter();
  virtual void navigateButtons();

  void syncListViewport(UiScreen& screen, freeink::ui::ListProps& props, bool hasSubtitle = false);
  void moveSelectionTo(int index);

  freeink::ui::ListNav nav;
  ButtonNavigator buttonNavigator;

 private:
  static void screenTrampoline(UiScreen& screen, void* user);
  static void rowActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  void onRowAction(int index);
  bool routeListTouch();
};
