#pragma once
#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "UiListActivity.h"
#include "components/themes/BaseTheme.h"
#include "settings/SettingInfo.h"

// Base class for activities that display a scrollable list of SettingInfo items.
// Provides toggle/cycle logic and drawMenuList() rendering; the buttons follow the list scheme
// UiListActivity runs through its ListController.
//
// Subclasses populate `menuItems` (typically in the constructor or onEnter()),
// then rely on the default loop()/onEnter() or override selectively.
//
// === Minimal example ===
//
//   class MyMenuActivity final : public MenuListActivity {
//    public:
//     explicit MyMenuActivity(GfxRenderer& r, MappedInputManager& m)
//         : MenuListActivity("MyMenu", r, m) {
//       // Separator header
//       menuItems.push_back(SettingInfo::Separator(StrId::STR_MY_SECTION));
//
//       // Toggle bound to a CrossPointSettings field
//       menuItems.push_back(SettingInfo::Toggle(StrId::STR_MY_TOGGLE, &CrossPointSettings::myFlag));
//
//       // Enum bound to a CrossPointSettings field
//       menuItems.push_back(SettingInfo::Enum(StrId::STR_MY_ENUM, &CrossPointSettings::myEnum,
//           {StrId::STR_OPTION_A, StrId::STR_OPTION_B, StrId::STR_OPTION_C}));
//
//       // DynamicEnum with local state (getter/setter lambdas)
//       menuItems.push_back(SettingInfo::DynamicEnum(StrId::STR_MY_DYNAMIC,
//           {StrId::STR_LOW, StrId::STR_HIGH},
//           [this]() -> uint8_t { return localValue; },
//           [this](uint8_t v)   { localValue = v; }));
//
//       // Action item (handled in onActionSelected)
//       menuItems.push_back(SettingInfo::Action(StrId::STR_DO_SOMETHING, SettingAction::None));
//     }
//
//     // render(): draw header/footer around the list
//     void render(RenderLock&&) override {
//       renderer.clearScreen();
//       const Rect r = listContentRect();           // leaves room for both hint strips
//       drawMenuList(r);                            // <-- draws the item list
//       drawListHints();                            // bottom and side hints
//       renderer.displayBuffer();
//     }
//
//    private:
//     uint8_t localValue = 0;
//
//     // Called when an ACTION item is confirmed
//     void onActionSelected(int index) override {
//       if (menuItems[index].nameId == StrId::STR_DO_SOMETHING) { /* ... */ }
//     }
//
//     // Called after a TOGGLE/ENUM/VALUE is cycled
//     void onSettingToggled(int) override { SETTINGS.saveToFile(); }
//   };
//
// The base class provides:
//   isRowSelectable() — overridden so the selection steps over separators.
//   onEnter()  — prepares submenus, moves the selection off a separator, and requestUpdate().
//   loop()     — Back (→ onBackPressed), Confirm (→ toggleCurrentItem) and navigation, all
//                through UiListActivity's ListController.
//   drawMenuList(rect) — renders a virtualized FreeInkUI list of SettingInfo rows.
//   getItemValueString(i) — override for custom per-item value display.
//   onBackPressed()    — override to customise Back behaviour (default: finish()).
//
class MenuListActivity : public UiListActivity {
 protected:
  std::vector<SettingInfo> menuItems;
  std::vector<SettingInfo::SubmenuData> submenuData;
  std::atomic<int>& selectedIndex;
  bool submenusPrepared = false;

  // Call after building/rebuilding menuItems: moves the selection off a separator, onto the next
  // row isRowSelectable() accepts.
  void initMenuList();

  // Virtual so a subclass can supply its own submenu wiring: toggleCurrentItem()
  // below dispatches through this, and EpubReaderMenuActivity needs a per-item
  // value-string override that the plain version has no way to pass.
  virtual void openSubmenu(const SettingInfo& submenuEntry);

  // Toggle/cycle the currently selected item.  For ACTION items, delegates to onActionSelected().
  virtual void toggleCurrentItem();

  // Draw the FreeInkUI list into the given rect.
  void drawMenuList(const Rect& rect);

  // Override to provide custom value display for specific items.
  // Default returns SettingInfo::getDisplayValue().
  [[nodiscard]] virtual std::string getItemValueString(int index) const;

  // Called when an ACTION-type item is confirmed.  Override to handle actions.
  virtual void onActionSelected(int index) {}

  // Called when Back is pressed.  Default calls finish().
  virtual void onBackPressed() { finish(); }

  // Called after a TOGGLE/ENUM/VALUE item has been toggled.
  // Override to persist changes or trigger side-effects.
  virtual void onSettingToggled(int /*index*/) {}

  int listCount() const override { return static_cast<int>(menuItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override { onBackPressed(); }

  // Separators are not rows the selection can rest on.
  [[nodiscard]] bool isRowSelectable(int index) const override;

 public:
  MenuListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  // Move the selection to a tapped menu row. Separators decline.
  ListRowTap::Result selectListRow(int index) override;
  void loop() override;

 private:
  void materializeListWindow();

  static constexpr size_t LIST_WINDOW_CAPACITY = 24;
  Rect listRect{};
  std::array<std::string, LIST_WINDOW_CAPACITY> windowLabels;
  std::array<std::string, LIST_WINDOW_CAPACITY> windowValues;
  std::array<freeink::ui::ListItem, LIST_WINDOW_CAPACITY> windowItems;
  uint16_t windowFirst = 0;
  uint16_t windowCount = 0;
};
