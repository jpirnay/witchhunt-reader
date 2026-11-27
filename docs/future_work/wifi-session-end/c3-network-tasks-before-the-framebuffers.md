# The C3s: the network tasks before the framebuffers (decide)

Part of [Wi-Fi session end](../wifi-session-end.md).

**Open, decide, not tried.** The C3s restart after every Wi-Fi session. An X3 spike that took the secondary
framebuffer back at the session end instead failed in most cases: the first session leaves long-lived blocks inside
the 52 KB the network trim freed, so the buffer no longer fits, and when it did fit the next TLS session found too
little in one block ([network-stack-findings.md](../../network-stack-findings.md), "Wi-Fi session end on a C3 without
the restart"). On the Picco those blocks were three network tasks ESP-IDF cannot stop (lwIP's `tiT`, `sys_evt`,
`arduino_events`, about 15 KB) and about 3.5 KB of small ones.

**The lever left.** Start what the first session leaves behind (`esp_netif_init()`, the default event loop,
Arduino's event task) at boot, before the framebuffers are allocated, so it sits outside the region the trim gives
back. It costs those ~15 KB of heap at every boot, Wi-Fi used or not, on the boards whose heap is the binding limit,
and it does not touch the ~3.5 KB of small blocks or the TLS headroom.

**Next step, if wanted:** a spike that creates them first in `setup()`, then the same X3 runs: does the buffer come
back, and what is left for the next TLS session and for the reader? Weigh the boot-time cost against a restart that
costs about 1.2 s and the screens below.
