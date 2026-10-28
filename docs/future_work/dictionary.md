# Dictionary

Status: open items as of 2026-10-28. The CJK dictionary (step 9) is tracked in [cjk.md](cjk.md#dictionary-on-cjk-pages).

## Phrases

**Planned, parked until the CJK dictionary has merged.** A word that only makes sense in a phrase ("favor" in "voted
in favor of") shows the phrase's meaning first:
- **phrase headwords:** the longest headword made of the tapped word and its neighbours ("look up", "put up with");
- **the idiom inside the article:** the definition opens at the bold phrase of the word's article that matches the
  text ("in ∼ of sth", "take something into account");
- **cross-reference stubs:** an entry that only points to another ("AM see favour", "↑ look") opens that entry.

One mechanism for every language that separates words with spaces: a small table of dictionary notation
(placeholders such as *sb/sth/someone*, cues such as *see/↑/past of*), no code per language. Flash: up to 4.5 KB
(maintainer, 2026-10-28).

**Next step.** Carry out `docs/superpowers/plans/2026-10-28-dictionary-phrases.md` (its Task 1 checks it against
master first); the design is `docs/superpowers/specs/2026-10-28-dictionary-phrases-design.md`.

## Considered and not taken (2026-10-28)

From a comparison with matcha-reader's dictionary support:
- **A dictionary chosen by the book's language, and falling through to the next dictionary on a miss:** switching
  dictionaries by hand is already convenient (maintainer).
- **matcha's own dictionary format** (a RAM-tier index, Japanese deinflection checked against parts of speech,
  Chinese segmentation by frequency): one language's worth of engine and a custom format, against StarDict-only and
  the flash budget.
- **Sentence mining to an Anki CSV:** a scope call; left out.
