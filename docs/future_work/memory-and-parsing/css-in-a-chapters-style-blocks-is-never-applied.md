# CSS in a chapter's `<style>` blocks is never applied (matcha-reader #627)

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** The parser skips `<head>` whole (`SKIP_TAGS`, `ChapterHtmlSlimParser.cpp:332`), `isNonVisibleTag()` counts a
`<style>` as invisible text (`lib/Epub/Epub/VisibleText.h:33`), and nothing parses its rules: only linked stylesheets
and `style=""` attributes reach the layout. A book whose only CSS is a `<style>` block in each chapter lays out
unstyled. matcha copies the block to a temp file (max 16 KB) and loads it like a linked sheet. First confirm with such
an EPUB. Size M, with a `SECTION_FILE_VERSION` bump.
