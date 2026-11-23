# CSS background pictures and `@font-face` rules stay book-wide

Part of [Memory and parsing](../memory-and-parsing.md).

**Open, only if a book shows it.** A chapter resolves its style rules against the stylesheets it links
([Stylesheet scope](../../design/stylesheet-scope.md), #843), but two side files of the compiled CSS are not ranged by
sheet: `css_bg.bin`, the `no-repeat` background pictures (a `CssParser::BackgroundImageRule` is a selector and a path,
with no order and no sheet), and `css_fonts.bin`, the `@font-face` rules. A picture or a face declared in a sheet a
chapter does not link still reaches it.

**Why it matters.** It is the same wrong rendering the scope fixed for rules, in two rarer places. No book is known to
show it.

**Where.** `CssParser::recordBackgroundImage`, `saveBackgroundImages`, `backgroundImageFor` (read by
`ChapterHtmlSlimParser` for a container's background picture); `CssParser::loadFontFaces` and `nextFontFace` (read by
`BookFonts`, which resolves against every sheet by design). The formats: [file-formats.md](../../file-formats.md),
"`css_rules.cache`, `css_bg.bin` and `css_fonts.bin`".

**Next step.** None until a book shows it. Then, for each file, Size S: keep the sheet each record comes from and skip
the records whose sheet is out of the chapter's scope, as `CssParser::Cascade::apply` does for rules (not designed
further), with a version bump of the file.
