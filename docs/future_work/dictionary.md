# Dictionary

Status: open items as of 2026-10-28; the items after Phrases added 2026-11-15, from the upstream triage.
The CJK dictionary (step 9) is tracked in [cjk.md](cjk.md#dictionary-on-cjk-pages).

## Phrases

**Planned, unblocked, not started.** The CJK dictionary it waited for merged as #574 (2026-10-29). A word that only
makes sense in a phrase ("favor" in "voted in favor of") shows the phrase's meaning first:
- **phrase headwords:** the longest headword made of the tapped word and its neighbours ("look up", "put up with");
- **the idiom inside the article:** the definition opens at the bold phrase of the word's article that matches the
  text ("in ∼ of sth", "take something into account");
- **cross-reference stubs:** an entry that only points to another ("AM see favour", "↑ look") opens that entry.

One mechanism for every language that separates words with spaces: a small table of dictionary notation
(placeholders such as *sb/sth/someone*, cues such as *see/↑/past of*), no code per language. Flash: up to 4.5 KB
(maintainer, 2026-10-28).

**Next step.** Carry out `docs/superpowers/plans/2026-10-28-dictionary-phrases.md` (its Task 1 checks it against
master first); the design is `docs/superpowers/specs/2026-10-28-dictionary-phrases-design.md`.

What was considered and not taken is in [docs/dictionary.md](../dictionary.md#what-the-reader-does-not-do).

## Swipes in a long definition (CrossPoint #4363)

A definition longer than one screen pages by a tap on either half of its text, or by the page buttons
(`DictionaryDefinitionActivity.cpp:264-283`); a swipe does nothing, because with the default bindings
`GestureEventManager` turns no swipe into a page button there. Touch boards only. Next step: page on a horizontal
swipe as the reading page does. crosspoint-reader #4363 (merged), Jadehawk (@jadehawk).

## A word split across a page break (matcha-reader #226)

Word selection joins a word that a hyphen splits over two lines (`DictionaryWordSelectActivity::joinHyphenatedWords`),
not one split over the page break. matcha appends the next page's first 8 characters to the lookup. Size S-M.

## `.dict.dz` without a resident chunk table (matcha-reader #619)

`DictZip` parses the dictzip chunk table, up to ~32 KB at `MAX_CHUNK_COUNT` 8192 (`src/util/DictZip.cpp:19,250-255`),
and holds it beside the 32 KB window, in the lent framebuffer or on the heap. On the X4 the 48 KB buffer cannot hold
the window and a large table, so a big `.dz` falls back to the heap. matcha sums the chunk sizes it needs straight
from the card and holds no table (44.1 to 36.7 KB for their test word). Its File Transfer "Unpack dictionary" option
(#620) is a feature on top. matcha-reader #619, Eszter Schuffert (eszter007).

## Vocabulary history (CrossPoint #4294): decide

Upstream records each successful lookup (word, a definition up to 2 KB, book, position) as JSON Lines in
`/Vocabulary.txt`: a 340-line store, a setting and a string. It is close to the clippings and highlights export that
was declined. Decide whether it is in scope ([SCOPE.md](../../SCOPE.md)). crosspoint-reader #4294.
