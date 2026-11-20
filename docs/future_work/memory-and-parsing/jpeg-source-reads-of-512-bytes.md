# JPEG source reads of 512 bytes (matcha-reader #245)

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** TJpgDec reads its input through `tjpgBmpInput` for covers
(`lib/JpegToBmpConverter/JpegToBmpConverter.cpp:57-66`) and `tjpgInput` in the reader
(`lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp:364-373`), one `HalFile` read of `JD_SZBUF` (512 B,
`lib/TJpgDec/src/tjpgdcnf.h:15`) per call. matcha measured about 2 ms fixed per read and cut a cold decode by 22 % with
an 8 KB read-ahead. The SDK's sector read-ahead (freeink-sdk cf63426ef, pinned in ec93b64fb) serves the SDMMC boards
only (`SdmmcBlockDevice`); the X3 and X4 read over SPI without it. Profile on the X3 and X4 first (#220 found the same
per-call cost on writes). matcha-reader #245, Eszter Schuffert (eszter007).
