# Detect Timezone keeps about 545 bytes a session on the Murphy

Part of [Wi-Fi session end](../wifi-session-end.md).

**Open, not explained.** Five Detect Timezone runs in a row on the Murphy (master ade8ab0ef, 2026-11-27) ended at
137,668, 137,116, 136,584, 136,024 and 135,492 B internal free, and Clock settings' heap fell 138,404 to 135,340 over
the five ([network-stack-findings.md](../../network-stack-findings.md), "Wi-Fi session end on the X4 Pro and the
Murphy"). The X4 Pro's five KOReader syncs kept about 24 B a session, the Picco's sessions 32 B. Each Detect Timezone
run makes two TLS requests to two hosts (api.ipify.org, timeapi.io); whether those, the JSON parse or the timezone
apply keep the memory was not narrowed down. At 545 B a session the Murphy reaches the minimums (67,584 B) after
more than a hundred runs in one boot, so the verdict bounds it.

**Next step:** five Detect Timezone runs on the X4 Pro, to see whether it is the screen or the board. Then, if it is
the screen, the heap-map method on the topic page around one run, or the same requests from a throwaway serial
command without the screen.
