# A cut or corrupt progressive JPEG draws a placeholder

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** A progressive JPEG whose stream is cut short or corrupt fails as soon as a scan's data does not decode, and the reader draws its framed placeholder (a cover is not converted). Work that changes this is written and tested but parked: the decoder stops a scan where its data is unusable and decodes every row from the scans that remain (a DC scan's blocks after the damage mid grey, as libjpeg leaves them), so a cut or corrupt progressive file draws what it holds, and a cut baseline file keeps its rows.

**Done (8a).** A baseline JPEG that fails to decode draws the same framed placeholder a progressive one does (it drew nothing). `ImageBlock::render` remembers, per reader session, the pictures whose decode failed for the file's own fault (`DecodeFailures`, 8 x 4 B hashes of the path in `EpubReaderActivity`, cleared on exit, nothing on the card) and draws their frame without decoding again; a render pass no longer decodes a cut file eight times (X4: about 14 s a visit). Only a decode error counts: a progressive `InvalidData` or `Unsupported`, a baseline format error, or a short read at the end of the file; a card error, a refusal for memory and a preemption by input do not. What stays parked is drawing what a damaged file holds, and caching it.

**Why the rest is parked.** Nobody has reported a damaged JPEG; the cut extracts it was written for came from the extractor itself (fixed in `Epub::extractItemToFile`). And caching damage makes a false detection permanent: a valid file that trips one of the new rules (the strict RSTn sequence, the padding overrun, a TJpgDec format error on an unusual baseline file) would be kept with grey or white areas for good, since `PixelCache::PXC_MAGIC` is not tied to the rules and cover BMPs are never invalidated.

**Where.** `lib/ProgressiveJpeg` (`BitReader`, `ProgressiveJpeg.cpp:667`; `decodeBand`, `:965`; `indexStep`, `:575`);
`JpegToFramebufferConverter::decodeToFramebuffer` (`JpegToFramebufferConverter.cpp:1521`); `JpegToBmpConverter.cpp`
(`SinkSet`, `:246`; `JpegThumbSession::continueSteps`, `:1154`); `PixelCache::PXC_MAGIC` (`PixelCache.h:93`).

**Tried.** The code and its tests (`test/epub_pipeline/JpegPartialDecodeTest.cpp`, the `prog_420_cut.jpg` fixture) are on the branch `archive/progressive-jpeg` (b623ca5a1). Its review found that once degraded pictures were cached, `Result::Degraded` behaved exactly like `Ok` everywhere, so the completeness tracking that told a file lacking only its EOI from a cut one bought nothing.

**Next step.** If it is ever taken, ship it decoder-only: fold `Degraded` into `Ok` with a `DecodeStats` flag for the logs, drop that completeness tracking, and leave the converters as they are -- they already cache `Ok`, and an I/O error keeps the placeholder. Bump the checkpoint magic with the `Cursor` layout it changes, and note in `PixelCache.h` that a change to the damage rules must bump `PXC_MAGIC`.
