# A cut or corrupt progressive JPEG draws a placeholder

Part of [Memory and parsing](../memory-and-parsing.md).

**Parked.** A progressive JPEG whose stream is cut short or corrupt fails as soon as a scan's data does not decode,
and the reader draws its framed placeholder (a cover is not converted). A failed decode of either kind draws that
placeholder, and the reader remembers the failure for the session instead of decoding the file again (#838;
[the image lane](../../background-rendering.md#image-lane--pixel-caches-ahead-of-the-reader), "A picture that will not
decode"). Work that draws what a damaged file holds is written and tested but parked: the decoder stops a scan where
its data is unusable and decodes every row from the scans that remain (a DC scan's blocks after the damage mid grey,
as libjpeg leaves them), so a cut or corrupt progressive file draws what it holds, and a cut baseline file keeps its
rows.

**Why it is parked.** Nobody has reported a damaged JPEG; the cut extracts it was written for came from the extractor
itself (fixed in `Epub::extractItemToFile`). And caching damage makes a false detection permanent: a valid file that
trips one of the new rules (the strict RSTn sequence, the padding overrun, a TJpgDec format error on an unusual
baseline file) would be kept with grey or white areas for good, since `PixelCache::PXC_MAGIC` is not tied to the rules
and cover BMPs are never invalidated. The triage (spec §8) left it parked.

**Where.** `lib/ProgressiveJpeg/ProgressiveJpeg.cpp` (`BitReader`, `decodeBand`, `indexStep`);
`JpegToFramebufferConverter::decodeToFramebuffer`; `JpegToBmpConverter.cpp` (`SinkSet`,
`JpegThumbSession::continueSteps`); `PixelCache::PXC_MAGIC` (`PixelCache.h`).

**Tried.** The code and its tests (`test/epub_pipeline/JpegPartialDecodeTest.cpp`, the `prog_420_cut.jpg` fixture) are
on the branch `archive/progressive-jpeg` (b623ca5a1). Its review found that once degraded pictures were cached,
`Result::Degraded` behaved exactly like `Ok` everywhere, so the completeness tracking that told a file lacking only its
EOI from a cut one bought nothing.

**Next step.** If it is ever taken, ship it decoder-only: fold `Degraded` into `Ok` with a `DecodeStats` flag for the
logs, drop that completeness tracking, and leave the converters as they are: they already cache `Ok`, and an I/O error
keeps the placeholder. Bump the checkpoint magic with the `Cursor` layout it changes, and note in `PixelCache.h` that a
change to the damage rules must bump `PXC_MAGIC`. At master 715896630 the branch was 1,727 commits behind, and a
trial merge conflicted in `ProgressiveJpeg.cpp/.h`, both converters, `Epub.cpp` and six test files (spec §8): taking
it is a re-port.
