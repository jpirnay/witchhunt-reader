#pragma once

// The USB serial wire's buffers, sized for the traffic there is.
//
// They used to be 4 KB in and 8 KB out from the moment a cable was seen, which took 12 KB of a C3's heap
// from anyone reading with the charger in. Now the wire opens with room for one command line, and the
// first command from a host, or the Serial Transfer screen, takes the 4 KB a bulk transfer needs: its
// 2,048-byte pieces come in a burst, and a queue that overflows drops bytes without a word. That stays
// until the next restart.
//
// The transmit ring stays at the driver's 256 bytes, on purpose. HWCDC frees it without its own lock, and
// any task may be logging, so it cannot safely be resized once the system runs; and it does not need to
// be. Measured on the X3 against 8 KB: a 1 MB download at the same 200 KB/s, a screenshot in 0.3 to 1 s for
// 0.27 s, and no boot log line lost.
namespace SerialBuffers {

// Before the wire opens, while nothing is reading it.
void sizeForCommands();

// Loop task only, as the wire's only reader: the old queue is deleted, so nothing may be reading it.
// Safe to call again.
void arm();

}  // namespace SerialBuffers
