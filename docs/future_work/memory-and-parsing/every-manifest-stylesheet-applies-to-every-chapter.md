# Every manifest stylesheet applies to every chapter (matcha-reader #453)

Part of [Memory and parsing](../memory-and-parsing.md).

**Done (13a and 13b); what is left is below. Next in this area:
[the `<style>` blocks](css-in-a-chapters-style-blocks-is-never-applied.md).** A chapter
resolves against the stylesheets its head links and the sheets those `@import`, as a browser does, not against every
sheet of the book: the rules of a sheet that no text chapter links (matcha's `vertical_image.css`) reached every
chapter, and an omnibus edition's part sheets with the same class names each other's. matcha-reader #453, Eszter
Schuffert (eszter007).

- **13a (`CSS_CACHE_VERSION` 36).** The compile records, at the end of `css_rules.cache`, each sheet's range of rule
  orders and its `@import` edges (`CssParser::sheetRanges`, `sheetImports`; layout in
  [file-formats.md](../../file-formats.md#css_rulescache-css_bgbin-and-css_fontsbin)). A redeclaration no longer merges
  across a sheet boundary.
- **13b (`SECTION_FILE_VERSION` 128).** `Section::scopeStylesheets` reads the chapter's head from `html_<spine>.bin`
  before the parser's setup, through the scanner the heading carry already has (`HeadingCarry::Scanner`'s link hook,
  `HeadingCarry::scanHead`: up to `<body>`, at most 8 KB, a 256 B window and the links' values in a 256 B buffer, on
  the stack); a carried heading file's links come from the scan `Section::prepareCarry` makes of it.
  `CssParser::addLink` resolves each href, `CssParser::scopeToSheets` follows the import edges and keeps one bit a
  sheet, and `CssParser::Cascade::apply` skips a rule whose sheet is out of scope, for every store and the complex
  rules alike. The table is loaded with the index, on the heap (16 B to 1.5 KB; a build's lent region has no room for a
  block that lives the whole build), and written last: a cache that ends in no valid table is deleted and compiled
  again. 128 imports now (64 before), and an `@import` after `@namespace` no longer counts.
- **Unscoped, as before.** A chapter that links no sheet; a link or an import the table does not know (a path spelt
  another way, an import of a file the book does not have: nothing tells it from a sheet the reader failed to find);
  a head past 8 KB or not read whole; a `rel` or `href` not quoted or past 256 B; a link to a sheet past the 64th (its
  range is the fold, which has no path); a chapter that links every sheet. A build logs how many of the book's sheets
  it resolves against. `BookFonts` and `BodySize` resolve against every sheet by design.
- **Not seen, so the sheet drops out of a scoped chapter** (the only ways a linked sheet is lost): a stylesheet
  `<link>` in the body; an `@import` inside a `<style>` block in the head; a sheet named by an `<?xml-stylesheet?>`
  instruction beside `<link>`s. A prefixed `<h:link>` is seen (the scanner reads local names).
- **Open.** The background pictures (`css_bg.bin`) and the `@font-face` rules (`css_fonts.bin`) are not ranged and stay
  book-wide: a picture or a face from a sheet a chapter does not link still reaches it. Size S each, if a book shows it.
