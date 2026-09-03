#pragma once

// Releases the ESP32-S3 USB-OTG PHY and reconnects the hardware USB
// Serial/JTAG peripheral before a normal application restart.
//
// The S3 has ONE USB PHY behind the D+/D- pads, shared by two peripherals: the
// hardware USB Serial/JTAG block (the board's normal personality — how we log,
// monitor and flash) and USB-OTG (which TinyUSB drives for USB Drive). Starting
// MSC takes the PHY away from Serial/JTAG; a plain ESP.restart() would leave the
// host still enumerating the OTG device that no longer exists, so the port comes
// back dead until the cable is replugged. This walks the PHY back and forces a
// host-visible disconnect/reconnect so the reboot lands on a working console.
//
// No-op on targets that do not enable USB Drive (FREEINK_CAP_USB_MSC).
//
// Ported from crosspoint-reader PR #3203 (Julia, julia@uxj.io).
void handoffUsbOtgToSerialJtag();
