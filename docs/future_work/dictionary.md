# Dictionary

Status: open items as of 2026-11-23. How the dictionary works is in [../dictionary.md](../dictionary.md). The open
items of lookups on Chinese, Japanese and Korean text are in [cjk.md](cjk.md#dictionary-on-cjk-pages).

## Phrases

**Planned, unblocked, not started.** The CJK dictionary it waited for merged as #574. A word that only makes sense in a
phrase ("favor" in "voted in favor of") shows the phrase's meaning first:
- **phrase headwords:** the longest headword made of the tapped word and its neighbours ("look up", "put up with");
- **the idiom inside the article:** the definition opens at the bold phrase of the word's article that matches the
  text ("in ∼ of sth", "take something into account");
- **cross-reference stubs:** an entry that only points to another ("AM see favour", "↑ look") opens that entry.

One mechanism for every language that separates words with spaces: a small table of dictionary notation
(placeholders such as *sb/sth/someone*, cues such as *see/↑/past of*), no code per language. Flash: up to 4.5 KB
(maintainer).

**Next step.** Carry out `docs/superpowers/plans/2026-10-28-dictionary-phrases.md` (its Task 1 checks it against
master first); the design is `docs/superpowers/specs/2026-10-28-dictionary-phrases-design.md`.

What was considered and not taken is in [docs/dictionary.md](../dictionary.md#what-the-reader-does-not-do).

## Some definitions fall back to plain text

**Open.** A definition laid out through the chapter parser (`buildDictionaryHtmlPages`, `src/util/DictHtmlPages.cpp`,
after `DictHtmlNormalize`) is shown in the plain-text viewer when its layout fails. Those entries lose their styling
(bold headwords, italics, indents) on the device. Two causes are known.

**1. The normalized entry does not parse** ("Definition is not parseable as XHTML", "yxml parse error"). Every entry of
the owner's two dictionaries was laid out on the host (the real normalizer and page builder against the
`epub_pipeline` host stubs, X4 and X3 body sizes); both builds failed the same entries:
- PONS Wörterbuch En-De (104,250 entries): 26 fail, and each of them contains `<mbp:pagebreak/>`, a Mobipocket tag
  (the tag occurs 26 times in the file).
- Oxford Dictionary of English (90,602 entries): 23 fail, 22 of them containing `<small>`. `<small>` alone is not
  the cause: 1,233 entries contain it and most lay out; 52 entries nest a `<small>` inside another.

Next step: dump the normalized XHTML of a failing entry from each dictionary and fix `DictHtmlNormalize` (for example
drop or rewrite a prefixed Mobipocket tag). The host harness that laid the dictionaries out is not in the repository;
a test should pin the fix with a made-up entry of the same shape.

**2. The styled layout stops on the heap** (`DHTML` "Styled definition stopped on", an error line on a path that
recovers). Seen with CC-CEDICT in a TrueType reader font, in a session whose glyph store did not yet hold the
definitions' glyphs: two of three definitions ran the free heap from 60 KB to 16 KB during the styled layout, stopped
on its retain floor and fell back to plain text. With the glyphs stored, and with the `.cpfont` family, all three laid
out styled (110-240 ms for 1.5-2.6 KB entries), on the X4.

Next step: find what the first-time layout holds on the heap. The TrueType misses of a glyph store that has not seen
the characters are the likely part (an inference from the two runs, not measured).

## Swipes in a long definition (CrossPoint #4363)

A definition longer than one screen pages by a tap on either half of its text, or by the page buttons
(`DictionaryDefinitionActivity::loop`); a swipe does nothing, because with the default bindings `GestureEventManager`
turns no swipe into a page button there. Touch boards only. Next step: page on a horizontal swipe as the reading page
does. crosspoint-reader #4363 (merged), Jadehawk (@jadehawk).

## A word split across a page break (matcha-reader #226)

Word selection joins a word that a hyphen splits over two lines (`DictionaryWordSelectActivity::joinHyphenatedWords`),
not one split over the page break. matcha appends the next page's first 8 characters to the lookup. Size S-M.

## `.dict.dz` without a resident chunk table (matcha-reader #619)

`DictZip` parses the dictzip chunk table, up to ~32 KB at `MAX_CHUNK_COUNT` 8192 (`src/util/DictZip.cpp`, the `RA`
subfield in `Scratch::loadInfo`), and holds it beside the 32 KB window, in the lent framebuffer or on the heap. On the
X4 the 48 KB buffer cannot hold the window and a large table, so a big `.dz` falls back to the heap. matcha sums the
chunk sizes it needs straight from the card and holds no table (44.1 to 36.7 KB for their test word). Its File
Transfer "Unpack dictionary" option (#620) is a feature on top. matcha-reader #619, Eszter Schuffert (eszter007).

## Vocabulary history (CrossPoint #4294): decide

Upstream records each successful lookup (word, a definition up to 2 KB, book, position) as JSON Lines in
`/Vocabulary.txt`: a 340-line store, a setting and a string. It is close to CrossPoint's highlights and clippings
(`My Clippings.txt`), which this firmware does not have ([../feature-comparison.md](../feature-comparison.md)); open
PR #300 adds highlights here. Decide whether it is in scope ([SCOPE.md](../../SCOPE.md)). crosspoint-reader #4294.
