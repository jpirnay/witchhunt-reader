# Writing modes: right-to-left and vertical text

Status: open items as of 2026-10-26.

Hebrew and Arabic books, and vertical Chinese, Japanese and Korean. None of it is built yet; the design and the
maintainer's decisions are in `docs/superpowers/specs/2026-10-26-rtl-and-vertical-text-architecture.md`:
- right-to-left bidi and Arabic shaping run once per paragraph at build time, and lines are stored in visual order;
- vertical text is a horizontal layout in a frame turned 90 degrees clockwise, not a second engine;
- the book decides the writing mode (CSS `writing-mode`, then OPF `page-progression-direction`), with a global setting
  and a per-book override;
- no right-to-left UI for now.

The turned frame was tried on the X3 (spec section 10, branch `spike/vertical-frame`): columns, ruby on the right,
sideways Latin and brackets all came out right, at the horizontal page's speed.

## Roadmap

Each step is its own plan and PR (spec section 7). After step 0, steps 1-2 and 3-4 do not depend on each other.

| Step | What | State |
|---|---|---|
| 0 | Parse every trigger (`writing-mode`, `text-orientation`, `text-combine-upright`, `direction`, `unicode-bidi`, `start`/`end`, `dir` with `<bdo>`/`<bdi>`, OPF ppd); CSS boxes along the writing mode, `start`/`end` along the direction; `line-height` parsed as CSS means it and kept through the cache; fixture books | built on `feat/writing-modes-step0` ([plan](../superpowers/plans/2026-10-26-writing-modes-step0.md)) |
| 1 | Bidi core and Hebrew; bidi controls from `unicode-bidi`, `<bdo>`, `<bdi>`; FlowContext and page progression | open |
| 2 | Arabic script | open |
| 3 | Vertical frame (from the spike); the vertical decision and its settings | open |
| 4 | Vertical typography: baked vertical forms, small kana, tate-chu-yoko (the per-word flag), A3a in vertical | open |
| 5 | Visual strings for titles and TOC; bidi in TXT and MD | open |

**Next step.** Line spacing (`cjk.md`, step 10), before steps 1-4 (maintainer, 2026-10-26): it reads the
`line-height` step 0 keeps, and it helps every horizontal CJK book now.

## Parsed in step 0, read later

Step 0 parses, caches and tests every trigger; these wait for the step that first reads them.

| What | Kept in | First read by |
|---|---|---|
| `writing-mode` (the section's root mode) | `CssStyle::writingMode` | step 3: the vertical decision |
| `text-orientation` | `CssStyle::textOrientation` | steps 3 and 4: upright or sideways runs |
| `text-combine-upright` | `CssStyle::textCombine` | step 4: tate-chu-yoko, as a per-word flag |
| `direction`, `dir` (beyond `start`/`end`) | `CssStyle::direction` | step 1: a paragraph's base direction |
| `unicode-bidi`, `<bdo>`, `<bdi>` | `CssStyle::unicodeBidi` | step 1: embeddings and isolates |
| OPF `page-progression-direction` | `Epub::pageProgression()` | step 1: page turns (reused by step 3) |
| `line-height` | `CssStyle::lineHeightEm` | line spacing (`cjk.md`, step 10) |

## Found while looking at real books

- **A Chinese book declares `cn`.** Not a language tag, so `TypographyProfile::forLanguage` gives it Latin rules. Read
  `cn` as `zh` (`TypographyProfile.cpp`).
- **Aozora Bunko conversions declare no writing mode.** They open horizontally under Automatic; the global setting's On
  is what sets them vertically (spec section 5.1).

**The acceptance book for steps 0, 3 and 4:** Hayakawa's 『七王国の玉座』 vol. 1 (`.idea/annas-arch-91dbd462043f.epub`,
local only, never in the repo): ppd `rtl`, `-webkit-writing-mode`, ~1,400 ruby groups, tate-chu-yoko, sesame dots, 12
images, ~380,000 CJK characters.

**The control for step 0:** the English original of the same novel (`.idea/A Game Of Thrones Book 1 -- Martin, George
R_ R_ -- 2011 -- chenjin5.epub`, local only): no `writing-mode` and 160 margin rules, so its page counts must not move.

## Left from the spike, for step 3

The reference run on the acceptance book is spec section 10.1: the same speed per page as horizontal, the same heap
peaks.
- **Images:** drawn in the turned frame they are rejected (a portrait image does not fit 792×528) and the placeholder
  turns sideways; their pixel caches were written at the turned size. Draw in the reader's frame, size with the
  reader's dimensions.
- **Decorations:** overlines and side lines land left of the column; in vertical they belong on the right.
- The dictionary's word boxes, taps and page progression were not touched.
