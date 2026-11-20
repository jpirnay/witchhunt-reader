# The page-elements limit drops the tail of a page

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** A page holds at most `Page::MAX_ELEMENTS` elements (1,024, `Page.h:228`). `Page::serialize` writes the first
1,024 (`Page.cpp:647`), and a page that holds more is cut: `Section::onPageComplete` logs "tail dropped" and marks the
section simplified (`Section.cpp:730-735`), so the rest of the page is lost. The page is not ended at the limit during
the build, and `addPageElement` (`ChapterHtmlSlimParser.cpp:2091`) has no such check. It takes 1,025 elements on one
page, which needs elements without height (a real page is about 28 lines plus images and rules), and no book is known to
reach it. (`kCapPageElements`, `ChapterHtmlSlimParser.h:1481`, is declared and nothing sets it; the section marks itself.)

**Why it matters.** Content after the 1,024th element of a page vanishes, with a log line only. The limit exists so that a
page that cannot be loaded (`Page.cpp:687`) is never built.

**Where.** `Page::MAX_ELEMENTS` (`Page.h:228`); the write path `Page::serialize` (`Page.cpp:647`) and
`Section::onPageComplete` (`Section.cpp:730-735`); the single add site `ChapterHtmlSlimParser::addPageElement`
(`ChapterHtmlSlimParser.cpp:2091`) and `emitPage` (`:1814`).

**Next step.** End the page at the limit: an `emitPage()` from `addPageElement` when the page holds `MAX_ELEMENTS`. Not a
hint (decided 2026-11-18: no hint for a chapter a fixed limit simplified; 2026-11-19: the truncated-chapter hint stays
untranslated English). Only with a fixture that reaches the limit, which no book does today.
