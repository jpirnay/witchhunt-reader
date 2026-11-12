#include "SerialBuffers.h"

#include <Logging.h>
#include <hal/usb_serial_jtag_ll.h>

#include <cstddef>
#include <cstdint>

#include "util/ReceiveQueueGrowth.h"
#include "util/SerialLineBuffer.h"

namespace {

// One whole line, however fast it arrives. Short of that a burst loses its tail, the line end with it,
// and the broken line then swallows the next command too (a 400-byte path did, on the X3).
constexpr size_t kCommandRxBytes = SerialLineBuffer::MAX_LINE_BYTES + 64;
constexpr size_t kBulkRxBytes = 4096;  // MicroReader's usb_serial_jtag rx_buffer_size

// HWCDC's setter replaces the queue without masking the USB Serial/JTAG interrupt, whose handler sends
// to it: with a host attached the handler is live, and a resize can race it into freed memory (X3,
// 2026-10-18: after CMD:REBOOT the device answered nothing until a USB reset). Bytes the interrupt has
// not taken wait in the hardware FIFO while it is masked.
class MaskedReceiveInterrupt {
 public:
  MaskedReceiveInterrupt() : enabled_(usb_serial_jtag_ll_get_intr_ena_status()) {
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
  }
  ~MaskedReceiveInterrupt() { usb_serial_jtag_ll_ena_intr_mask(enabled_); }
  MaskedReceiveInterrupt(const MaskedReceiveInterrupt&) = delete;
  MaskedReceiveInterrupt& operator=(const MaskedReceiveInterrupt&) = delete;

 private:
  uint32_t enabled_;
};

// Call with the interrupt masked. The setter deletes the old queue first, so a failed allocation would leave the
// wire deaf: then the command size, and 256 bytes as the last resort.
bool replaceReceiveQueue(const size_t bytes) {
  if (logSerial.setRxBufferSize(bytes) != 0) return true;
  if (logSerial.setRxBufferSize(kCommandRxBytes) == 0) logSerial.setRxBufferSize(256);
  return false;
}

ReceiveQueueGrowth growth;

}  // namespace

void SerialBuffers::sizeForCommands() {
  const MaskedReceiveInterrupt masked;
  replaceReceiveQueue(kCommandRxBytes);
}

void SerialBuffers::arm() {
  if (growth.grown()) return;
  bool grew = false;
  bool resized = false;
  {
    // The queue is looked at with the interrupt masked, so no byte can come into it between the look and the swap.
    const MaskedReceiveInterrupt masked;
    const int queued = logSerial.available();
    grew = growth.growNow(queued > 0 ? static_cast<size_t>(queued) : 0);
    if (grew) resized = replaceReceiveQueue(kBulkRxBytes);
  }
  if (!grew) return;  // commands wait behind this one: the queue grows at one with nothing behind it
  if (resized) {
    LOG_INF("SER", "receive buffer armed: %u bytes", static_cast<unsigned>(kBulkRxBytes));
  } else {
    LOG_ERR("SER", "No %u bytes for the receive buffer: uploads may lose data", static_cast<unsigned>(kBulkRxBytes));
  }
}
