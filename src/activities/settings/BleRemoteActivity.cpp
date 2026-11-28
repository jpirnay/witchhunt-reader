#include "BleRemoteActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "BleRadio.h"
#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"

// Only where a BLE host is linked: elsewhere nothing opens the screen (ActivityManager::openBleRemote and the settings
// dispatch are compiled out there), and against BleRadio's stubs its conditions would all be constant.
#if FREEINK_CAP_BLE_HID_HOST

namespace fui = freeink::ui;

namespace {

enum class MenuRow : int16_t { Enable, Pair, Paired, Swap };
// A remote advertises for a while after a key wakes it; 15 s covers a press made after the search started.
constexpr uint32_t kSearchMs = 15000;
constexpr unsigned long kPollMs = 250;
// A search finds remotes one at a time, and every repaint drives the panel again: what the radio does shows at most
// this often. What the user does shows at once.
constexpr unsigned long kStatusRepaintMs = 2000;
constexpr int16_t kNoAction = -1;

}  // namespace

void BleRemoteActivity::onEnter() {
  UiListActivity::onEnter();
  // A radio stopped for idleness may start again, and does not stop for it while the screen is open.
  BleRadio::setScreenOpen(true);
  view = startView;
  if (view == View::Search) searchPending = true;
  refresh();
  requestUpdate();
}

void BleRemoteActivity::onExit() {
  BleRadio::stopScan();
  BleRadio::setScreenOpen(false);
  UiListActivity::onExit();
}

void BleRemoteActivity::loop() {
  UiListActivity::loop();
  if (millis() - lastPollMs < kPollMs) return;
  lastPollMs = millis();

  const BleRadio::Status st = BleRadio::status();
  if (searchPending && st.on && !st.stopping) {
    searchPending = false;
    BleRadio::scan(kSearchMs);
  }
  // The remote being paired linked: the menu shows it connected.
  if (pairingAddr[0] != '\0' && st.linked && std::strcmp(BleRadio::linked().addr, pairingAddr) == 0) {
    pairingAddr[0] = '\0';
    showView(View::Menu);
    return;
  }
  if (signature() == shownSignature) return;
  if (millis() - lastStatusRepaintMs < kStatusRepaintMs) return;
  lastStatusRepaintMs = millis();
  refresh();
  requestUpdate();
}

const char* BleRemoteActivity::headerTitle() const {
  switch (view) {
    case View::Menu:
      return tr(STR_BLE_REMOTE);
    case View::Search:
      return tr(STR_BLE_PAIR_REMOTE);
    case View::Paired:
      return tr(STR_BLE_PAIRED_REMOTES);
  }
  return tr(STR_BLE_REMOTE);
}

void BleRemoteActivity::showView(const View target) {
  {
    RenderLock lock(*this);
    view = target;
    nav.reset();
  }
  refresh();
  requestUpdate();
}

void BleRemoteActivity::onBackButton() {
  if (view == View::Menu) {
    finish();
    return;
  }
  if (view == View::Search) BleRadio::stopScan();
  searchPending = false;
  showView(View::Menu);
}

bool BleRemoteActivity::isRowSelectable(const int index) const {
  return index >= 0 && index < static_cast<int>(rows.size()) && rows[index].enabled;
}

uint32_t BleRemoteActivity::signature() const {
  const BleRadio::Status st = BleRadio::status();
  return (st.on ? 1u : 0u) | (st.stopping ? 2u : 0u) | (st.linked ? 4u : 0u) | (st.connecting ? 8u : 0u) |
         (BleRadio::scanning() ? 16u : 0u) | (st.wifiPaused ? 32u : 0u) | (SETTINGS.bleRemote ? 64u : 0u) |
         (SETTINGS.bleSwapKeys ? 128u : 0u) | static_cast<uint32_t>(BleRadio::savedRemoteCount()) << 8 |
         static_cast<uint32_t>(BleRadio::foundCount()) << 16 | static_cast<uint32_t>(st.refusals & 0xFF) << 24;
}

void BleRemoteActivity::setEnabled(const bool on) {
  SETTINGS.bleRemote = on ? 1 : 0;
  SETTINGS.saveToFile();
  LOG_INF("BLE", "remote %s (screen)", on ? "on" : "off");
  // The radio follows the setting on its next pass (BleRadio::service). With nothing paired yet, pairing is next.
  if (on && BleRadio::savedRemoteCount() == 0) {
    searchPending = true;
    showView(View::Search);
  }
}

void BleRemoteActivity::startSearch() {
  if (BleRadio::status().on) {
    BleRadio::scan(kSearchMs);
  } else {
    searchPending = true;
  }
}

void BleRemoteActivity::refresh() {
  RenderLock lock(*this);
  shownSignature = signature();
  const BleRadio::Status st = BleRadio::status();

  char line[48];
  uint32_t passkey = 0;
  if (!SETTINGS.bleRemote) {
    status = tr(STR_STATE_OFF);
  } else if (st.stopping) {
    status = tr(STR_BLE_STOPPING);
  } else if (st.wifiPaused) {
    status = tr(STR_BLE_WIFI_PAUSED);
  } else if (BleRadio::passkey(passkey)) {
    // A keyboard that pairs with a passkey types this one (the SDK's fixed key; Just Works remotes never ask).
    snprintf(line, sizeof(line), "%s %06lu", tr(STR_BLE_PASSKEY), static_cast<unsigned long>(passkey));
    status = line;
  } else if (BleRadio::lastFailure(line, sizeof(line))) {
    status = line;  // the SDK's own words, untranslated
  } else if (!st.on) {
    status = BleRadio::refusedForMemory(st) ? tr(STR_BLE_LOW_MEMORY) : tr(STR_BLE_STARTING);
  } else if (st.linked) {
    const char* name = BleRadio::linked().name;
    status = name[0] != '\0' ? name : BleRadio::linked().addr;
  } else if (st.connecting) {
    status = tr(STR_CONNECTING);
  } else if (BleRadio::scanning()) {
    status = tr(STR_SEARCHING);
  } else if (BleRadio::savedRemoteCount() > 0) {
    status = tr(STR_BLE_WAITING);
  } else {
    status = tr(STR_STATE_ON);
  }

  // Labels first: the rows point into them.
  const uint8_t found = view == View::Search ? BleRadio::foundCount() : 0;
  const uint8_t saved = view == View::Paired ? BleRadio::savedRemoteCount() : 0;
  labels.clear();
  addrs.clear();
  labels.reserve(found + saved);
  addrs.reserve(found + saved);
  for (uint8_t i = 0; i < found + saved; ++i) {
    const BleRadio::Peer peer = i < found ? BleRadio::found(i) : BleRadio::savedRemote(static_cast<uint8_t>(i - found));
    labels.emplace_back(peer.name[0] != '\0' ? peer.name : peer.addr);
    addrs.emplace_back(peer.addr);
  }

  rows.clear();
  const auto add = [this](const char* label, const int16_t code, const bool enabled = true) -> fui::ListItem& {
    fui::ListItem row;
    row.label = label;
    row.actionValue = code;
    row.enabled = enabled;
    rows.push_back(row);
    return rows.back();
  };
  switch (view) {
    case View::Menu: {
      auto& enable = add(tr(STR_BLE_REMOTE), static_cast<int16_t>(MenuRow::Enable));
      enable.subtitle = status.c_str();
      enable.toggle = true;
      enable.toggleChecked = SETTINGS.bleRemote != 0;
      add(tr(STR_BLE_PAIR_REMOTE), static_cast<int16_t>(MenuRow::Pair), SETTINGS.bleRemote != 0 && !st.wifiPaused);
      const uint8_t savedCount = BleRadio::savedRemoteCount();
      auto& paired = add(tr(STR_BLE_PAIRED_REMOTES), static_cast<int16_t>(MenuRow::Paired), savedCount > 0);
      snprintf(pairedValue, sizeof(pairedValue), "%u", static_cast<unsigned>(savedCount));
      paired.value = pairedValue;
      auto& swap = add(tr(STR_BLE_SWAP_KEYS), static_cast<int16_t>(MenuRow::Swap));
      swap.subtitle = tr(STR_BLE_SWAP_KEYS_HINT);
      swap.toggle = true;
      swap.toggleChecked = SETTINGS.bleSwapKeys != 0;
      break;
    }
    case View::Search:
      add(BleRadio::scanning() ? tr(STR_BLE_STOP_SEARCH) : tr(STR_BLE_SEARCH_AGAIN), kNoAction, st.on);
      for (uint8_t i = 0; i < found; ++i) add(labels[i].c_str(), static_cast<int16_t>(i));
      if (found == 0) {
        add(BleRadio::scanning() || searchPending ? tr(STR_SEARCHING) : tr(STR_BLE_NONE_FOUND), kNoAction, false);
      }
      break;
    case View::Paired:
      for (uint8_t i = 0; i < saved; ++i) {
        // Forgetting asks the radio, which is up only while the remote is switched on.
        auto& row = add(labels[found + i].c_str(), static_cast<int16_t>(i), st.on);
        row.value = tr(STR_FORGET_BUTTON);
      }
      if (saved == 0) add(tr(STR_BLE_NONE_PAIRED), kNoAction, false);
      break;
  }
  if (nav.selected >= static_cast<int>(rows.size())) nav.selected = static_cast<int>(rows.size()) - 1;
}

void BleRemoteActivity::activateIndex(const int index) {
  if (!isRowSelectable(index)) return;
  nav.selected = index;
  // The row stays on screen with a new value; a lingering flash would gray it on the repaint below.
  app.clearTapFlash();
  const int16_t code = rows[index].actionValue;
  switch (view) {
    case View::Menu:
      switch (static_cast<MenuRow>(code)) {
        case MenuRow::Enable:
          setEnabled(SETTINGS.bleRemote == 0);
          if (view != View::Menu) return;  // setEnabled() went to the search
          break;
        case MenuRow::Pair:
          searchPending = false;
          startSearch();
          showView(View::Search);
          return;
        case MenuRow::Paired:
          showView(View::Paired);
          return;
        case MenuRow::Swap:
          SETTINGS.bleSwapKeys = SETTINGS.bleSwapKeys ? 0 : 1;
          SETTINGS.saveToFile();
          break;
      }
      break;
    case View::Search:
      if (code == kNoAction) {
        if (BleRadio::scanning()) {
          BleRadio::stopScan();
        } else {
          startSearch();
        }
      } else {
        snprintf(pairingAddr, sizeof(pairingAddr), "%s", addrs[static_cast<size_t>(code)].c_str());
        BleRadio::stopScan();
        if (!BleRadio::pair(pairingAddr)) pairingAddr[0] = '\0';
        showView(View::Menu);
        return;
      }
      break;
    case View::Paired: {
      // Copied: refresh() below rebuilds addrs.
      char addr[18];
      snprintf(addr, sizeof(addr), "%s", addrs[static_cast<size_t>(code)].c_str());
      BleRadio::forget(addr);
      if (BleRadio::savedRemoteCount() == 0) {
        showView(View::Menu);
        return;
      }
      break;
    }
  }
  refresh();
  requestUpdate();
}

void BleRemoteActivity::buildScreen(UiScreen& screen) {
  layoutListArea(screen);
  auto props = listProps(screen);
  props.items = rows.data();
  props.count = static_cast<uint16_t>(rows.size());
  addList(screen, props, /*hasSubtitle=*/view == View::Menu);
}

#endif  // FREEINK_CAP_BLE_HID_HOST
