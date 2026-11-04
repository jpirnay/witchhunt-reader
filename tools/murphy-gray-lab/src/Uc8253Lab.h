#pragma once

// Direct UC8253 access for the Murphy M3 gray tests. Planes are controller-native: 240 wide,
// 416 tall, 30 bytes a row, MSB first. Rotation is the host's job, so what lab.py sends is what
// the controller receives.
//
// Bit-banged like the OEM firmware, because the read path has to turn SDA around. There is
// deliberately no way to send an arbitrary command: 0xA0, 0xA1 and 0xA3 program the OTP, once and
// for good.

#include <stddef.h>
#include <stdint.h>

#include "LabRules.h"

namespace lab {

constexpr uint16_t kCtrlWidth = 240;
constexpr uint16_t kCtrlHeight = 416;
constexpr size_t kPlaneBytes = static_cast<size_t>(kCtrlWidth) / 8 * kCtrlHeight;

// Murphy M3 display bus, BoardConfig MURPHY_M3. BUSY reads low while busy.
constexpr uint8_t kPinMosi = 3;
constexpr uint8_t kPinClock = 4;
constexpr uint8_t kPinCs = 5;
constexpr uint8_t kPinDc = 6;
constexpr uint8_t kPinReset = 7;
constexpr uint8_t kPinBusy = 8;
constexpr uint8_t kPinFrontlight = 48;

struct Bank {
  uint8_t lut[kLutSlots][kLutBytes];
};

enum class Clean : uint8_t { White, Black };

// How a refresh treats the controller around it.
enum class Handover : uint8_t {
  Cold,  // reset and full init first, power off after: what the Murphy driver does for a page
  Hold,  // the same, but stay powered for a Warm refresh to follow
  Warm   // no reset, no init: LUTs, planes and refresh on a controller left running by Hold
};

// Which read handshake the controller answers. The SDK has two, and the datasheet describes a
// third. REV has a known answer, so the lab tries them all rather than guessing.
enum class ReadMode : uint8_t {
  HeldSample,  // CS held across the command, sample while SCL is low (XteinkDetect epdCmdRead)
  HeldStock,   // CS held, sample after the rising edge, no pull-up (stock X3 and X4C firmware)
  Reselect,    // CS released after the command, data starts at the next CS fall (datasheet p.43)
  Count
};

class Panel {
 public:
  void begin();
  bool ready() const;
  const char* lastError() const { return error_; }

  // Read-only. These reset the controller, which moves no ink.
  void readRevision(ReadMode mode, uint8_t out[5]);
  uint8_t readStatus(ReadMode mode);
  bool beginOtpRead(ReadMode mode, bool powerOn);
  uint8_t readOtpByte();
  void endOtpRead();

  // Drive the panel. `waveformMs` is how long BUSY held for the refresh itself.
  bool clean(Clean target, uint32_t* waveformMs);
  bool refresh(const PanelConfig& cfg, const Bank& bank, const uint8_t* dtm1, const uint8_t* dtm2, uint32_t* waveformMs,
               Handover handover = Handover::Cold);

 private:
  void reset();
  bool initController(const PanelConfig& cfg);
  void loadBank(const Bank& bank);
  bool finishRefresh(uint32_t* waveformMs, bool powerOff = true);
  void command(uint8_t cmd);
  void data(uint8_t value);
  void dataBytes(const uint8_t* bytes, size_t length);
  void fillPlane(uint8_t cmd, uint8_t value);
  void writeBits(uint8_t value);
  void startRead(ReadMode mode, uint8_t cmd);
  uint8_t readBits();
  void endRead();
  bool waitReady(const char* stage, uint32_t timeoutMs);

  ReadMode readMode_ = ReadMode::HeldSample;
  bool held_ = false;  // a Hold refresh left the controller running
  const char* error_ = nullptr;
};

}  // namespace lab
