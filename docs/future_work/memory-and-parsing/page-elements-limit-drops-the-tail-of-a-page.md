# The page-elements limit drops the tail of a page

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** A page holds at most `Page::MAX_ELEMENTS` elements (1,024, `Page.h`). `Page::serialize` writes the first
1,024, and a page that holds more is cut: `Section::onPageComplete` logs "tail dropped" and marks the section
simplified, so the rest of the page is lost. The page is not ended at the limit during the build, and
`ChapterHtmlSlimParser::addPageElement` has no such check. It takes 1,025 elements on one page, which needs elements
without height (a real page is about 28 lines plus images and rules), and no book is known to reach it.
(`ChapterHtmlSlimParser::kCapPageElements` is declared and nothing sets it; the section marks itself.)

**Why it matters.** Content after the 1,024th element of a page vanishes, with a log line only. The limit exists so that a
page that cannot be loaded (`Page::deserialize` refuses more) is never built.

**Where.** `Page::MAX_ELEMENTS`; the write path `Page::serialize` and `Section::onPageComplete`; the single add site
`ChapterHtmlSlimParser::addPageElement` and `ChapterHtmlSlimParser::emitPage`.

**Next step.** End the page at the limit: an `emitPage()` from `addPageElement` when the page holds `MAX_ELEMENTS`. Not a
hint (decided 2026-11-18: no hint for a chapter a fixed limit simplified; 2026-11-19: the truncated-chapter hint stays
untranslated English, #831 closed). Only with a fixture that reaches the limit, which no book does today.
