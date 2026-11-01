# Design records

These are design records for shipped features, cited from code comments by path (and, for the list scheme, the CJK fonts, `cjk-ttf-fonts.md`, and CJK typography, `cjk-typography.md`, by section number). They describe why the code is shaped as it is; the code is the authority on what it does now.
Open work lives in `docs/future_work/`.

Code in `lib/EpdFont/ttf/` cites `cjk-ttf-fonts.md` as "spec N.N"; older comments there cite the TTF engine spec,
`docs/superpowers/specs/2026-10-13-ttf-fonts-design.md`.

The CJK layout code in `lib/Epub/Epub/` (`CjkPunctuation`, `TypographyProfile`, `ParsedText`) and its tests
(`test/cjk_layout/`, `test/typography/`, `test/epub_pipeline/CjkRulesTest.cpp`) cite `cjk-typography.md` as
"spec 5.N" (punctuation widths, push-in, the CJK–Latin gap, the grid). Their ruby comments ("spec 7.N") and the rule
checker's "spec section 9.1" cite the earlier CJK layout spec, `docs/superpowers/specs/2026-10-16-cjk-layout-design.md`
on branch `docs/cjk-layout-design` (commit `7e1136008`).

`lib/Epub/Epub/BookFonts.cpp` cites `book-fonts-probe.md` for the Publisher Fonts probe: which spine heads it reads,
how it ranks a book's families by their text, which faces it copies, and the first-open times behind its budget.
