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
| 0 | Parse `dir`, `direction`, `writing-mode`, `text-combine-upright`, `start`/`end`, OPF ppd; FlowContext; page progression in the reader; generated fixture books | plan to write |
| 1 | Bidi core and Hebrew | open |
| 2 | Arabic script | open |
| 3 | Vertical frame (from the spike) | open |
| 4 | Vertical typography: baked vertical forms, small kana, tate-chu-yoko, A3a in vertical | open |
| 5 | Visual strings for titles and TOC; bidi in TXT and MD | open |

**Next step.** Write the step 0 plan.

## Found while looking at real books

- **A Chinese book declares `cn`.** Not a language tag, so `TypographyProfile::forLanguage` gives it Latin rules. Read
  `cn` as `zh` (`TypographyProfile.cpp`).
- **Aozora Bunko conversions declare no writing mode.** They open horizontally under Automatic; the global setting's On
  is what sets them vertically (spec section 5.1).
- **A calibre-processed commercial book says it only as `-webkit-writing-mode: vertical-rl`,** with no unprefixed
  property, through a class on `<body>`; its tate-chu-yoko (`text-combine-upright: all` with `-webkit-text-combine:
  horizontal`) and sesame dots arrive through renamed classes too. Step 0 must read the prefixed property and resolve
  `body`'s classes through the cascade.

**The acceptance book for steps 0, 3 and 4:** Hayakawa's 『七王国の玉座』 vol. 1 (`.idea/annas-arch-91dbd462043f.epub`,
local only, never in the repo): ppd `rtl`, `-webkit-writing-mode`, ~1,400 ruby groups, tate-chu-yoko, sesame dots, 12
images, ~380,000 CJK characters.

## Left from the spike, for step 3

- The pre-render costs ~4 KB more heap in vertical (6.4-10.4 KB against 5.8-6.1 KB on the same pages). First suspect:
  the per-character advance and ascender lookups during the prewarm scan.
- Images, the dictionary's word boxes, taps and page progression were not touched.
