# CSS in a chapter's `<style>` blocks is never applied (matcha-reader #627)

Part of [Memory and parsing](../memory-and-parsing.md).

**Open.** Only linked stylesheets and `style=""` attributes reach the layout. The parser skips `<head>` whole
(`SKIP_TAGS` in `ChapterHtmlSlimParser.cpp`), `isNonVisibleTag()` (`lib/Epub/Epub/VisibleText.h`) counts a `<style>`
as invisible text, and the head scan that scopes a chapter's stylesheets skips a `<style>`'s content
(`HeadingCarry::Scanner`). Nothing parses its rules. A book whose only CSS is a `<style>` block in each chapter lays out
unstyled. For the same reason an `@import` inside a head `<style>` is not seen: a chapter scoped to its linked sheets
loses the sheet that `@import` names ([Stylesheet scope](../../design/stylesheet-scope.md)). matcha-reader copies the
block to a temp file (at most 16 KB) and loads it like a linked sheet.

**Why it matters.** Hand-made and Sigil books put their CSS in `<style>` blocks, and Calibre moves the blocks into a
sheet (triage spec §12, not checked against a book here). The spec asks that the PR name one real book of that kind.

**Where.** The head scan before a build's parse: `Section::scopeStylesheets`, `HeadingCarry::scanHead`
(`HeadingCarry.h`). The compile: `CssParser::beginCacheCompile`, `appendCompiledFromStream`, `endCacheCompile`, and
the compile's heap gate in `Epub::parseCssFiles`. The cascade: `CssParser::resolve` and `CssParser::Cascade`.

**Next step.** First confirm on the host: a generator `test/epubs/make_test_style_block.py` whose chapters carry no
linked sheet, only a `<style>` block (alignment, italic by class, `display: none`, a margin); its golden today shows
the text unstyled and the `display: none` text visible. Then the design of triage spec §12: the head scan collects the
`<style>` text up to a cap (16 KB on the C3, more where `HalCapabilities::buildsInPsram()`), a second `CssParser`
compiles it into a cache file of the spine's own, and the overlay resolves after the book's rules, its orders offset so
a `<style>` rule beats a linked one of equal specificity. A block past the cap or a compile the heap refuses marks the
section `kStatusCssDegraded`. Size L, with a `SECTION_FILE_VERSION` bump. `BookFonts` and the body-size measurement
would not see the overlay; the PR says which readers of the book's CSS do.
