# Stylesheet scope: a chapter resolves against the sheets it links — design

> **Status.** Implemented: the sheet table (#835) and the per-chapter scope (#843). Code: `lib/Epub/Epub/css/CssParser.*`
> (`sheetRanges`, `sheetImports`, `addLink`, `scopeToSheets`, `Cascade::apply`), `Section::scopeStylesheets` and
> `Section::prepareCarry` (`lib/Epub/Epub/Section.cpp`), `HeadingCarry::Scanner` and `HeadingCarry::scanHead`
> (`lib/Epub/Epub/HeadingCarry.*`). The table's layout: [file-formats.md](../file-formats.md), "`css_rules.cache`,
> `css_bg.bin` and `css_fonts.bin`". Tests: `test/epub_css/CssParserCacheTest.cpp`,
> `test/epub_pipeline/CssSheetRangesTest.cpp`, the `test_css_scoping` golden (`test/epubs/make_test_css_scoping.py`).
> Open: [CSS background pictures and `@font-face` rules stay
> book-wide](../future_work/memory-and-parsing/css-backgrounds-and-font-faces-stay-book-wide.md), [CSS in a chapter's
> `<style>` blocks is never applied](../future_work/memory-and-parsing/css-in-a-chapters-style-blocks-is-never-applied.md).

This record says why the code is shaped as it is; the code is the authority on what it does now.

## Problem

Every stylesheet of the manifest applied to every chapter. A sheet that no text chapter links reached every chapter
(matcha-reader #453, Eszter Schuffert, eszter007: matcha-reader's case was a `vertical_image.css` whose
`body { text-align: center }` centred every chapter), and an omnibus edition's part sheets, which reuse class names,
styled each other's parts. In this engine an unlinked sheet's element and class rules, its inherited `body` font
properties, its `body` margins and a root `writing-mode` reached every chapter (#843). matcha-reader #453's exact case does not occur here, because
`text-align` is decided per element.

## Each sheet owns a range of rule orders (#835)

A rule's `order` comes from one counter that runs across the sheets in the order they are compiled (the manifest's,
then those found only in the archive), so each sheet already owns one contiguous range of orders. Scoping a chapter is
a filter on `order`. The rule records do not change, and neither does the index.

- **The sheet table** records each sheet's range and its `@import` edges at the end of `css_rules.cache`. At the end,
  not in the header, because the index offsets are absolute: a table in the header would move them all. It is written
  last, after the index is patched, so a file that ends in a valid table is complete; one that does not is deleted
  and the CSS compiled again (#843).
- **`@import` is recorded, not followed.** The tokenizer drops a top-level at-rule at its `;`, so an imported sheet
  applied only because every sheet applied. Without the edges, scoping would unstyle every book that imports. Only the
  `@import`s before a sheet's first rule count, and only those that name a file in the book (file-formats.md has the
  rule).
- **A redeclaration stays inside its sheet.** The same selector at the start of the next sheet used to merge into the
  previous sheet's record, which took the later sheet's order. A merge guard keeps one record per sheet. The triage's
  alternative, a gap of two orders at each sheet's start, would have renumbered every rule after the first sheet. The
  winning declaration is the same for every book; each cross-sheet redeclaration costs one more record against
  `MAX_RULES` (1,500).
- **Limits.** `MAX_SHEETS` (64): a book with more folds its 64th sheet and every later one into one last range with no
  path, which applies to every chapter. Past `MAX_SHEET_IMPORTS` (128) imports, when the orders run out, or when the
  compile has no heap for the table, the table is written empty, and every sheet applies to every chapter.

## A chapter's scope (#843)

- **Reading the links.** `Section::scopeStylesheets` runs before the parser's first style lookup and reads the
  chapter's head from its inflated XHTML (`html_<spine>.bin`) with the scanner the heading carry already has
  (`HeadingCarry::Scanner`'s link hook, `HeadingCarry::scanHead`): up to `<body>`, at most
  `HeadingCarry::kMaxHeadBytes` (8 KB), through a 256 B window, the links' values in a 256 B buffer, all on the stack.
  A review cut the change from about 3.7 KB of flash to about 2.8 KB (object sizes, #843), mainly by reusing that
  scanner instead of a second tokenizer. A carried heading file's links come from the scan `Section::prepareCarry` makes of it. With
  one sheet or none the head is not read.
- **The filter.** `CssParser::addLink` resolves each href against the chapter's path, `CssParser::scopeToSheets`
  follows the import edges at any depth and keeps one bit a sheet, and `CssParser::Cascade::apply` skips a rule whose
  sheet is out of scope, for every store and the complex rules alike. One place: a chapter that links every sheet
  takes no test per rule, and its layout is byte-identical.
- **Memory.** The table is loaded with the index, on the heap (16 B to 1.5 KB). A C3 build's lent region has no room
  for a block that lives the whole build ([Memory Allocation Strategy
  §8.2](../memory-allocation-strategy.md#82-the-arena-by-moment)).
- **The log.** A build logs how many of the book's sheets it resolves against
  (`spine=N resolves against X of Y stylesheets`), so a report of a chapter that lost its styling can be read from its
  log.
- `SECTION_FILE_VERSION` 128 and `CSS_CACHE_VERSION` 36 at the time; both moved on since.

## Where every sheet still applies

A chapter resolves against every sheet, as before, when nothing tells which sheets it means:

- a chapter that links no sheet;
- a link or an import the table does not know: a path spelt another way, an import of a file the book does not have
  (nothing tells it from a sheet the reader failed to find);
- a head past 8 KB, or one not read whole;
- a `rel` or `href` not quoted, or past 256 B;
- a link to a sheet past the 64th (its range is the fold, which has no path);
- a carried heading file that links no sheet;
- an empty, folded or missing sheet table.

`BookFonts` and the body-size measurement (`BodySize`) resolve against every sheet by design.

## What the scan does not see

These are the only ways a linked sheet drops out of a scoped chapter:

- a stylesheet `<link>` in the body;
- an `@import` inside a `<style>` block in the head (the `<style>` text is not read at all: [the `<style>`
  item](../future_work/memory-and-parsing/css-in-a-chapters-style-blocks-is-never-applied.md));
- a sheet named by an `<?xml-stylesheet?>` instruction beside `<link>`s.

A prefixed `<h:link>` is seen: the scanner reads local names.

## Book-wide still

The background pictures (`css_bg.bin`) and the `@font-face` rules (`css_fonts.bin`) are not ranged: a picture or a
face from a sheet a chapter does not link still reaches it. That is an open item, worth doing only if a book shows it.
