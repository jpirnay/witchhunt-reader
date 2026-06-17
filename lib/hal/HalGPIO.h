#pragma once

#include <Arduino.h>
#include <InputManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8   // SPI Clock
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#define EPD_CS 21    // Chip Select
#define EPD_DC 4     // Data/Command
#define EPD_RST 5    // Reset
#define EPD_BUSY 6   // Busy

#define SPI_MISO 7  // SPI MISO, shared between SD card and display (Master In Slave Out)

#define BAT_GPIO0 0  // Battery voltage

#define UART0_RXD 20  // Used for USB connection detection

// Xteink X3 Hardware
#define X3_I2C_SDA 20
#define X3_I2C_SCL 0
#define X3_I2C_FREQ 400000

// TI BQ27220 Fuel gauge I2C
#define I2C_ADDR_BQ27220 0x55    // Fuel gauge I2C address
#define BQ27220_SOC_REG 0x2C     // StateOfCharge() command code (%)
#define BQ27220_CUR_REG 0x0C     // Current() command code (signed mA)
#define BQ27220_VOLT_REG 0x08    // Voltage() command code (mV)
#define BQ27220_FLAGS_REG 0x0A   // BatteryStatus() / Flags() command code (bit0=DSG)
#define BQ27220_FLAG_DSG 0x0001  // DSG bit: 1 = discharging, 0 = charging or at rest

// Analog DS3231 RTC I2C
#define I2C_ADDR_DS3231 0x68  // RTC I2C address
#define DS3231_SEC_REG 0x00   // Seconds command code (BCD)

// QST QMI8658 IMU I2C
#define I2C_ADDR_QMI8658 0x6B        // IMU I2C address
#define I2C_ADDR_QMI8658_ALT 0x6A    // IMU I2C fallback address
#define QMI8658_WHO_AM_I_REG 0x00    // WHO_AM_I command code
#define QMI8658_WHO_AM_I_VALUE 0x05  // WHO_AM_I expected value

namespace X3GPIO {
// Read a 16-bit little-endian I2C register. Returns false on bus error.
bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue);
}  // namespace X3GPIO

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

  bool lastUsbConnected = false;
  bool usbStateChanged = false;

 public:
  enum class DeviceType : uint8_t { X4, X3 };

  // A single debounced button transition captured by the background sampler.
  // `button` is a raw BTN_* index; `pressed` is true for a press edge, false for
  // a release edge; `timeMs` is the millis() value at the time the edge was
  // detected, so consumers can classify Short/Long/Double independent of how
  // often the loop task gets around to draining the queue.
  struct ButtonEdge {
    uint8_t button = 0;
    bool pressed = false;
    uint32_t timeMs = 0;
  };

 private:
  DeviceType _deviceType = DeviceType::X4;

  // ---- Background button sampler (see HalGPIO.cpp) ----------------------------
  // The buttons are read by polling the ADC; the loop task can be blocked for
  // hundreds of ms at a time (e.g. a sliced background section build whose slice
  // overshoots its budget on a heavy page), so polling once per loop iteration
  // drops presses that begin and end inside one slow iteration. A dedicated task
  // samples + debounces on a fixed ~10ms cadence regardless of loop progress and
  // latches every edge for the loop task to drain.
  TaskHandle_t samplerTaskHandle_ = nullptr;
  volatile bool samplerRunning_ = false;
  portMUX_TYPE inputMux_ = portMUX_INITIALIZER_UNLOCKED;

  // Shared sampler→loop state, guarded by inputMux_.
  uint8_t accumPressed_ = 0;   // press edges seen since the last update() drain
  uint8_t accumReleased_ = 0;  // release edges seen since the last update() drain
  uint8_t liveState_ = 0;      // latest debounced button state
  unsigned long heldTimeSnapshot_ = 0;
  static constexpr int EDGE_BUF = 32;
  ButtonEdge edgeBuf_[EDGE_BUF] = {};
  int edgeHead_ = 0;
  int edgeTail_ = 0;

  // Loop-side snapshot refreshed by update(); only the loop task reads/writes these.
  uint8_t snapState_ = 0;
  uint8_t snapPressed_ = 0;
  uint8_t snapReleased_ = 0;

  void sampleOnce();
  void pushEdgeLocked(uint8_t button, bool pressed, uint32_t timeMs);
  static void samplerTask(void* arg);

 public:
  HalGPIO() = default;

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }

  // Start button GPIO and setup SPI for screen and SD card
  void begin();

  // Button input methods
  void update();
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;

  // Start/stop the background sampler. startInputSampler() must be called once
  // input handling is wanted (end of setup, after the boot-time power-button
  // handling that drives inputMgr.update() directly). Until then update() falls
  // back to sampling synchronously. stopInputSampler() is called before deep
  // sleep so no ADC read races with the display/power-rail teardown.
  void startInputSampler();
  void stopInputSampler();

  // True if any button press has been sampled since the last update() drain.
  // Safe to call from any context (e.g. mid-stall inside loop()); does not
  // consume the edge — update() will still see it on the next main-loop tick.
  bool hasPendingInput() const;

  // Pop the oldest queued button edge (FIFO). Returns false when the queue is
  // empty. Drained by ButtonEventManager to drive its press-type FSM.
  bool popButtonEdge(ButtonEdge& out);
  // Drop all queued edges and pending accumulated press/release bits. Called on
  // activity transitions so stale input does not bleed across screens.
  void flushButtonEdges();

  // Minimum free stack (bytes) the sampler task has ever had, for right-sizing its
  // stack allocation. 0 when the sampler is not running.
  UBaseType_t samplerStackHighWater() const {
    return samplerTaskHandle_ ? uxTaskGetStackHighWaterMark(samplerTaskHandle_) : 0;
  }

  // Wait until the raw power-button GPIO reads HIGH (released) for a sustained period.
  // Uses the raw pin directly instead of the InputManager debounced state to avoid
  // the 5 ms debounce being fooled by mechanical switch bounce during release.
  void waitForStablePowerRelease();

  // Setup wake up GPIO and enter deep sleep
  void startDeepSleep();

  // Verify power button was held long enough after wakeup.
  // If verification fails, enters deep sleep and does not return.
  // Should only be called when wakeup reason is PowerButton.
  void verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed);

  // Check if USB is connected
  bool isUsbConnected() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

extern HalGPIO gpio;
