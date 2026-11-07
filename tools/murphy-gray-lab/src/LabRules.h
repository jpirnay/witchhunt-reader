#pragma once

// What the lab will and will not send to the panel. No Arduino dependency, so the host tests
// exercise exactly this code. There is one Murphy on the bench: anything outside these rules is
// refused rather than warned about.
//
// LUT layout, UC8253c datasheet rev 0.61 p.22: six groups of 7 bytes in KW mode.
//   byte 0     group repeat
//   bytes 1-4  level[7:6] | frames[5:0]
//   bytes 5-6  state 1 and state 2 repeat
// Levels: 00 0 V, 01 VSH (toward black), 10 VSL (toward white), 11 VDHR.

#include <stddef.h>
#include <stdint.h>

namespace lab {

#ifndef LAB_BOARD_X3
#define LAB_BOARD_X3 0
#endif

// The X3's UC8253 takes another layout (UC8179-style, as freeink-sdk lut/Uc8253X3Luts.h): seven groups
// of 6 bytes, a level byte for four sub-phases (phase 1 in bits 7:6), four frame counts, a repeat.
constexpr size_t kLutBytes = 42;
constexpr size_t kGroupBytes = LAB_BOARD_X3 ? 6 : 7;
constexpr size_t kGroups = kLutBytes / kGroupBytes;
constexpr uint8_t kLutSlots = 5;  // 0 = VCOM (0x20) .. 4 = KK (0x24)

// Longest waveform the lab sends. The supplied recipe's VCOM LUT is the longest known, at 96.
constexpr uint16_t kMaxFrames = 96;
// Largest one-directional drive in a single refresh. The recipe's LUT 22 sits exactly here.
constexpr uint16_t kMaxNetFrames = 32;

struct PanelConfig {
  uint8_t vsl;     // 0x01 byte 4
  uint8_t vdhr;    // 0x01 byte 5
  uint8_t vcom;    // 0x82
  uint8_t boost1;  // 0x06 byte 2
  uint8_t pll;     // 0x30
};

// Murphy production init (Uc8253MurphyDriver::initController).
constexpr PanelConfig kProductionConfig = {0x3B, 0x0D, 0x0F, 0xD7, 0x09};

struct LutDrive {
  uint16_t vsh = 0;
  uint16_t vsl = 0;
  uint16_t rest = 0;
  uint16_t vdhr = 0;

  uint16_t frames() const { return static_cast<uint16_t>(vsh + vsl + rest + vdhr); }
  // Positive is toward black. VDHR has the same polarity as VSH.
  int net() const { return static_cast<int>(vsh) + static_cast<int>(vdhr) - static_cast<int>(vsl); }
};

inline void addPhase(LutDrive& drive, uint8_t level, uint16_t frames) {
  switch (level) {
    case 0:
      drive.rest = static_cast<uint16_t>(drive.rest + frames);
      break;
    case 1:
      drive.vsh = static_cast<uint16_t>(drive.vsh + frames);
      break;
    case 2:
      drive.vsl = static_cast<uint16_t>(drive.vsl + frames);
      break;
    default:
      drive.vdhr = static_cast<uint16_t>(drive.vdhr + frames);
      break;
  }
}

inline LutDrive measureLut(const uint8_t* lut) {
  LutDrive drive;
  for (size_t group = 0; group < kGroups; group++) {
    const uint8_t* g = lut + group * kGroupBytes;
    for (size_t phase = 0; phase < 4; phase++) {
      if (LAB_BOARD_X3) {
        addPhase(drive, static_cast<uint8_t>(g[0] >> (6 - phase * 2) & 3), g[1 + phase]);
      } else {
        addPhase(drive, static_cast<uint8_t>(g[1 + phase] >> 6), g[1 + phase] & 0x3F);
      }
    }
  }
  return drive;
}

// nullptr when the LUT may be sent, otherwise why not.
inline const char* checkLut(uint8_t slot, const uint8_t* lut) {
  if (slot >= kLutSlots) return "bad slot";
  for (size_t group = 0; group < kGroups; group++) {
    const uint8_t* g = lut + group * kGroupBytes;
    // Repeat bytes multiply a waveform past what measureLut() sees, and 1 is what every known table
    // uses: at 1 the frame counts alone reproduce the measured FAST and HALF refresh times.
    if (LAB_BOARD_X3 ? g[5] > 1 : (g[0] > 1 || g[5] > 1 || g[6] > 1)) return "repeat byte above 1";
  }
  const LutDrive drive = measureLut(lut);
  if (LAB_BOARD_X3 && drive.vdhr) return "level 3 is not measured on the X3";
  if (drive.frames() > kMaxFrames) return "too many frames";
  if (slot == 0 && (drive.vsh || drive.vsl || drive.vdhr)) return "VCOM LUT must stay at VCOM_DC";
  if (drive.net() > static_cast<int>(kMaxNetFrames) || drive.net() < -static_cast<int>(kMaxNetFrames)) {
    return "net drive above limit";
  }
  return nullptr;
}

// Only settings already seen on this panel family: Murphy production, the OEM alternate init, the
// Good Display sample and the bench recipe. VSH is not settable: it stays at 15 V, the maximum.
inline const char* checkConfig(const PanelConfig& cfg) {
  if (LAB_BOARD_X3) return nullptr;  // the X3 runs its production init whatever CFG says
  if (cfg.vsl != 0x3B && cfg.vsl != 0x3F) return "VSL not allowed";
  if (cfg.vdhr != 0x0D && cfg.vdhr != 0x03) return "VDHR not allowed";
  if (cfg.vcom != 0x0F && cfg.vcom != 0x03 && cfg.vcom != 0x00) return "VCOM not allowed";
  if (cfg.boost1 != 0xD7 && cfg.boost1 != 0xDF) return "booster not allowed";
  if (cfg.pll != 0x09 && cfg.pll != 0x13 && cfg.pll != 0x1F) return "PLL not allowed";  // 50, 100, 200 Hz
  return nullptr;
}

}  // namespace lab
