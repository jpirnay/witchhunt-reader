#include "Uc8253Lab.h"

#include <Arduino.h>
#include <string.h>

// The refresh sequence follows freeink-sdk Uc8253MurphyDriver: reset and full init before every
// refresh, then planes, then 0x12. The read paths follow freeink-sdk XteinkDetect.cpp. Written from
// the bench harness in Murphy_M3_Grayscale_Implementation_Scope (murphy-m3-grayscale-tests.zip),
// whose `W` command sent 0x00 and so painted the panel black.

namespace lab {
namespace {

constexpr uint8_t kCmdPanelSetting = 0x00;
constexpr uint8_t kCmdPowerSetting = 0x01;
constexpr uint8_t kCmdPowerOff = 0x02;
constexpr uint8_t kCmdPowerOn = 0x04;
constexpr uint8_t kCmdBooster = 0x06;
constexpr uint8_t kCmdDtm1 = 0x10;
constexpr uint8_t kCmdDataStop = 0x11;
constexpr uint8_t kCmdRefresh = 0x12;
constexpr uint8_t kCmdDtm2 = 0x13;
constexpr uint8_t kCmdLutFirst = 0x20;
constexpr uint8_t kCmdPll = 0x30;
constexpr uint8_t kCmdCdi = 0x50;
constexpr uint8_t kCmdResolution = 0x61;
constexpr uint8_t kCmdRevision = 0x70;
constexpr uint8_t kCmdStatus = 0x71;
constexpr uint8_t kCmdVcom = 0x82;
constexpr uint8_t kCmdReadOtp = 0xA2;

constexpr uint32_t kBusyTimeoutMs = 15000;
constexpr uint32_t kReadBusyTimeoutMs = 300;

#if LAB_BOARD_X3
constexpr uint8_t kCmdPowerOffSequence = 0x03;
constexpr uint8_t kCmdGateSourceStart = 0x65;
constexpr uint8_t kCmdLvSelection = 0xE1;
// Uc8253X3Driver writes every LUT at 42 bytes.
constexpr uint8_t kLutWireBytes[kLutSlots] = {42, 42, 42, 42, 42};
// The X3's full refresh bank (freeink-sdk lut/Uc8253X3Luts.h, _full, CDI 0x29 0x07). With the same data in
// both planes only WW (VSH 24, VSL 24) and BB (VSL 24, VSH 24) fire, and both net to zero.
constexpr Bank kX3FullBank = {{
    {0x00, 0x18, 0x04, 0x0E, 0x0A, 0x01, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x01},
    {0x4A, 0x18, 0x04, 0x0E, 0x0A, 0x01, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x01},
    {0x0A, 0x18, 0x04, 0x0E, 0x0A, 0x01, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x01},
    {0x04, 0x18, 0x04, 0x0E, 0x0A, 0x01, 0x40, 0x0A, 0x00, 0x00, 0x00, 0x01},
    {0x84, 0x18, 0x04, 0x0E, 0x0A, 0x01, 0x40, 0x0A, 0x00, 0x00, 0x00, 0x01},
}};
#else
// Production writes VCOM (0x20) and KW (0x22) at 56 bytes. KW mode uses only six groups, so the
// tail is zero padding, but this keeps the bus traffic identical to the path proven on this unit.
constexpr uint8_t kLutWireBytes[kLutSlots] = {56, 42, 56, 42, 42};
#endif

// Murphy production DEFAULT bank (freeink-sdk lut/Uc8253MurphyLuts.h). It is the only bank with
// months of use on this panel, so clean() uses it. With the same data in both planes only WW and
// KK fire, and both net to zero.
constexpr Bank kProductionBank = {{
    {0x01, 0x0F, 0x0F, 0x0F, 0x01, 0x01, 0x01},
    {0x01, 0x4F, 0x8F, 0x0F, 0x01, 0x01, 0x01},
    {0x01, 0x0F, 0x8F, 0x0F, 0x01, 0x01, 0x01},
    {0x01, 0x4F, 0x8F, 0x4F, 0x01, 0x01, 0x01},
    {0x01, 0x0F, 0x8F, 0x4F, 0x01, 0x01, 0x01},
}};

inline void clockDelay() { delayMicroseconds(1); }

}  // namespace

void Panel::begin() {
  pinMode(kPinMosi, OUTPUT);
  pinMode(kPinClock, OUTPUT);
  pinMode(kPinCs, OUTPUT);
  pinMode(kPinDc, OUTPUT);
  pinMode(kPinReset, OUTPUT);
  pinMode(kPinBusy, INPUT);
  digitalWrite(kPinClock, LOW);
  digitalWrite(kPinCs, HIGH);
  digitalWrite(kPinDc, HIGH);
  digitalWrite(kPinReset, HIGH);
}

bool Panel::ready() const { return digitalRead(kPinBusy) == HIGH; }

// EpdBus::reset(200): the Murphy panel wants the long settle. The X3 driver uses reset(50).
void Panel::reset() {
  digitalWrite(kPinReset, HIGH);
  delay(10);
  digitalWrite(kPinReset, LOW);
  delay(10);
  digitalWrite(kPinReset, HIGH);
  delay(LAB_BOARD_X3 ? 60 : 210);
}

bool Panel::waitReady(const char* stage, uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (!ready()) {
    if (millis() - start >= timeoutMs) {
      error_ = stage;
      return false;
    }
    delay(1);
  }
  return true;
}

void Panel::writeBits(uint8_t value) {
  for (uint8_t bit = 0; bit < 8; bit++) {
    digitalWrite(kPinMosi, (value & 0x80) ? HIGH : LOW);
    digitalWrite(kPinClock, HIGH);
    digitalWrite(kPinClock, LOW);
    value = static_cast<uint8_t>(value << 1);
  }
}

void Panel::command(uint8_t cmd) {
  digitalWrite(kPinDc, LOW);
  digitalWrite(kPinCs, LOW);
  writeBits(cmd);
  digitalWrite(kPinCs, HIGH);
  digitalWrite(kPinDc, HIGH);
}

void Panel::data(uint8_t value) { dataBytes(&value, 1); }

void Panel::dataBytes(const uint8_t* bytes, size_t length) {
  digitalWrite(kPinDc, HIGH);
  digitalWrite(kPinCs, LOW);
  for (size_t i = 0; i < length; i++) writeBits(bytes[i]);
  digitalWrite(kPinCs, HIGH);
}

void Panel::fillPlane(uint8_t cmd, uint8_t value) {
  command(cmd);
  digitalWrite(kPinDc, HIGH);
  digitalWrite(kPinCs, LOW);
  for (size_t i = 0; i < kPlaneBytes; i++) writeBits(value);
  digitalWrite(kPinCs, HIGH);
  if (LAB_BOARD_X3) command(kCmdDataStop);
}

void Panel::writePlane(uint8_t cmd, const uint8_t* plane) {
  command(cmd);
  dataBytes(plane, kPlaneBytes);
  if (LAB_BOARD_X3) command(kCmdDataStop);
}

// Uc8253MurphyDriver::initController, with the settings the lab varies taken from `cfg`. On the X3,
// Uc8253X3Driver::initController as it is: the lab does not vary that panel's voltages.
bool Panel::initController(const PanelConfig& cfg) {
  error_ = nullptr;
  reset();
  if (!waitReady("reset", kBusyTimeoutMs)) return false;
#if LAB_BOARD_X3
  (void)cfg;
  const uint8_t panelSetting[] = {0x3F, 0x0A};
  command(kCmdPanelSetting);
  dataBytes(panelSetting, sizeof(panelSetting));
  const uint8_t resolution[] = {0x03, 0x18, 0x02, 0x58};
  command(kCmdResolution);
  dataBytes(resolution, sizeof(resolution));
  const uint8_t gateSource[] = {0x00, 0x00, 0x00, 0x00};
  command(kCmdGateSourceStart);
  dataBytes(gateSource, sizeof(gateSource));
  command(kCmdPowerOffSequence);
  data(0x20);
  const uint8_t power[] = {0x07, 0x17, 0x3F, 0x3F, 0x17};
  command(kCmdPowerSetting);
  dataBytes(power, sizeof(power));
  command(kCmdVcom);
  data(0x24);
  const uint8_t booster[] = {0x25, 0x25, 0x3C, 0x37};
  command(kCmdBooster);
  dataBytes(booster, sizeof(booster));
  command(kCmdPll);
  data(0x09);
  command(kCmdLvSelection);
  data(0x02);
  const uint8_t cdi[] = {0x29, 0x07};  // the CDI the X3 driver loads its banks with
  command(kCmdCdi);
  dataBytes(cdi, sizeof(cdi));
  return true;
#else
  const uint8_t power[] = {0x03, 0x10, 0x3F, cfg.vsl, cfg.vdhr};
  command(kCmdPowerSetting);
  dataBytes(power, sizeof(power));
  const uint8_t booster[] = {0xD7, cfg.boost1, 0x1F};
  command(kCmdBooster);
  dataBytes(booster, sizeof(booster));
  command(kCmdPowerOn);
  if (!waitReady("power on", kBusyTimeoutMs)) return false;
  command(kCmdPanelSetting);
  data(0xFF);
  command(kCmdPll);
  data(cfg.pll);
  const uint8_t resolution[] = {0xF0, 0x01, 0xA0};
  command(kCmdResolution);
  dataBytes(resolution, sizeof(resolution));
  command(kCmdVcom);
  data(cfg.vcom);
  command(kCmdCdi);
  data(0x97);
  return true;
#endif
}

void Panel::loadBank(const Bank& bank) {
  for (uint8_t slot = 0; slot < kLutSlots; slot++) {
    command(static_cast<uint8_t>(kCmdLutFirst + slot));
    dataBytes(bank.lut[slot], kLutBytes);
    // Continue the same register write with the zero tail.
    digitalWrite(kPinDc, HIGH);
    digitalWrite(kPinCs, LOW);
    for (uint8_t i = kLutBytes; i < kLutWireBytes[slot]; i++) writeBits(0x00);
    digitalWrite(kPinCs, HIGH);
  }
}

bool Panel::finishRefresh(uint32_t* waveformMs, bool powerOff) {
#if LAB_BOARD_X3
  // The X3 init leaves the rails off; its driver powers on before every refresh it starts cold.
  command(kCmdPowerOn);
  if (!waitReady("power on", kBusyTimeoutMs)) return false;
#endif
  const uint32_t start = millis();
  command(kCmdRefresh);
  delay(1);  // Good Display sample: BUSY needs at least 200 us to assert
  // The X3's BUSY can stay high a little after 0x12 (Uc8253X3Driver::displayStart waits up to 50 ms).
  for (uint32_t t = millis(); LAB_BOARD_X3 && ready() && millis() - t < 50;) delay(1);
  const bool ok = waitReady("refresh", kBusyTimeoutMs);
  if (waveformMs) *waveformMs = millis() - start;
  if (!powerOff) return ok;
  command(kCmdPowerOff);
  // Keep the first failure: a refresh timeout must not be overwritten by a clean power off.
  const char* refreshError = error_;
  const bool off = waitReady("power off", kBusyTimeoutMs);
  if (refreshError) error_ = refreshError;
  return ok && off;
}

// DDX=01: {NEW,OLD} 11 selects WW, so 0xFF is white. Good Display's clear screen writes 0xFF too.
bool Panel::clean(Clean target, uint32_t* waveformMs) {
  held_ = false;
  if (!initController(kProductionConfig)) return false;
#if LAB_BOARD_X3
  loadBank(kX3FullBank);
#else
  loadBank(kProductionBank);
#endif
  const uint8_t fill = target == Clean::White ? 0xFF : 0x00;
  fillPlane(kCmdDtm1, fill);
  fillPlane(kCmdDtm2, fill);
  return finishRefresh(waveformMs);
}

bool Panel::refresh(const PanelConfig& cfg, const Bank& bank, const uint8_t* dtm1, const uint8_t* dtm2,
                    uint32_t* waveformMs, Handover handover) {
  const char* rejected = checkConfig(cfg);
  for (uint8_t slot = 0; slot < kLutSlots && !rejected; slot++) rejected = checkLut(slot, bank.lut[slot]);
  if (!rejected && handover == Handover::Warm && !held_) rejected = "warm refresh needs a Hold first";
  if (rejected) {
    error_ = rejected;
    return false;
  }
  if (handover == Handover::Warm) {
    error_ = nullptr;
  } else if (!initController(cfg)) {
    return false;
  }
  held_ = false;
  loadBank(bank);
  writePlane(kCmdDtm1, dtm1);
  writePlane(kCmdDtm2, dtm2);
  const bool hold = handover == Handover::Hold;
  const bool done = finishRefresh(waveformMs, !hold);
  held_ = hold && done;
  return done;
}

void Panel::startRead(ReadMode mode, uint8_t cmd) {
  readMode_ = mode;
  digitalWrite(kPinDc, LOW);
  digitalWrite(kPinCs, LOW);
  clockDelay();
  writeBits(cmd);
  digitalWrite(kPinDc, HIGH);
  clockDelay();
  if (mode == ReadMode::Reselect) {
    digitalWrite(kPinCs, HIGH);
    clockDelay();
  }
  pinMode(kPinMosi, mode == ReadMode::HeldStock ? INPUT : INPUT_PULLUP);
  delayMicroseconds(2);
  if (mode == ReadMode::Reselect) {
    digitalWrite(kPinCs, LOW);
    clockDelay();
  }
}

uint8_t Panel::readBits() {
  uint8_t value = 0;
  for (uint8_t bit = 0; bit < 8; bit++) {
    if (readMode_ == ReadMode::HeldStock) {
      digitalWrite(kPinClock, LOW);
      clockDelay();
      digitalWrite(kPinClock, HIGH);
      clockDelay();
      value = static_cast<uint8_t>((value << 1) | (digitalRead(kPinMosi) == HIGH ? 1 : 0));
    } else {
      clockDelay();
      value = static_cast<uint8_t>((value << 1) | (digitalRead(kPinMosi) == HIGH ? 1 : 0));
      digitalWrite(kPinClock, HIGH);
      clockDelay();
      digitalWrite(kPinClock, LOW);
    }
  }
  digitalWrite(kPinClock, LOW);
  return value;
}

void Panel::endRead() {
  digitalWrite(kPinCs, HIGH);
  pinMode(kPinMosi, OUTPUT);
}

void Panel::readRevision(ReadMode mode, uint8_t out[5]) {
  reset();
  waitReady("reset", kReadBusyTimeoutMs);
  startRead(mode, kCmdRevision);
  for (uint8_t i = 0; i < 5; i++) out[i] = readBits();
  endRead();
}

uint8_t Panel::readStatus(ReadMode mode) {
  reset();
  waitReady("reset", kReadBusyTimeoutMs);
  startRead(mode, kCmdStatus);
  const uint8_t status = readBits();
  endRead();
  return status;
}

bool Panel::beginOtpRead(ReadMode mode, bool powerOn) {
  error_ = nullptr;
  reset();
  if (!waitReady("reset", kReadBusyTimeoutMs)) return false;
  if (powerOn) {
    // Rails only. Nothing reaches the glass without a refresh command.
    command(kCmdPowerOn);
    if (!waitReady("power on", kBusyTimeoutMs)) return false;
  }
  startRead(mode, kCmdReadOtp);
  readBits();  // the datasheet's dummy byte
  return true;
}

uint8_t Panel::readOtpByte() { return readBits(); }

void Panel::endOtpRead() {
  endRead();
  command(kCmdPowerOff);
  waitReady("power off", kBusyTimeoutMs);
}

}  // namespace lab
