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
  AUTO_CONNECTING,     // Trying to connect to the last known network
  AUTO_CYCLING,        // Cycling through remaining saved credentials after AUTO_CONNECTING failed
  SCANNING,            // Scanning for networks
  HOME_CHANNEL_PROBE,  // Short scan of the last-known-good channel, before begin()
  NETWORK_LIST,        // Displaying available networks
  PASSWORD_ENTRY,      // Entering password for selected network
  CONNECTING,          // Attempting to connect
  CONNECTED,           // Successfully connected
  SAVE_PROMPT,         // Asking user if they want to save the password
  CONNECTION_FAILED,   // Connection failed
  FORGET_PROMPT,       // Asking user if they want to forget the network
  CAPTIVE_PORTAL       // Connected but network requires web-based login
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
  // Connect-time scan budget, applied via esp_wifi_set_scan_parameters() (see applyScanBudget()).
  // Scan duration is CHANNELS x DWELL and independent of SSID density; 80 ms puts a 13-channel
  // sweep at ~1040 ms against ~1560 ms at the IDF default of 120 ms, while keeping margin for an
  // AP that is slow to answer a probe. Too short lands back in NO_AP_FOUND plus a driver retry.
  // min is the REAL listening floor: the station leaves a channel after min ms unless it has
  // already heard an AP, in which case it may stay until max. A min of 0 therefore does not mean
  // "use max", it means "do not wait" -- measured directly: a probe with min=100/max=300 came
  // back in 101 ms. Keep them equal so a sweep is deterministic (13 x 120 ms ~ 1560 ms).
  static constexpr uint32_t SCAN_ACTIVE_DWELL_MIN_MS = 120;
  static constexpr uint32_t SCAN_ACTIVE_DWELL_MAX_MS = 120;
  static constexpr uint32_t SCAN_PASSIVE_DWELL_MS = 200;
  // Documented minimum; only relevant while already associated, which this path is not.
  static constexpr uint8_t SCAN_HOME_CHAN_DWELL_MS = 30;

  // Home-channel probe (see startHomeChannelProbe()). A full sweep gives the AP a single dwell
  // on its own channel out of ~13, so one delayed beacon or lost probe response there fails the
  // whole attempt. 300 ms on one channel is both cheaper than the sweep and three beacon
  // intervals deep instead of barely one.
  static constexpr uint32_t HOME_PROBE_DWELL_MS = 300;
  // Arduino's WiFiScanClass default for active.min, restored after the probe overrides it so the
  // network-list sweep is not silently lengthened to 13 x 300 ms.
  static constexpr uint32_t ARDUINO_SCAN_ACTIVE_MIN_DEFAULT_MS = 100;
  // Ceiling on the async scan before we give up on it and take the full sweep. Generous against
  // the dwell so only a genuinely stuck scan trips it.
  static constexpr unsigned long HOME_PROBE_TIMEOUT_MS = 1500;
  // Active probes and listens; passive only listens for beacons. Active is the default because
  // it also hears an AP that beacons late in the window. If the probe logs show our SSID missing
  // while other APs on the channel are visible, that is probe-response suppression and this
  // should go passive -- beacons cannot be suppressed that way.
  static constexpr bool HOME_PROBE_PASSIVE = false;
  // Upper bound on a sane cached channel hint; anything above it is a corrupt record.
  static constexpr uint8_t MAX_2G4_CHANNEL = 14;

  unsigned long connectionStartTime = 0;

  // Set by the STA_CONNECTED handler; read by the disconnect log to tell a mid-connect failure
  // apart from a drop after association.
  volatile bool currentAttemptAssociated = false;

  // True while the outstanding begin() is restricted to the channel the probe just heard.
  bool currentAttemptPinned = false;
  // Raised by the disconnect handler when a pinned attempt came back NO_AP_FOUND, and consumed
  // in checkConnectionStatus(). The handler runs in the WiFi event task, so it only sets a flag:
  // re-issuing begin() from there would call into the driver from its own callback.
  volatile bool pinnedAttemptMissed = false;

  unsigned long homeProbeStartMs = 0;
  uint8_t homeProbeChannel = 0;

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
  // Issues WiFi.begin() with a full, signal-sorted scan. See the definition for why the cached
  // channel/BSSID hint that used to shortcut this was removed.
  void issueWifiBegin();
  // Channel-restricted variant: WIFI_FAST_SCAN on the one channel the home-channel probe just
  // heard the AP on. Deliberately does NOT pin the BSSID; see the definition.
  void issueWifiBeginOnChannel(uint8_t channel);
  // Short single-channel scan of the last-known-good channel. Returns false when there is no
  // usable hint or the scan could not start, in which case the caller takes the full sweep.
  bool startHomeChannelProbe();
  // Polls the probe scan and either pins what it found or falls back to the full sweep.
  void processHomeChannelProbe();
  // Bound the connect-time scan; see the definition.
  void applyScanBudget();
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
