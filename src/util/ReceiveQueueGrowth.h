#pragma once

#include <cstddef>

// When the USB serial receive queue may grow from its command size to the bulk size (SerialBuffers::arm()), kept
// free of the device so the rule can be tested on the host.
//
// HWCDC grows the queue by deleting it and creating a bigger one, and whatever waited in the old one goes with it:
// commands a host sent back to back behind the first one. So the queue grows once, at the first moment nothing waits
// in it. The caller asks on every command and when a bulk transfer starts; with commands queued behind, the last of
// them finds the queue empty and grows it. The idle queue already holds a whole command line
// (SerialBuffers::sizeForCommands()), so a long line is never cut while it waits.
class ReceiveQueueGrowth {
 public:
  // `queued`: the bytes waiting in the queue now, read with its interrupt masked. True when the caller should replace
  // the queue now; it is then grown for good, and never replaced again.
  bool growNow(const size_t queued) {
    if (grown_ || queued != 0) return false;
    grown_ = true;
    return true;
  }
  bool grown() const { return grown_; }

 private:
  bool grown_ = false;
};
