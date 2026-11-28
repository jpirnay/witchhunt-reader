#pragma once

#include <GfxRenderer.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class MappedInputManager;

// Settings > Controls > Bluetooth Remote: switch the remote on or off, pair one, forget one, swap its two keys. The
// radio and the remotes belong to src/BleRadio; this screen only asks it. While the screen is open the radio does not
// stop for idleness, so a scan and a pairing can take their time.
//
// The shape (one screen, a menu that opens a search and a list of paired remotes) is CrossPoint Reader's
// BleSettingsActivity (#3785, Tuan Q. Nguyen with Justin Mitchell; #4187, Uri Tauber), without its learned bindings.
class BleRemoteActivity final : public UiListActivity {
 public:
  enum class View : uint8_t { Menu, Search, Paired };

  // `start` Search: the toggle action found no remote paired yet, and pairing is what the user needs next.
  explicit BleRemoteActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const View start = View::Menu)
      : UiListActivity("BleRemote", renderer, mappedInput), startView(start) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  int listCount() const override { return static_cast<int>(rows.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void onBackButton() override;
  [[nodiscard]] bool isRowSelectable(int index) const override;

  void showView(View target);
  // Switching on with no remote paired goes straight to the search.
  void setEnabled(bool on);
  void startSearch();
  // What the rows show depends on: a change of it repaints the screen.
  uint32_t signature() const;
  // Rebuilds the rows from the radio; under the render lock, since buildScreen() reads them on the render task.
  void refresh();

  const View startView;
  View view = View::Menu;
  // Written on the loop task under the render lock, read by buildScreen() on the render task.
  std::vector<freeink::ui::ListItem> rows;
  std::vector<std::string> labels;  // the rows' remote names point into these
  // Each remote row's address as it was drawn: a search lists HID remotes first and finds them one by one, so the
  // remote at a row's index can change before the row is chosen.
  std::vector<std::string> addrs;
  std::string status;
  char pairedValue[8] = {};
  uint32_t shownSignature = 0;
  unsigned long lastPollMs = 0;
  unsigned long lastStatusRepaintMs = 0;
  // A search asked for before the radio was up (switching on, or the action's start): it runs once the radio is.
  bool searchPending = false;
  // The address a pairing was asked for: once it links, the screen goes back to the menu, which shows it connected.
  char pairingAddr[18] = {};
};
