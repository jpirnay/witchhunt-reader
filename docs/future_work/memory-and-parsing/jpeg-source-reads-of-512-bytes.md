# JPEG source reads of 512 bytes (matcha-reader #245)

Part of [Memory and parsing](../memory-and-parsing.md).

**Open, needs a device measurement.** TJpgDec reads its input through `tjpgBmpInput` for covers
(`lib/JpegToBmpConverter/JpegToBmpConverter.cpp`) and `tjpgInput` in the reader
(`lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp`), one `HalFile` read of `JD_SZBUF` (512 B,
`lib/TJpgDec/src/tjpgdcnf.h`) per call. matcha-reader measured about 2 ms fixed per read and cut a cold decode by 22 %
with an 8 KB read-ahead. The SDK's sector read-ahead (freeink-sdk cf63426ef, pinned in ec93b64fb) serves the SDMMC
boards only (`SdmmcBlockDevice`); the X3 and X4 read over SPI without it. #220 found the same per-call cost on writes.
matcha-reader #245, Eszter Schuffert (eszter007).

**Why it matters.** Unknown until measured. The one figure here is 0.47 ms per 512 B refill on the X4, from the
dictionary's reads ([cjk.md](../cjk.md)), a quarter of matcha-reader's figure; there is no X3 figure.

**Next step (triage spec §15).** On the X3 and X4, dev build, a measurement-only patch: accumulate `micros()` inside
`tjpgInput` and log it with the decode's total for five cold baseline decodes of a 300-600 KB JPEG. Build the change
only if reads are 15 % or more of a cold decode on either board. The change, if built: raise `JD_SZBUF` to 2048 and
`TJPG_WORK_POOL_SIZE` from 12 KB to 14 KB in both converters, with no read-ahead wrapper (TJpgDec refills its own
buffer whole, from its pool, so the pool already is the buffer); the heap floors follow the constant, and the pool
still comes from the `image_scratch` arena first.
