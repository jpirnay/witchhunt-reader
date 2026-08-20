#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Structure to hold WiFi network information
struct WifiNetworkInfo {
  std::string ssid;
  int32_t rssi;
  bool isEncrypted;
  bool hasSavedPassword;  // Whether we have saved credentials for this network
  std::string ipAddress;  // Populated after connection for display
};

// WiFi selection states
enum class WifiSelectionState {
  AUTO_CONNECTING,    // Trying to connect to the last known network
  AUTO_CYCLING,       // Cycling through remaining saved credentials after AUTO_CONNECTING failed
  SCANNING,           // Scanning for networks
  NETWORK_LIST,       // Displaying available networks
  PASSWORD_ENTRY,     // Entering password for selected network
  CONNECTING,         // Attempting to connect
  CONNECTED,          // Successfully connected
  SAVE_PROMPT,        // Asking user if they want to save the password
  CONNECTION_FAILED,  // Connection failed
  FORGET_PROMPT,      // Asking user if they want to forget the network
  CAPTIVE_PORTAL      // Connected but network requires web-based login
};

/**
 * WifiSelectionActivity is responsible for scanning WiFi APs and connecting to them.
 * It will:
 * - Enter scanning mode on entry
 * - List available WiFi networks
 * - Allow selection and launch KeyboardEntryActivity for password if needed
 * - Save the password if requested
 * - Call onComplete callback when connected or cancelled
 *
 * The onComplete callback receives true if connected successfully, false if cancelled.
 */
class WifiSelectionActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  WifiSelectionState state = WifiSelectionState::SCANNING;
  int selectedNetworkIndex = 0;
  std::vector<WifiNetworkInfo> networks;

  // Selected network for connection
  std::string selectedSSID;
  bool selectedRequiresPassword = false;

  // Connection result
  std::string connectedIP;
  std::string connectionError;

  // Password to potentially save (from keyboard or saved credentials)
  std::string enteredPassword;

  // Cached MAC address string for display
  std::string cachedMacAddress;

  // Whether network was connected using a saved password (skip save prompt)
  bool usedSavedPassword = false;

  // Whether to attempt auto-connect on entry
  const bool allowAutoConnect;

  // Whether we are attempting to auto-connect
  bool autoConnecting = false;

  // Saved-credential candidates for auto-cycling (SSIDs visible in scan, sorted by RSSI desc)
  std::vector<std::string> autoCycleCandidates;
  size_t autoCycleCandidateIndex = 0;
  bool autoCycleAfterScan = false;  // Scan was triggered to build cycle candidates

  // Save/forget prompt selection (0 = Yes, 1 = No)
  int savePromptSelection = 0;
  int forgetPromptSelection = 0;

  // Connection timeouts
  static constexpr unsigned long CONNECTION_TIMEOUT_MS = 15000;
  static constexpr unsigned long AUTO_CYCLE_TIMEOUT_MS = 5000;
  // Backstop for the hint attempt before falling back to a full scan.
  //
  // The premise this was built on ("hint-based connect completes in <2 s") does not hold on real
  // APs. Measured on an X4 across six sessions, association is 2517/2518/2535/2569/2571/2615 ms
  // and is INDIFFERENT to the hint and to the scan method -- the scan is not what dominates it,
  // auth/assoc is. At 3000 ms that left ~400 ms of margin over the normal case, so ordinary
  // jitter tripped it and paid the timeout plus a full re-association (~6.6 s vs ~3.6 s).
  //
  // A stale hint no longer needs a timeout to be detected: since issueWifiBegin() uses
  // WIFI_FAST_SCAN for hinted attempts, an AP that has moved off the cached channel yields
  // NO_AP_FOUND -> WL_NO_SSID_AVAIL almost immediately and trips the hintHardFail path instead.
  // This value now only catches an attempt that hangs without ever reporting a hard status, so
  // it belongs well clear of normal association time rather than just above it.
  //
  // Safe to enlarge: the fallback resets connectionStartTime, so it gets its own full
  // CONNECTION_TIMEOUT_MS budget and this does not eat into it.
  static constexpr unsigned long HINT_ATTEMPT_TIMEOUT_MS = 8000;
  unsigned long connectionStartTime = 0;

  // BSSID/channel hint used on the current attempt (channel==0 means no hint).
  uint8_t currentAttemptBssid[6] = {0};
  uint8_t currentAttemptChannel = 0;
  volatile bool currentAttemptAssociated = false;
  // Whether we've already done the silent fallback retry without the hint for this
  // user-initiated connection. Prevents loops if the AP genuinely isn't reachable.
  bool hintFallbackDone = false;

  // WiFi event handler IDs so we can deregister on exit.
  uint16_t evtIdConnected = 0;
  uint16_t evtIdGotIp = 0;
  uint16_t evtIdStaStart = 0;
  uint16_t evtIdDisconnected = 0;

  void renderNetworkList() const;
  void renderPasswordEntry() const;
  void renderConnecting() const;
  void renderConnected() const;
  void renderSavePrompt() const;
  void renderConnectionFailed() const;
  void renderForgetPrompt() const;
  void renderCaptivePortal() const;

  void startWifiScan();
  void processWifiScanResults();
  void buildAutoCycleCandidates();
  void tryNextAutoCycleCandidate();
  void selectNetwork(int index);
  void attemptConnection();
  void checkConnectionStatus();
  // Issues WiFi.begin() either with the cached BSSID/channel hint (fast path) or without
  // (full scan fallback). `useHint=false` clears currentAttemptChannel so the success path
  // doesn't double-store the same hint.
  void issueWifiBegin(bool useHint);
  // Prepares the WiFi stack for a connect attempt: ensures STA mode and a clean state,
  // sets a deterministic hostname. Skips the expensive disconnect(true,true) when WiFi
  // is already idle so the warm reconnect path doesn't pay an NVS-erase cost.
  void prepareForConnect();
  bool checkCaptivePortal();
  std::string getSignalStrengthIndicator(int32_t rssi) const;

  std::string captivePortalUrl;

  void onComplete(bool connected);

 public:
  explicit WifiSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool autoConnect = true)
      : Activity("WifiSelection", renderer, mappedInput), allowAutoConnect(autoConnect) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
