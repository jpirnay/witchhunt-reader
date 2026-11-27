#include "NetworkModeSelectionActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <iterator>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "SilentRestart.h"
#include "components/icons/networkModeIcons.h"
#include "network/WifiSessionEnd.h"

namespace fui = freeink::ui;

namespace {
// The last row is the USB transfer method this board actually has. Boards
// that can act as a USB mass-storage device (FREEINK_CAP_USB_MSC) show "USB
// Drive"; the rest keep the serial protocol.
#if FREEINK_CAP_USB_MSC
constexpr NetworkMode USB_MODE = NetworkMode::USB_DRIVE;
constexpr StrId USB_MODE_LABEL = StrId::STR_USB_DRIVE;
constexpr StrId USB_MODE_DESC = StrId::STR_USB_DRIVE_DESC;
constexpr const uint8_t* USB_MODE_ICON = networkModeIconUsb;
#else
constexpr NetworkMode USB_MODE = NetworkMode::USB_SERIAL;
constexpr StrId USB_MODE_LABEL = StrId::STR_USB_TRANSFER;
constexpr StrId USB_MODE_DESC = StrId::STR_USB_TRANSFER_DESC;
constexpr const uint8_t* USB_MODE_ICON = networkModeIconTransfer;
#endif

struct ModeRow {
  NetworkMode mode;
  StrId label;
  StrId desc;
  const uint8_t* icon;
};

// Display order, top to bottom.
constexpr ModeRow kRows[] = {
    {NetworkMode::JOIN_NETWORK, StrId::STR_JOIN_NETWORK, StrId::STR_JOIN_DESC, networkModeIconWifi},
    {NetworkMode::CONNECT_CALIBRE, StrId::STR_CALIBRE_WIRELESS, StrId::STR_CALIBRE_DESC, networkModeIconLibrary},
    {NetworkMode::OPDS_BROWSER, StrId::STR_OPDS_BROWSER, StrId::STR_OPDS_DESC, networkModeIconOpds},
    {NetworkMode::CREATE_HOTSPOT, StrId::STR_CREATE_HOTSPOT, StrId::STR_HOTSPOT_DESC, networkModeIconHotspot},
    {USB_MODE, USB_MODE_LABEL, USB_MODE_DESC, USB_MODE_ICON},
};
static_assert(std::size(kRows) == NetworkModeSelectionActivity::MAX_ROWS);

// The OPDS browser has nothing to open until a server is configured (Settings > System > OPDS
// Servers, or the web settings page).
bool isListed(const ModeRow& row) { return row.mode != NetworkMode::OPDS_BROWSER || OPDS_STORE.hasServers(); }

// Opens a mode that has a screen of its own (activateIndex()).
//
// A Join Network the user backed out of (Back while scanning, connecting or at the list) leaves the
// radio up: CrossPointWebServerActivity starts it before the join and takes it down only in its
// onExit(), whose ServerSession::end() restarts to Home. Replacing the stack runs that onExit(), so
// the choice was lost to a restart to Home. So the session ends here (wifi_session::end()): where it
// says reboot (the C3s), this restarts into the choice; elsewhere the radio is off and the choice
// opens in this boot, and ServerSession::end() finds nothing left to undo. A silentRestart*()
// returns only while a deep sleep supersedes it.
void handOff(const NetworkMode mode) {
  const bool restart = WiFi.getMode() != WIFI_MODE_NULL && wifi_session::end("NETMODE") == wifi_session::Exit::Reboot;
  if (restart) LOG_DBG("NETMODE", "Radio left up by a cancelled join; restarting into the choice");
  switch (mode) {
    case NetworkMode::OPDS_BROWSER:
      if (restart) {
        silentRestartToOpdsBrowser();
      } else {
        activityManager.goToBrowser();
      }
      return;
    case NetworkMode::USB_SERIAL:
      if (restart) {
        silentRestartToSerialTransfer();
      } else {
        activityManager.goToSerialTransfer();
      }
      return;
    case NetworkMode::USB_DRIVE:
      if (restart) {
        silentRestartToUsbDrive();
      } else {
        activityManager.goToUsbDrive();
      }
      return;
    case NetworkMode::JOIN_NETWORK:
    case NetworkMode::CONNECT_CALIBRE:
    case NetworkMode::CREATE_HOTSPOT:
      return;  // not hand-offs: these go back to CrossPointWebServerActivity
  }
}

// A 32 px icon from networkModeIcons.h as the bitmap a list row draws (Mask1: bit 0 = ink).
fui::BitmapRef rowIcon(const uint8_t* bits) {
  fui::BitmapRef icon;
  icon.data = bits;
  icon.width = 32;
  icon.height = 32;
  icon.format = fui::BitmapFormat::Mask1;
  return icon;
}
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           const NetworkMode selected)
    : UiListActivity("NetworkModeSelection", renderer, mappedInput), selectedOnEnter(selected) {
  for (const ModeRow& row : kRows) {
    if (!isListed(row)) continue;
    fui::ListItem item;
    item.label = I18N.get(row.label);
    item.subtitle = I18N.get(row.desc);
    item.icon = rowIcon(row.icon);
    item.actionValue = static_cast<int16_t>(rowCount);
    rowItems[rowCount] = item;
    rowModes[rowCount] = row.mode;
    rowCount++;
  }
}

void NetworkModeSelectionActivity::onEnter() {
  // The base resets the selection to the top row; move it after.
  UiListActivity::onEnter();
  for (int i = 1; i < rowCount; i++) {
    if (rowModes[i] == selectedOnEnter) {
      moveSelectionTo(i);
      break;
    }
  }
}

const char* NetworkModeSelectionActivity::headerTitle() const { return tr(STR_FILE_TRANSFER); }

void NetworkModeSelectionActivity::activateIndex(const int index) {
  // Every choice leaves this screen; a lingering tap flash would gray an unrelated element on the
  // next screen's first render.
  app.clearTapFlash();
  nav.selected = index;
  const NetworkMode mode = rowModes[index];

  // Neither USB mode needs WiFi or the web server, so hand off directly here
  // instead of routing the result back through the WiFi-centric
  // CrossPointWebServerActivity. The OPDS browser does need WiFi, but brings it up
  // itself and reboots on exit, so it is handed off the same way. The web server
  // modes still return to that owner.
  switch (mode) {
    case NetworkMode::OPDS_BROWSER:
    case NetworkMode::USB_SERIAL:
    case NetworkMode::USB_DRIVE:
      handOff(mode);
      return;
    case NetworkMode::JOIN_NETWORK:
    case NetworkMode::CONNECT_CALIBRE:
    case NetworkMode::CREATE_HOTSPOT:
      onModeSelected(mode);
      return;
  }
}

void NetworkModeSelectionActivity::buildScreen(UiScreen& screen) {
  layoutListArea(screen);
  auto props = listProps(screen);
  props.items = rowItems;
  props.count = static_cast<uint16_t>(rowCount);
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  addList(screen, props, /*hasSubtitle=*/true);
}

void NetworkModeSelectionActivity::onModeSelected(NetworkMode mode) {
  setResult(NetworkModeResult{mode});
  finish();
}

void NetworkModeSelectionActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
