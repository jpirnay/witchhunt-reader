// Murphy M3 gray lab: a serial command loop around Uc8253Lab. tools/murphy-gray-lab/lab.py drives
// it; see the README. One text line in, one OK or ERR line out. State is kept per boot, so a host
// sends its whole setup with each step and a reset costs nothing but ARM.

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "LabRules.h"
#include "Uc8253Lab.h"

using namespace lab;

namespace {

constexpr size_t kLineBytes = 160;
constexpr uint32_t kFrameTimeoutMs = 5000;
constexpr uint16_t kOtpBytes = 8192;
constexpr uint8_t kOtpLineBytes = 32;

Panel panel;
Bank bank;
PanelConfig config = kProductionConfig;
uint8_t dtm1[kPlaneBytes];
uint8_t dtm2[kPlaneBytes];
bool armed = false;
uint8_t lutsLoaded = 0;  // bit per slot
bool frameLoaded = false;

void fail(const char* why) {
  Serial.print("ERR ");
  Serial.println(why);
}

void ok(const char* what) {
  Serial.print("OK ");
  Serial.println(what);
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseHex(const char* text, uint8_t* out, size_t length) {
  if (!text || strlen(text) != length * 2) return false;
  for (size_t i = 0; i < length; i++) {
    const int high = hexValue(text[i * 2]);
    const int low = hexValue(text[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    out[i] = static_cast<uint8_t>(high << 4 | low);
  }
  return true;
}

void printHexByte(uint8_t value) {
  static const char digits[] = "0123456789abcdef";
  Serial.print(digits[value >> 4]);
  Serial.print(digits[value & 0x0F]);
}

void reportRefresh(bool done, uint32_t waveformMs, uint32_t startMs) {
  if (!done) {
    fail(panel.lastError() ? panel.lastError() : "refresh failed");
    return;
  }
  Serial.print("OK waveform_ms=");
  Serial.print(waveformMs);
  Serial.print(" total_ms=");
  Serial.println(millis() - startMs);
}

void commandRev() {
  for (uint8_t mode = 0; mode < static_cast<uint8_t>(ReadMode::Count); mode++) {
    uint8_t rev[5];
    panel.readRevision(static_cast<ReadMode>(mode), rev);
    const uint8_t status = panel.readStatus(static_cast<ReadMode>(mode));
    Serial.print("REV ");
    Serial.print(mode);
    Serial.print(' ');
    for (uint8_t byte : rev) printHexByte(byte);
    Serial.print(" FLG ");
    printHexByte(status);
    Serial.println();
  }
  ok("rev");
}

void commandOtp(char* args) {
  const char* modeText = strtok(args, " ");
  const char* powerText = strtok(nullptr, " ");
  if (!modeText || !powerText) return fail("usage: OTP <mode> <poweron 0|1>");
  const int mode = atoi(modeText);
  if (mode < 0 || mode >= static_cast<int>(ReadMode::Count)) return fail("bad mode");
  if (!panel.beginOtpRead(static_cast<ReadMode>(mode), atoi(powerText) != 0)) {
    return fail(panel.lastError() ? panel.lastError() : "otp read failed");
  }
  for (uint16_t address = 0; address < kOtpBytes; address = static_cast<uint16_t>(address + kOtpLineBytes)) {
    Serial.print("OTP ");
    printHexByte(static_cast<uint8_t>(address >> 8));
    printHexByte(static_cast<uint8_t>(address & 0xFF));
    Serial.print(' ');
    for (uint8_t i = 0; i < kOtpLineBytes; i++) printHexByte(panel.readOtpByte());
    Serial.println();
  }
  panel.endOtpRead();
  ok("otp");
}

void commandCfg(char* args) {
  uint8_t bytes[5];
  if (!parseHex(strtok(args, " "), bytes, sizeof(bytes))) return fail("usage: CFG <vsl vdhr vcom boost1 pll as hex>");
  const PanelConfig next = {bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]};
  const char* rejected = checkConfig(next);
  if (rejected) return fail(rejected);
  config = next;
  ok("cfg");
}

void commandLut(char* args) {
  const char* slotText = strtok(args, " ");
  const char* hexText = strtok(nullptr, " ");
  uint8_t lut[kLutBytes];
  if (!slotText || !parseHex(hexText, lut, sizeof(lut))) return fail("usage: LUT <slot> <84 hex digits>");
  const uint8_t slot = static_cast<uint8_t>(atoi(slotText));
  const char* rejected = checkLut(slot, lut);
  if (rejected) return fail(rejected);
  memcpy(bank.lut[slot], lut, sizeof(lut));
  lutsLoaded = static_cast<uint8_t>(lutsLoaded | 1u << slot);
  ok("lut");
}

// FRAME, then both planes as raw bytes. The reply carries a sum the host checks before it sends GO.
void commandFrame() {
  frameLoaded = false;
  Serial.println("READY");
  Serial.setTimeout(kFrameTimeoutMs);
  if (Serial.readBytes(reinterpret_cast<char*>(dtm1), kPlaneBytes) != kPlaneBytes ||
      Serial.readBytes(reinterpret_cast<char*>(dtm2), kPlaneBytes) != kPlaneBytes) {
    return fail("short frame");
  }
  uint32_t sum = 0;
  for (size_t i = 0; i < kPlaneBytes; i++) sum += static_cast<uint32_t>(dtm1[i]) + dtm2[i];
  frameLoaded = true;
  Serial.print("OK sum=");
  Serial.println(sum);
}

void commandGo() {
  if (lutsLoaded != (1u << kLutSlots) - 1) return fail("LUTs not loaded");
  if (!frameLoaded) return fail("frame not loaded");
  const uint32_t start = millis();
  uint32_t waveformMs = 0;
  const bool done = panel.refresh(config, bank, dtm1, dtm2, &waveformMs);
  reportRefresh(done, waveformMs, start);
}

void commandClean(Clean target) {
  const uint32_t start = millis();
  uint32_t waveformMs = 0;
  const bool done = panel.clean(target, &waveformMs);
  reportRefresh(done, waveformMs, start);
}

void handle(char* line) {
  // A command sent without arguments gets its usage reply, so strtok always sees a real string.
  static char noArgs[] = "";
  char* args = strchr(line, ' ');
  if (args) {
    *args++ = '\0';
  } else {
    args = noArgs;
  }

  if (!strcmp(line, "HELLO")) {
    Serial.print("OK murphy-gray-lab 1 armed=");
    Serial.println(armed ? 1 : 0);
  } else if (!strcmp(line, "ARM")) {
    armed = true;
    ok("armed until reset");
  } else if (!strcmp(line, "BUSY")) {
    ok(panel.ready() ? "ready" : "busy");
  } else if (!strcmp(line, "REV")) {
    commandRev();
  } else if (!strcmp(line, "OTP")) {
    commandOtp(args);
  } else if (!strcmp(line, "CFG")) {
    commandCfg(args);
  } else if (!strcmp(line, "LUT")) {
    commandLut(args);
  } else if (!strcmp(line, "FRAME")) {
    commandFrame();
  } else if (!strcmp(line, "GO") || !strcmp(line, "W") || !strcmp(line, "K")) {
    // The three commands that move ink.
    if (!armed) return fail("not armed");
    if (line[0] == 'G') {
      commandGo();
    } else {
      commandClean(line[0] == 'W' ? Clean::White : Clean::Black);
    }
  } else {
    fail("unknown command");
  }
}

}  // namespace

void setup() {
  // Off for photographs, and a constant if it were left floating.
  pinMode(kPinFrontlight, OUTPUT);
  digitalWrite(kPinFrontlight, LOW);
  Serial.begin(115200);
  panel.begin();
}

void loop() {
  static char line[kLineBytes];
  static size_t length = 0;
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      line[length] = '\0';
      length = 0;
      if (line[0]) handle(line);
      return;
    }
    if (length + 1 < sizeof(line)) line[length++] = static_cast<char>(c);
  }
  delay(1);
}
