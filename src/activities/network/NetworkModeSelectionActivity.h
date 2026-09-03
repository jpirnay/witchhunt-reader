#pragma once

#include <functional>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

// USB_SERIAL and USB_DRIVE are mutually exclusive in the menu: a board that can
// present itself as a USB mass-storage device has no use for the serial
// file-transfer protocol, so it offers the drive instead (see the menu tables
// in the .cpp). Both enumerators exist in every build; only the listing differs.
enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, USB_SERIAL, USB_DRIVE };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 */
class NetworkModeSelectionActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;

 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("NetworkModeSelection", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  // Tap on a list row -> move the selection there; ActivityManager then synthesizes Confirm.
  ListRowTap::Result selectListRow(int index) override;
  void loop() override;
  void render(RenderLock&&) override;

  void onModeSelected(NetworkMode mode);
  void onCancel();
};
