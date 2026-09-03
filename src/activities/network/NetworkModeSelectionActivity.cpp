#include "NetworkModeSelectionActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEM_COUNT = 4;

// The fourth row is the USB transfer method this board actually has. Boards
// that can act as a USB mass-storage device (FREEINK_CAP_USB_MSC) show "USB
// Drive"; the rest keep the serial protocol.
#if FREEINK_CAP_USB_MSC
constexpr NetworkMode USB_MODE = NetworkMode::USB_DRIVE;
constexpr StrId USB_MODE_LABEL = StrId::STR_USB_DRIVE;
constexpr StrId USB_MODE_DESC = StrId::STR_USB_DRIVE_DESC;
constexpr UIIcon USB_MODE_ICON = UIIcon::Usb;
#else
constexpr NetworkMode USB_MODE = NetworkMode::USB_SERIAL;
constexpr StrId USB_MODE_LABEL = StrId::STR_USB_TRANSFER;
constexpr StrId USB_MODE_DESC = StrId::STR_USB_TRANSFER_DESC;
constexpr UIIcon USB_MODE_ICON = UIIcon::Transfer;
#endif
}  // namespace

void NetworkModeSelectionActivity::onEnter() {
  Activity::onEnter();

  // Reset selection
  selectedIndex = 0;

  // Trigger first update
  requestUpdate();
}

void NetworkModeSelectionActivity::onExit() { Activity::onExit(); }

void NetworkModeSelectionActivity::loop() {
  // Handle back button - cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
    return;
  }

  // Handle confirm button - select current option
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    NetworkMode mode = NetworkMode::JOIN_NETWORK;
    if (selectedIndex == 1) {
      mode = NetworkMode::CONNECT_CALIBRE;
    } else if (selectedIndex == 2) {
      mode = NetworkMode::CREATE_HOTSPOT;
    } else if (selectedIndex == 3) {
      mode = USB_MODE;
    }

    // Neither USB mode needs WiFi or the web server, so hand off directly here
    // instead of routing the result back through the WiFi-centric
    // CrossPointWebServerActivity. The WiFi modes still return to that owner.
    if (mode == NetworkMode::USB_SERIAL) {
      activityManager.goToSerialTransfer();
      return;
    }
    if (mode == NetworkMode::USB_DRIVE) {
      activityManager.goToUsbDrive();
      return;
    }

    onModeSelected(mode);
    return;
  }

  // Handle navigation
  buttonNavigator.onNextList(selectedIndex, MENU_ITEM_COUNT, [this] { requestUpdate(); });
  buttonNavigator.onPreviousList(selectedIndex, MENU_ITEM_COUNT, [this] { requestUpdate(); });
}

void NetworkModeSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_FILE_TRANSFER));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing * 2;
  // Menu items and descriptions
  static constexpr StrId menuItems[MENU_ITEM_COUNT] = {StrId::STR_JOIN_NETWORK, StrId::STR_CALIBRE_WIRELESS,
                                                       StrId::STR_CREATE_HOTSPOT, USB_MODE_LABEL};
  static constexpr StrId menuDescs[MENU_ITEM_COUNT] = {StrId::STR_JOIN_DESC, StrId::STR_CALIBRE_DESC,
                                                       StrId::STR_HOTSPOT_DESC, USB_MODE_DESC};
  static constexpr UIIcon menuIcons[MENU_ITEM_COUNT] = {UIIcon::Wifi, UIIcon::Library, UIIcon::Hotspot, USB_MODE_ICON};

  GUI.drawList(
      renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight}, static_cast<int>(MENU_ITEM_COUNT),
      selectedIndex, [](int index) { return std::string(I18N.get(menuItems[index])); },
      [](int index) { return std::string(I18N.get(menuDescs[index])); }, [](int index) { return menuIcons[index]; });

  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
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

ListRowTap::Result NetworkModeSelectionActivity::selectListRow(const int index) {
  return ListRowTap::apply(index, static_cast<int>(MENU_ITEM_COUNT), selectedIndex);
}
