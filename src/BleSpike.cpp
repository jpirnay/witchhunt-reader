// SPIKE ONLY (branch chore/ble-c3-spike, never for merge): what Bluetooth LE costs on the C3.
//
// CMD:BLE:CTRL      Espressif's controller alone, driven over VHCI with raw HCI (reset, event mask, a 3 s passive
//                   scan): the floor that any host sits on, NimBLE or a hand-written keypress-only one.
// CMD:BLE:CTRL_OFF  controller disable + deinit.
// CMD:BLE:HOST      the SDK's BleKeyboardHost (NimBLE-Arduino's host on the same controller): begin, a 3 s scan.
// CMD:BLE:HOST_OFF  BleHid.end().
// Link test with a real remote (the host stays up between commands, polled from SerialTestApi::service()):
// CMD:BLE:SCAN[=ms]        scan, list named or HID devices [addr|name|rssi|type|hid|connectable].
// CMD:BLE:CONNECT=addr     connect and bond (Just Works); waits up to 30 s.
// CMD:BLE:RECONNECT[=ms]   begin() if down, arm the bonded reconnect, wait for the link: a radio restart.
// CMD:BLE:KEYS[=ms]        wait, then return and clear the log: link up/down, link_ms, keys, raw edges, times.
// CMD:BLE:LINK, FORGET=addr, DISCONNECT.
// CMD:BLE:KEEP=0|1  whether the next boot keeps the controller's memory (initArduino() releases it when bleInUse()
//                   says no); takes effect after CMD:REBOOT. CMD:BLE:MEM reports the current state.
//
// Each reply lists free/largest-free-block after every step, and the since-boot minimum. REBOOT between runs.
//
// With `custom_ble_spike_controller = flash` (scripts/ble_spike_flash_ctrl.py) the build links Espressif's
// flash-only controller instead; BLE_SPIKE_FLASH_CTRL then compiles the stand-ins for flash-only bt.c below. CTRL and
// HOST both report run_in_flash/irq_iram_cleared, the proof that the stand-ins took effect.
#include <Arduino.h>
#include <BleKeyboardHost.h>
#include <HalSystem.h>
#include <esp32-hal-bt.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_intr_alloc.h>
#include <esp_memory_utils.h>
#include <esp_system.h>
#include <nvs.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

// Strong override of the core's weak bleInUse() (esp32-hal-bt.c): initArduino() calls it right after
// nvs_flash_init() and releases the controller's memory for good when it returns false. Read from NVS, so one
// firmware measures both: memory kept (the default, as with NimBLE linked) and released (as master).
extern "C" bool bleInUse(void) {
  uint8_t keep = 1;
  nvs_handle_t handle;
  if (nvs_open("blespike", NVS_READONLY, &handle) == ESP_OK) {
    nvs_get_u8(handle, "keep", &keep);
    nvs_close(handle);
  }
  return keep != 0;
}

#ifndef BLE_SPIKE_FLASH_CTRL
#define BLE_SPIKE_FLASH_CTRL 0
#endif

void bleSpikeAcceptConnParams(bool on);  // freeink-sdk BleKeyboardHost.cpp (spike copy only)
bool bleSpikeConnParams(uint16_t& interval, uint16_t& latency, uint16_t& timeout);

namespace {
// Controller interrupts whose IRAM flag the wrapper below cleared (flash-only controller only).
std::atomic<uint32_t> irqFlagsCleared{0};
// Set around esp_bt_controller_init() in controllerOn(): the only window in which the wrapper touches a call.
std::atomic<bool> inControllerInit{false};
}  // namespace

#if BLE_SPIKE_FLASH_CTRL
// The flash-only controller (scripts/ble_spike_flash_ctrl.py) with the prebuilt bt.c, which was compiled for the
// ROM-assisted one. Flash-only bt.c (IDF v5.5.5 components/bt/controller/esp32c3/bt.c) skips these ROM-table resets
// (line 1355), and the flash-only library does not define them.
extern "C" void ble_dtm_funcs_reset(void) {}
extern "C" void ble_scan_funcs_reset(void) {}
extern "C" void ble_42_adv_funcs_reset(void) {}
extern "C" void ble_init_funcs_reset(void) {}
extern "C" void ble_con_funcs_reset(void) {}
extern "C" void ble_ext_adv_funcs_reset(void) {}
extern "C" void ble_ext_scan_funcs_reset(void) {}
extern "C" void ble_base_funcs_reset(void) {}
extern "C" void ble_enc_funcs_reset(void) {}

// Flash-only bt.c allocates the controller interrupt without ESP_INTR_FLAG_IRAM (line 874): its handler is in flash,
// and IDF refuses an IRAM-flagged interrupt whose handler is not in IRAM. Linked with -Wl,--wrap=esp_intr_alloc;
// only calls made inside esp_bt_controller_init() for a handler outside IRAM are changed.
extern "C" esp_err_t __real_esp_intr_alloc(int source, int flags, intr_handler_t handler, void* arg,
                                           intr_handle_t* ret_handle);
extern "C" esp_err_t __wrap_esp_intr_alloc(int source, int flags, intr_handler_t handler, void* arg,
                                           intr_handle_t* ret_handle) {
  if (inControllerInit.load() && (flags & ESP_INTR_FLAG_IRAM) && handler != nullptr &&
      !esp_ptr_in_iram(reinterpret_cast<const void*>(handler))) {
    flags &= ~ESP_INTR_FLAG_IRAM;
    irqFlagsCleared.fetch_add(1);
  }
  return __real_esp_intr_alloc(source, flags, handler, arg, ret_handle);
}
#endif

namespace {

void appendHeap(std::string& out, const char* tag) {
  char buf[64];
  snprintf(buf, sizeof(buf), " %s=%lu/%lu", tag, static_cast<unsigned long>(esp_get_free_heap_size()),
           static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  out += buf;
}

void appendNumber(std::string& out, const char* tag, long value) {
  char buf[48];
  snprintf(buf, sizeof(buf), " %s=%ld", tag, value);
  out += buf;
}

void appendMin(std::string& out) { appendNumber(out, "min", static_cast<long>(esp_get_minimum_free_heap_size())); }

std::atomic<uint32_t> hciEvents{0};
std::atomic<uint32_t> advReports{0};
std::atomic<uint32_t> commandsDone{0};

void onSendAvailable() {}

int onPacket(uint8_t* data, uint16_t len) {
  if (len >= 3 && data[0] == 0x04) {  // H4 event
    hciEvents.fetch_add(1);
    if (data[1] == 0x0E || data[1] == 0x0F) commandsDone.fetch_add(1);                  // Command Complete / Status
    if (data[1] == 0x3E && len >= 5 && data[3] == 0x02) advReports.fetch_add(data[4]);  // LE Advertising Report
  }
  return 0;
}

esp_vhci_host_callback_t vhciCallbacks = {onSendAvailable, onPacket};

bool hciCommand(uint16_t opcode, const uint8_t* params, uint8_t length) {
  uint8_t packet[4 + 16];
  if (length > 16) return false;
  packet[0] = 0x01;  // H4 command
  packet[1] = static_cast<uint8_t>(opcode & 0xFF);
  packet[2] = static_cast<uint8_t>(opcode >> 8);
  packet[3] = length;
  if (length) memcpy(packet + 4, params, length);
  for (int i = 0; i < 200 && !esp_vhci_host_check_send_available(); ++i) delay(1);
  const uint32_t before = commandsDone.load();
  esp_vhci_host_send_packet(packet, static_cast<uint16_t>(4 + length));
  for (int i = 0; i < 500; ++i) {
    if (commandsDone.load() != before) return true;
    delay(1);
  }
  return false;
}

bool controllerUp = false;

std::string controllerOn() {
  std::string out = "BLE_CTRL:";
  appendHeap(out, "pre");
  if (controllerUp) return out + " error=already_up";
  // With the flash-only controller, scripts/ble_spike_nimble_c3.h defines CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY, so the
  // macro sets .run_in_flash = 1.
  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  appendNumber(out, "run_in_flash", cfg.run_in_flash);
  const unsigned long t0 = millis();
  inControllerInit.store(true);
  esp_err_t err = esp_bt_controller_init(&cfg);
  inControllerInit.store(false);
  appendNumber(out, "flash_ctrl", BLE_SPIKE_FLASH_CTRL);
  appendNumber(out, "irq_iram_cleared", static_cast<long>(irqFlagsCleared.load()));
  if (err != ESP_OK) {
    appendNumber(out, "init_err", err);
    return out;
  }
  appendHeap(out, "init");
  err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (err != ESP_OK) {
    appendNumber(out, "enable_err", err);
    esp_bt_controller_deinit();
    return out;
  }
  controllerUp = true;
  appendNumber(out, "up_ms", static_cast<long>(millis() - t0));
  appendHeap(out, "enable");
  esp_vhci_host_register_callback(&vhciCallbacks);

  int ok = 0;
  ok += hciCommand(0x0C03, nullptr, 0);                                           // Reset
  const uint8_t eventMask[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x00, 0x20};  // default + LE Meta
  ok += hciCommand(0x0C01, eventMask, sizeof(eventMask));
  const uint8_t scanParams[7] = {0x00, 0x60, 0x00, 0x30, 0x00, 0x00, 0x00};  // passive, 60 ms / 30 ms
  ok += hciCommand(0x200B, scanParams, sizeof(scanParams));
  const uint8_t scanOn[2] = {0x01, 0x00};
  ok += hciCommand(0x200C, scanOn, sizeof(scanOn));
  appendHeap(out, "scanning");
  delay(3000);
  appendHeap(out, "scan3s");
  const uint8_t scanOff[2] = {0x00, 0x00};
  ok += hciCommand(0x200C, scanOff, sizeof(scanOff));
  appendNumber(out, "cmds_ok", ok);
  appendNumber(out, "events", static_cast<long>(hciEvents.load()));
  appendNumber(out, "adv", static_cast<long>(advReports.load()));
  appendHeap(out, "idle");
  appendMin(out);
  return out;
}

std::string controllerOff() {
  std::string out = "BLE_CTRL_OFF:";
  appendHeap(out, "pre");
  if (!controllerUp) return out + " error=not_up";
  esp_bt_controller_disable();
  for (int i = 0; i < 200 && esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_INITED; ++i) delay(5);
  const esp_err_t err = esp_bt_controller_deinit();
  appendNumber(out, "deinit_err", err);
  controllerUp = false;
  appendHeap(out, "off");
  appendMin(out);
  return out;
}

std::string hostOn() {
  std::string out = "BLE_HOST:";
  appendHeap(out, "pre");
  if (controllerUp) return out + " error=controller_up";
  const unsigned long t0 = millis();
  inControllerInit.store(true);  // NimBLE initialises the controller inside begin()
  const bool begun = BleHid.begin("WitchSpike");
  inControllerInit.store(false);
  appendNumber(out, "flash_ctrl", BLE_SPIKE_FLASH_CTRL);
  appendNumber(out, "irq_iram_cleared", static_cast<long>(irqFlagsCleared.load()));
  if (!begun) {
    appendHeap(out, "begin_failed");
    return out;
  }
  appendNumber(out, "up_ms", static_cast<long>(millis() - t0));
  appendHeap(out, "begin");
  BleHid.startScan(3000);
  appendHeap(out, "scanning");
  const unsigned long scanStart = millis();
  while (millis() - scanStart < 3500) {
    BleHid.poll();
    delay(20);
  }
  appendHeap(out, "scan3s");
  appendNumber(out, "devices", BleHid.deviceCount());
  BleHid.stopScan();
  BleHid.releaseScanResults();
  appendHeap(out, "idle");
  appendMin(out);
  return out;
}

std::string hostOff() {
  std::string out = "BLE_HOST_OFF:";
  appendHeap(out, "pre");
  const unsigned long t0 = millis();
  bool done = false;
  while (!done && millis() - t0 < 4000) {
    done = BleHid.end(1000);
    if (!done) delay(50);
  }
  appendNumber(out, "ended", done ? 1 : 0);
  appendNumber(out, "end_ms", static_cast<long>(millis() - t0));
  appendHeap(out, "off");
  appendMin(out);
  return out;
}

// ---- Link test with a real remote (S3 probe) -------------------------------------------------------------------
// The log below is filled by bleSpikeService() (called from SerialTestApi::service() on the loop task, and from the
// wait loops of the commands, which run on the same task) and emptied by CMD:BLE:KEYS. Nothing else touches it.

void appendInternal(std::string& out, const char* tag) {
  constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  char buf[64];
  snprintf(buf, sizeof(buf), " %s=%lu/%lu", tag, static_cast<unsigned long>(heap_caps_get_free_size(caps)),
           static_cast<unsigned long>(heap_caps_get_largest_free_block(caps)));
  out += buf;
}

struct LogEntry {
  uint32_t atMs;
  char text[44];
};
constexpr int kLogLen = 64;
LogEntry logRing[kLogLen];
int logCount = 0;
uint32_t logDropped = 0;
bool lastConnected = false;
uint32_t armedAtMs = 0;  // start of the last CONNECT or RECONNECT; 0 once its link came up

__attribute__((format(printf, 1, 2))) void logEvent(const char* format, ...) {
  if (logCount == kLogLen) {
    ++logDropped;
    return;
  }
  LogEntry& entry = logRing[logCount++];
  entry.atMs = millis();
  va_list args;
  va_start(args, format);
  vsnprintf(entry.text, sizeof(entry.text), format, args);
  va_end(args);
}

void serviceLink() {
  if (!BleHid.isRunning()) {
    lastConnected = false;
    return;
  }
  BleHid.poll();
  const bool connected = BleHid.isConnected();
  if (connected != lastConnected) {
    lastConnected = connected;
    if (connected) {
      logEvent("up %s", BleHid.connectedName());
      if (armedAtMs != 0) {
        logEvent("link_ms=%lu", static_cast<unsigned long>(millis() - armedAtMs));
        armedAtMs = 0;
      }
    } else {
      logEvent("down");
    }
  }
  char failure[48];
  if (BleHid.takeConnectFailure(failure, sizeof(failure))) logEvent("fail %.36s", failure);
  uint32_t passkey = 0;
  if (BleHid.takePairingPasskey(passkey)) logEvent("passkey %06lu", static_cast<unsigned long>(passkey));
  freeink::KeyEvent key;
  while (BleHid.popKey(key))
    logEvent("key kc=%02x sp=%u ch=%u m=%02x", key.keycode, static_cast<unsigned>(key.special),
             static_cast<unsigned>(static_cast<uint8_t>(key.ch)), key.mods);
  freeink::RawButtonEvent raw;
  while (BleHid.popRawButton(raw))
    logEvent("raw %06lx %s%s @%lu", static_cast<unsigned long>(raw.code()), raw.pressed ? "dn" : "up",
             raw.wasRest ? " rest" : "", static_cast<unsigned long>(raw.atMs));
}

// Services the link for up to `ms`, or until done() says so.
template <typename Done>
void serviceFor(const uint32_t ms, Done done) {
  const uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    serviceLink();
    HalSystem::feedWatchdog();
    if (done()) return;
    delay(10);
  }
}

uint32_t parseMs(const std::string_view value, const uint32_t fallback, const uint32_t cap) {
  if (value.empty()) return fallback;
  uint32_t ms = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') return fallback;
    ms = ms * 10 + static_cast<uint32_t>(c - '0');
    if (ms > cap) return cap;
  }
  return ms;
}

// begin() when the host is down; its time in begin_ms (0 when it was already up).
bool ensureHost(std::string& out) {
  if (BleHid.isRunning()) {
    appendNumber(out, "begin_ms", 0);
    return true;
  }
  const uint32_t t0 = millis();
  const bool begun = BleHid.begin("WitchSpike");
  appendNumber(out, "begin_ms", static_cast<long>(millis() - t0));
  if (!begun) out += " error=begin_failed";
  return begun;
}

std::string scan(const std::string_view value) {
  std::string out = "BLE_SCAN:";
  if (!ensureHost(out)) return out;
  const uint32_t ms = parseMs(value, 8000, 30000);
  BleHid.startScan(ms);
  serviceFor(ms + 500, [] { return false; });
  BleHid.stopScan();
  appendNumber(out, "devices", BleHid.deviceCount());
  for (uint8_t i = 0; i < BleHid.deviceCount(); ++i) {
    const freeink::DiscoveredDevice& d = BleHid.device(i);
    if (!d.hid && !d.hasName) continue;  // keep the reply to what could be a remote
    char buf[96];
    snprintf(buf, sizeof(buf), " [%s|%s|%d|t%u|hid%d|c%d]", d.addr, d.name, d.rssi, d.addrType, d.hid ? 1 : 0,
             d.connectable ? 1 : 0);
    out += buf;
  }
  appendInternal(out, "internal");
  return out;
}

std::string connectTo(const std::string_view addr) {
  std::string out = "BLE_CONNECT:";
  if (addr.size() != 17) return out + " error=address";
  if (!ensureHost(out)) return out;
  char address[18];
  memcpy(address, addr.data(), 17);
  address[17] = '\0';
  armedAtMs = millis();
  const uint32_t t0 = armedAtMs;
  const bool started = BleHid.connect(address);
  appendNumber(out, "started", started ? 1 : 0);
  serviceFor(30000, [] { return BleHid.isConnected(); });
  appendNumber(out, "connected", BleHid.isConnected() ? 1 : 0);
  appendNumber(out, "ms", static_cast<long>(millis() - t0));
  out += " name=";
  out += BleHid.connectedName();
  appendNumber(out, "bonds", BleHid.pairedCount());
  appendInternal(out, "internal");
  return out;
}

// Starts the host if it is down and arms the bonded reconnect: the restart a reader would do after stopping the
// radio. link_ms in the log counts from the arm.
std::string reconnect(const std::string_view value) {
  std::string out = "BLE_RECONNECT:";
  if (!ensureHost(out)) return out;
  armedAtMs = millis();
  const uint32_t t0 = armedAtMs;
  const bool armed = BleHid.armBondedReconnect(freeink::PickPolicy::First, nullptr);
  appendNumber(out, "armed", armed ? 1 : 0);
  serviceFor(parseMs(value, 30000, 125000), [] { return BleHid.isConnected(); });
  appendNumber(out, "connected", BleHid.isConnected() ? 1 : 0);
  appendNumber(out, "ms", static_cast<long>(millis() - t0));
  appendInternal(out, "internal");
  return out;
}

std::string keys(const std::string_view value) {
  serviceFor(parseMs(value, 0, 120000), [] { return false; });
  serviceLink();
  std::string out = "BLE_KEYS:";
  appendNumber(out, "now", static_cast<long>(millis()));
  appendNumber(out, "connected", BleHid.isConnected() ? 1 : 0);
  for (int i = 0; i < logCount; ++i) {
    char buf[64];
    snprintf(buf, sizeof(buf), " [%lu %s]", static_cast<unsigned long>(logRing[i].atMs), logRing[i].text);
    out += buf;
  }
  appendNumber(out, "dropped", static_cast<long>(logDropped));
  logCount = 0;
  logDropped = 0;
  return out;
}

std::string link() {
  std::string out = "BLE_LINK:";
  appendNumber(out, "running", BleHid.isRunning() ? 1 : 0);
  appendNumber(out, "connected", BleHid.isConnected() ? 1 : 0);
  appendNumber(out, "connecting", BleHid.isConnecting() ? 1 : 0);
  out += " name=";
  out += BleHid.connectedName();
  out += " addr=";
  out += BleHid.connectedAddr();
  uint16_t interval = 0, latency = 0, timeout = 0;
  if (bleSpikeConnParams(interval, latency, timeout)) {
    appendNumber(out, "itvl", interval);
    appendNumber(out, "latency", latency);
    appendNumber(out, "timeout", timeout);
  }
  appendNumber(out, "bonds", BleHid.pairedCount());
  for (uint8_t i = 0; i < BleHid.pairedCount(); ++i) {
    out += " [";
    out += BleHid.paired(i).addr;
    out += '|';
    out += BleHid.paired(i).name;
    out += ']';
  }
  appendInternal(out, "internal");
  appendMin(out);
  return out;
}

std::string forget(const std::string_view addr) {
  if (addr.size() != 17) return "BLE_FORGET: error=address";
  char address[18];
  memcpy(address, addr.data(), 17);
  address[17] = '\0';
  BleHid.forget(address);
  return link();
}

}  // namespace

void bleSpikeService() { serviceLink(); }

std::string memState(const char* head) {
  std::string out = head;
  appendNumber(out, "flash_ctrl", BLE_SPIKE_FLASH_CTRL);
  appendNumber(out, "released", btMemReleased(BT_MODE_BLE) ? 1 : 0);
  appendNumber(out, "keep_next_boot", bleInUse() ? 1 : 0);
  appendHeap(out, "now");
  appendMin(out);
  return out;
}

std::string setKeep(const bool keep) {
  nvs_handle_t handle;
  if (nvs_open("blespike", NVS_READWRITE, &handle) != ESP_OK) return "BLE_ERROR:nvs_open";
  const esp_err_t err = nvs_set_u8(handle, "keep", keep ? 1 : 0);
  const esp_err_t commit = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK || commit != ESP_OK) return "BLE_ERROR:nvs_write";
  return memState("BLE_KEEP:");
}

std::string bleSpikeRun(const std::string_view arg) {
  if (arg == "MEM") return memState("BLE_MEM:");
  if (arg == "KEEP=0") return setKeep(false);
  if (arg == "KEEP=1") return setKeep(true);
  if ((arg == "CTRL" || arg == "HOST") && btMemReleased(BT_MODE_BLE))
    return "BLE_ERROR:controller memory released at boot";
  if (arg == "CTRL") return controllerOn();
  if (arg == "CTRL_OFF") return controllerOff();
  if (arg == "HOST") return hostOn();
  if (arg == "HOST_OFF") return hostOff();
  if (btMemReleased(BT_MODE_BLE)) return "BLE_ERROR:controller memory released at boot";
  const size_t eq = arg.find('=');
  const std::string_view name = arg.substr(0, eq);
  const std::string_view value = eq == std::string_view::npos ? std::string_view() : arg.substr(eq + 1);
  if (name == "SCAN") return scan(value);
  if (name == "CONNECT") return connectTo(value);
  if (name == "RECONNECT") return reconnect(value);
  if (name == "KEYS") return keys(value);
  if (name == "LINK") return link();
  if (name == "FORGET") return forget(value);
  if (name == "PARAMS") {  // accept (1) or reject (0) the peripheral's connection-parameter requests
    bleSpikeAcceptConnParams(value == "1");
    return std::string("BLE_PARAMS: accept=") + (value == "1" ? "1" : "0");
  }
  if (name == "DISCONNECT") {
    BleHid.disconnect();
    return link();
  }
  return "BLE_ERROR:expected CTRL, CTRL_OFF, HOST, HOST_OFF, SCAN[=ms], CONNECT=addr, RECONNECT[=ms], KEYS[=ms], "
         "LINK, FORGET=addr or DISCONNECT";
}
