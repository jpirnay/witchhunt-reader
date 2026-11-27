// SPIKE ONLY (branch chore/ble-c3-spike, never for merge): what Bluetooth LE costs on the C3.
//
// CMD:BLE:CTRL      Espressif's controller alone, driven over VHCI with raw HCI (reset, event mask, a 3 s passive
//                   scan): the floor that any host sits on, NimBLE or a hand-written keypress-only one.
// CMD:BLE:CTRL_OFF  controller disable + deinit.
// CMD:BLE:HOST      the SDK's BleKeyboardHost (NimBLE-Arduino's host on the same controller): begin, a 3 s scan.
// CMD:BLE:HOST_OFF  BleHid.end().
// CMD:BLE:KEEP=0|1  whether the next boot keeps the controller's memory (initArduino() releases it when bleInUse()
//                   says no); takes effect after CMD:REBOOT. CMD:BLE:MEM reports the current state.
//
// Each reply lists free/largest-free-block after every step, and the since-boot minimum. REBOOT between runs.
#include <Arduino.h>
#include <BleKeyboardHost.h>
#include <esp32-hal-bt.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <nvs.h>

#include <atomic>
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
  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  const unsigned long t0 = millis();
  esp_err_t err = esp_bt_controller_init(&cfg);
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
  if (!BleHid.begin("WitchSpike")) {
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
  appendHeap(out, "off");
  appendMin(out);
  return out;
}

}  // namespace

std::string memState(const char* head) {
  std::string out = head;
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
  return "BLE_ERROR:expected CTRL, CTRL_OFF, HOST or HOST_OFF";
}
