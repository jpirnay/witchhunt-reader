#pragma once
// Stage-1 compiled-content build gate (docs/compiled-book-pipeline-plan.md Phase 3).
//
// The walk → LayoutSink path ships unconditionally (the device build drives an internal
// LayoutSink). EPUB_STAGE1 gates the content.bin PERSISTENCE side: the ContentSink, the host
// content_stage1_dump tool, and the device Section content.bin write/read wiring in
// EpubReaderActivity (buildSectionFromContentBin read-back + one-time compileBookToContentBin; see
// docs/parser-stage1-content-bin-device-wiring-design-2026-07-26.md). Host test targets set it
// (-DEPUB_STAGE1=1); the device default env leaves it off, so the parse path is bit-for-bit
// unchanged. Overridable #ifndef guard so a target (or -D on the build) can flip it.
#ifndef EPUB_STAGE1
#define EPUB_STAGE1 0
#endif

// FRESH_READER_CONTENTBIN gates the CURSOR-NATIVE reader path (M2+): the EpubReaderActivity renders
// pages PURELY from content.bin via ContentBinCompiler::readPageAt + a PagePosition cursor, with the
// producer stepped cooperatively from the reader loop — the "one-producer" model. Opt-in per build
// (-DFRESH_READER_CONTENTBIN=1); only meaningful under EPUB_STAGE1 (ContentBinCompiler compiles only
// then). Default 0 → the reader behaves exactly as today (section-file + page-index path).
#ifndef FRESH_READER_CONTENTBIN
#define FRESH_READER_CONTENTBIN 0
#endif
