#include "SerialBuffers.h"

#include <Logging.h>
#include <hal/usb_serial_jtag_ll.h>

#include <cstddef>
#include <cstdint>

#include "util/SerialLineBuffer.h"

namespace {

// One whole line, however fast it arrives. Short of that a burst loses its tail, the line end with it,
// and the broken line then swallows the next command too (a 400-byte path did, on the X3).
constexpr size_t kCommandRxBytes = SerialLineBuffer::MAX_LINE_BYTES + 64;
constexpr size_t kBulkRxBytes = 4096;  // MicroReader's usb_serial_jtag rx_buffer_size

// HWCDC's setter replaces the queue without masking the USB Serial/JTAG interrupt, whose handler sends
// to it: with a host attached the handler is live, and a resize can race it into freed memory (X3,
// 2026-10-18: after CMD:REBOOT the device answered nothing until a USB reset). Bytes the interrupt has
// not taken wait in the hardware FIFO meanwhile.
bool resizeReceiveQueue(const size_t bytes) {
  const uint32_t enabled = usb_serial_jtag_ll_get_intr_ena_status();
  usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
  // The setter deletes the old queue first, so a failed allocation would leave the wire deaf.
  const bool resized = logSerial.setRxBufferSize(bytes) != 0;
  if (!resized) logSerial.setRxBufferSize(256);
  usb_serial_jtag_ll_ena_intr_mask(enabled);
  return resized;
}

}  // namespace

void SerialBuffers::sizeForCommands() { resizeReceiveQueue(kCommandRxBytes); }

void SerialBuffers::arm() {
  static bool armed = false;
  if (armed) return;
  armed = true;
  if (resizeReceiveQueue(kBulkRxBytes)) {
    LOG_INF("SER", "receive buffer armed: %u bytes", static_cast<unsigned>(kBulkRxBytes));
  } else {
    LOG_ERR("SER", "No %u bytes for the receive buffer: uploads may lose data", static_cast<unsigned>(kBulkRxBytes));
  }
}
