# Content-offset baseline (before any change)

- Host suite: 1464/1464.
- Round-trip allowances: test_table_cell_overflow {behind 2, ahead 0}; test_spine_toc_edges {behind 2, ahead 1}; all others {1, 0}.
- Goldens sha256-of-sha256s: f0b7d80e6e6c724558e253c33c7a7f6c257f2001c0ca090e71a4b345286af29f
- X3 (debug build, pre-change firmware; capture 2026-10-06 14:01-14:05, C:/tmp/x3-baseline.log):
  - Book B, *The Anarchy* "9 The Corpse of India" (spine 30, Calibre-wrapped): parse-and-build 7,143 ms; lowest during the build `free=42,176 contig=36,852` (one dip to `contig=30,708`); `Min Free` 35,176 B (set by the book open, not lowered by the build); no watermark drops during the build. Note: no power-cycle before this book.
  - Sync (manual, ended in an upload, fresh TLS handshake): before `free=63,804 contig=57,332`; `TLS handshake heap: preLargest=57,332 troughLargest=40,948 consumedContig=16,384 troughFree=50,412`; after the PUT `free=62,780 contig=49,140`.
  - Book A, *Quang Âm Chi Ngoại* "Chương 2955" (spine 2955, body-child paragraphs, 32 pages): parse-and-build 1,845 ms; lowest during the build `free=44,680 contig=36,852`; `Min Free` 26,604 B (26,608 before the chapter, set by opening the 2,956-spine book; not lowered by the build). Power-cycled before this book.
