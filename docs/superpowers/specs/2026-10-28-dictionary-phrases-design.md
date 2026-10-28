# Dictionary phrases: phrase headwords, idioms inside an article, cross-references

Status: design agreed with the maintainer on 2026-10-28; to be built after the work in flight (CJK step 9 and the
rest of the CJK roadmap). Not scheduled yet. Amended the same day with the maintainer's answers to the plan's
questions: inflection stubs as base forms (5.2, 5.4), Oxford's inflection cues (5.4), the flash budget and the speed
gate's books (4).

## 1. Intent and why

The maintainer reads German and English books with two StarDict dictionaries on the card: the Oxford Dictionary of
English (English to English) and PONS Wörterbuch En-De (English to German). Switching between them is already
convenient. What is missing is **context**: a word that only makes sense as part of a phrase. Tapping *favor* in
"voted in favor of the motion" shows the noun and the verb; the reader wanted "in favour of".

The improvement must be **generic**: one mechanism for every language that separates words with spaces, driven by
what the dictionary contains, not a block of code per language. Flash and implementation cost matter more than
covering every case: matcha-reader's Japanese-only dictionary engine is the counter-example (compared on
2026-10-28: a custom format, a RAM-tier index, segmentation and deinflection for one language).

## 2. What the dictionaries actually contain

Measured on the maintainer's files (copied off the X4's card to `C:/tmp/dictrecon`, local only, never in the repo;
their content is copyrighted and must not be committed):

| | Oxford (90,602 entries) | PONS En-De (104,250 entries) |
|---|---|---|
| Multi-word headwords | 21,309, almost all compounds and names ("bank balance", "Banja Luka"). No idiom and no phrasal verb is a headword ("look up", "give up", "in favour of", "on the other hand" all absent) | 21,409: compounds **and phrasal verbs** ("look up", "give up", "make up for", "put up with"). Idioms are not headwords |
| Idioms inside the main word's article | yes: a `⧼PHRASES⧽` and a `⧼PHRASAL VERB⧽` section; each phrase is a `<b>…</b>` after a "▫" bullet, with placeholders: "in favour of", "take **something** into account", "on **someone's** account", "call (or **bring**) **someone** to account" | yes, inline: each phrase is a `<b>…</b>`, the headword written as **∼**, placeholders **sb** / **sth**, alternatives in brackets or with a slash: "in ∼ of sth", "to vote in ∼ of sth", "as a ∼ of fact", "to have a ∼ round" |
| US spelling | no *favor* entry at all (the article says "favour (US favor)") | *favor* is a 179-byte stub: "AM *see* **favour**"; 866 such cross-reference stubs |
| Inflected forms | mostly no entry (*looked*, *gives*: `resolve()`'s stemmer finds the base); irregular ones are stubs with cues of their own: *gave* is "past of **GIVE**" | entries of their own: *looked*, *gives*, *gave* each end in "↑ *look*" / "↑ *give*", the target in italics; 37,566 such stubs |
| Format | `sametypesequence=h`, plain `.dict` (58 MB), PocketBookDic conversion | `sametypesequence=h`, plain `.dict` (35 MB), PocketBookDic conversion |

So a phrase search over headwords alone would find PONS's phrasal verbs and nothing of the idioms; the idioms have
to be found inside the article of the word that was tapped.

## 3. Goals, non-goals, anti-goals

**Goals.** A lookup of a word that stands in a phrase shows that phrase's meaning first, in three parts:

- **A. Phrase headwords.** The longest dictionary headword made of the tapped word and its neighbours.
- **B. The idiom inside the article.** The definition opens at the bold phrase of the word's article that matches
  the text around the tapped word.
- **C. Cross-reference stubs.** An entry that only points to another headword opens that headword's entry.

**Non-goals.**
- Choosing the dictionary by book language, falling through to another dictionary on a miss (the maintainer:
  switching is already convenient).
- Deinflection or stemming beyond what `resolve()` already does for the tapped word.
- US/UK spelling tables. A dictionary that has stubs (PONS) is followed (C); one without (Oxford) is not helped.
- Phrases whose parts are far apart (a German separable verb with a long middle field).
- Japanese, Chinese and Korean: step 9 covers them; this design does not change a CJK lookup.
- A new dictionary format, a converter, shipped dictionaries, a new setting.

**Anti-goals** (a failure even if it works):
- Code that branches on the book's or the dictionary's language.
- A Latin lookup that becomes noticeably slower, or a cursor move that does more card reads than today.
- Steady-state RAM, or transient RAM beyond the existing lookup session and the definition buffer.
- Losing the word's own definition: the phrase comes first, the word stays reachable.

## 4. Constraints

- ESP32-C3, no PSRAM, about 380 KB RAM; the word screen and the definition screen already run close to the heap's
  edge on dense pages (step 9's spec, section 5.6, and Part B's measurements).
- No steady-state RAM; transient RAM within the lookup session plus a few bytes; stack locals under 256 bytes per
  function; no allocation in a scan loop. The definition buffer (`MAX_DEFINITION_BYTES`, 64 KB cap) is the only large
  buffer, and B and C work on it in place.
- **Speed gate (my call, after the builder check):** what the reader feels, the time from Confirm to the
  definition's first page on screen. Its median with phrases on stays within 1.5x master's on the X4, over about 50
  words from two English books (about half inside phrases the dictionaries hold, misses included), with Oxford and
  with PONS, timed from the log. Index work alone is not the measure: A does several descents where `resolve()` did
  one, and that is acceptable as long as the page appears about as fast. The two books (the maintainer, 2026-10-28):
  a generated, phrase-heavy test book, and `/test/Alice.epub` from the X4's card.
- **Flash:** at most 4.5 KB in `gh_release` for the whole feature (A, B, C and the two screens), measured from the
  map file (the maintainer, 2026-10-28; it was about 3 KB before the inflection stubs and the cues of 5.4); more needs
  the maintainer's yes.
- The first-hit behaviour of every existing lookup path stays as it is when nothing phrase-like is found: today's
  host tests stay green.

## 5. Design

### 5.1 The words around the tapped word

The word screen already holds the page's words in reading order (`DictionaryWordSelectActivity`, `words` /
`fragments`) and the selected index. The **context** of a lookup is the tapped word plus up to **three words before
it** and as many after it as fit **48 bytes** from the earliest start (`Dictionary::MAX_PREFIX_RUN_BYTES`, step 9's
run limit), joined with single spaces, within one paragraph:

- it crosses line ends, and paragraph ends too, as step 9's CJK run does: a page carries no paragraph marks
  (`PageLine` holds only its `TextBlock`), and adding one would bump `SECTION_FILE_VERSION` for every book. The
  punctuation stops below are the boundary (my call); a heading without final punctuation can join the next line,
  which only matters if the dictionary holds that exact phrase;
- it stops at a word that ends or starts with sentence punctuation (`. ! ? ; :` and their full-width forms) on the
  side away from the tapped word, and drops the punctuation itself; other punctuation (commas, quotes, brackets) is
  stripped from the word's edges the way `cleanWord` strips it today;
- a soft hyphen or a line-end hyphen inside a word is removed, as the word screen already does for lookups.

It is built only when the lookup runs (Confirm), not while the cursor moves.

### 5.2 A: phrase headwords

For each start in the context, from the earliest to the tapped word, run step 9's longest-prefix search
(`Dictionary::resolvePrefixes`) on the context from that start. A hit counts when it is **at least two words** long
and **covers the tapped word** (starts at or before it and ends after it). The longest counting hit wins; a tie goes
to the earlier start.

- Case: a sentence-initial capital must not stop "In favour of" from matching "in favour of". The prefix search
  compares bytes; the context is searched as written and, when its first letter is an uppercase ASCII or Latin-1
  letter, once more with that letter lowercased. (Builder: if StarDict's case-insensitive index order makes one
  search enough, use one.)
- **The tapped word's base form (my call).** When `resolve()` found the tapped word under another form (*looked* →
  *look*, *gives* → *give*), the starts up to the tapped word are searched again with that form in its place, so
  "she looked up the word" finds PONS's "look up". Only the tapped word is replaced, only when its form differs.
  **A stub gives the base form too** (the maintainer, 2026-10-28): PONS carries *looked*, *gives* and *gave* as
  entries of their own (section 2), so `resolve()` finds them as written. When the tapped word's own entry is a stub
  (5.4), the headword it points to is the base form (*looked* → *look*, *gave* → *give*).
- **A runs on a miss too (my call).** When the tapped word alone has no entry, Confirm still runs A, so *facto* in
  "de facto" or *hoc* in "ad hoc" finds its phrase. The speculative outline over a missed word does not change (it is
  index-only for the word); only a Confirm pays for A.
- **Cost.** Each start is its own index descent: a different first word lands elsewhere in the `.idx` (about nine
  sample reads and a window scan each, then up to three probes per file). Four starts, and up to four more for the
  base form, are several descents where the exact lookup did one. Open one lookup session for all of them rather
  than one per `resolvePrefixes` call (the builder: add a session parameter or an overload). The speed gate (section
  4) decides whether that is enough; if it is not, drop starts whose first probe shows no headword with that prefix.
  **The stub check before A is one extra read**: the tapped word's own entry, when it is small enough to be a stub
  (at most 2 KB; the largest stub measured is 1,045 bytes), is read before A rather than after it. When A finds
  nothing, that is the read the lookup makes anyway; when A finds a phrase, the entry is read a second time into the
  phrase's definition.
- What is shown: every entry of the phrase headword (step 9's `readPrefixDefinitions`), then the tapped word's own
  entries as today's lookup finds them (with `resolve()`'s fallbacks), in one definition, the phrase first. **When
  the phrase's entries alone fit the styled view's cap (`MAX_STYLED_HTML_BYTES`, 16 KB in
  `DictionaryDefinitionActivity`), the word's entries are cut to keep the whole under it (my call)**: PONS's "give up"
  (2.1 KB) plus *give* (14.8 KB) would otherwise drop to the plain-text view. Past the 64 KB cap the word's entries are
  cut as well, never the phrase's.

### 5.3 B: the idiom inside the article

When A finds nothing, the tapped word's own definition (as today's lookup returns it, after C) is scanned for a bold
phrase that matches the context. B runs only on HTML definitions (`sametypesequence` `h`, `x` or `g`); a plain-text
dictionary has no markup to find phrases by.

**Reading a bold phrase.** Each `<b>…</b>` of the definition, with nested tags removed and entities decoded:
- `∼` (U+223C) and `~` stand for the article's headword (the headword the lookup resolved to, so a tapped *favours*
  resolved to *favour*, or *favor* sent to *favour* by C, matches);
- **placeholders** match one to three context words: *someone, somebody, something, someone's, somebody's, one,
  one's, oneself, sb, sth, sb's, sth's, sb/sth*, and the German dictionary forms *jdn, jdm, jds, etw*; also `——`
  and `...`;
- **optional words** may be absent from the context: a leading *to* (PONS writes verbs as "to vote in ∼ of sth");
- **alternatives:** "(or X)" and "[or X]" replace the word before them; "x/y" offers either word. Each reading is
  tried. A bold run with an unclosed bracket (Oxford splits "in (or out of" across tags) is read as far as it goes.

The placeholder and optional-word lists are one small constant table: dictionary notation, not per-language code.
New notations are added there.

**Matching.** A bold phrase matches when, in the context, its words appear in order and contiguously (placeholders
absorbing one to three words), with **at least two literal words matched and the tapped word among them**. A
partial match counts: "took it into account" matches "take something into account" through "into account". The
best match has the most literal words; a tie goes to the first in the article.

**What is shown.** The same definition as today, opened on the page that holds the matched bold phrase instead of
page 1, with the phrase marked on that page. Both definition views support it:
- the styled view (`buildDictionaryHtmlPages`): the page whose text holds the phrase's words in order;
- the plain fallback view: the line that holds them.

How the phrase is marked is the builder's call (an underline or an inverted box over its words, whichever the
renderer already draws cheaply). Paging back from there reaches the start of the article as usual.

**Cost.** One pass over the definition already in RAM; no card reads.

### 5.4 C: cross-reference stubs

When the tapped word's definition is a stub that only points to another headword, the lookup shows that headword's
entry instead (one hop, never a chain), and B then runs on it.

A **stub** (rule revised after the builder check measured the first one on the maintainer's files): a definition
with at most 256 bytes of text after tags are removed, that ends with a bold run (nothing after it but punctuation
and closing tags), not inside `<i>`, **directly preceded by a cross-reference cue**, and whose bold run is a headword
of the dictionary (an exact lookup with none of `resolve()`'s fallbacks). **An italic target directly after the
"↑" cue counts as well** (the maintainer, 2026-10-28): that is how PONS writes its inflected forms, "↑ *look*", also
after a "pt of **give**" line. The target is read without a `<sup>` homograph number (Oxford's "FAVOUR¹").

The cues live in the same constant table as B's placeholders, as dictionary notation (my call): *see*, *see also*,
*↑*, *→*, *another term for*, *variant of*, *variant spelling of*, *pl of*, *pt of*, *pp of*, *abbrev of*,
*abbreviation of*, and the German dictionaries' *siehe* and *s.*. With them, Oxford's "another term for X" stubs are
followed too. **Oxford's inflection cues are in the table too** (the maintainer, 2026-10-28): *past of*, *past
participle of*, *past and past participle of*, *past tense of*, *plural of*, *short for*, *archaic spelling of*,
*dialect form of*, *comparative of*, *superlative of*.

Measured with the whole rule on the maintainer's files (each stub's target a headword, compared without ASCII case,
as `locate()` compares; a target naming its own entry not counted):

| | Oxford | PONS |
|---|---|---|
| Bold target after a cue of the first list | 2,080 (*another term for* 1,343, *see* 350, *variant of* 186, *see also* 84, *abbreviation of* 76, *variant spelling of* 41) | 1,108 (*see* 817, *abbrev of* 84, *pp of* 77, *pt of* 59, *pl of* 49, *see also* 21, *variant of* 1) |
| Bold target after an inflection cue | 408 (*short for* 135, *past and past participle of* 83, *past of* 76, *past participle of* 55, *archaic spelling of* 38, *plural of* 9, *dialect form of* 8, *superlative of* 2, *comparative of* 1, *past tense of* 1) | 13 (*short for*) |
| Italic target after "↑" | 0 | 37,566 (*looked*, *gives*, *gave* among them) |
| All stubs | 2,488 | 38,687 |

The largest stub measured is 1,045 bytes of HTML.

Why the cue is needed: without it the size-and-last-bold rule caught 9,104 real Oxford entries (it bolds labels and
derivatives: *skylight* → "n", *mendacious* → "-ious") and 2,593 PONS entries against its 866 real stubs
(*congruence* → "with").

The stub's own line (for example "AM see favour") is kept above the followed entry so the reader sees why the entry
changed.

### 5.5 Where the selection and the screen change

- The word screen's highlight and speculation do not change for Latin text: the phrase work happens at Confirm.
  (Growing the highlight before Confirm would need the article for B, which speculation never reads.)
- The definition screen gains: a starting page; a marked phrase on that page; and for A, the phrase headword as the
  header's headword (the word's own entries follow under their headword, as step 9 already lays out several entries).

## 6. What it depends on

- **Step 9 (the CJK dictionary), merged:** `resolvePrefixes`, `readPrefixDefinitions`, `MAX_PREFIX_RUN_BYTES`, and
  the word screen's run building (`CjkLookupRun`), which 5.1 generalises to space-separated words.
- **Part B of the step 9 plan** (`feat/dict-release-word-arrays`): the word arrays are freed while a definition is
  open; the context in 5.1 must be built before that.

## 7. Testing

- **Host tests** with dictionaries written by the tests (as `DictionaryTest`'s `writeDictionary` does), using
  **made-up** entries that copy only the two markup styles of section 2, never real dictionary text:
  - A: a phrase headword found from every start; the tapped-word coverage rule; the capital-first retry; the
    cut-for-room rule;
  - B: `∼` and spelled-out headwords, each placeholder, the optional *to*, both alternative forms, an unclosed
    bracket, partial matches, ties, no match, a plain dictionary (B off);
  - C: a stub followed once, a stub pointing to a missing headword (not followed), a long entry that ends in bold
    (not a stub), a chain (one hop only), an italic target after "↑" (followed) and anywhere else (not), an
    inflection stub giving A its base form;
  - the context builder: punctuation stops, line ends crossed, paragraph ends not, soft hyphens.
- **The X4** with the maintainer's Oxford and PONS on the card, a small test EPUB (sentences such as "voted in
  favor of the motion", "took it into account", "she looked it up", "as a matter of fact") and `/test/Alice.epub`:
  screenshots of each
  definition's first page; the speed gate (section 4); `CMD:STATE` heap on the definition screen against master.

## 8. Left to the builder

- How the phrase is marked on its page.
- Whether one case-insensitive search suffices (5.2).
- Where the context builder lives, and how it shares code with `CjkLookupRun`.
- The exact stub size threshold, if 256 bytes of text misses real stubs or catches real entries. Measured with the
  whole rule: bold stubs at 256 / 384 / 512 bytes number 2,488 / 2,756 / 2,856 in Oxford and 1,121 / 1,133 / 1,139
  in PONS. The Oxford entries between 256 and 384 bytes are full articles whose etymology ends in "(see X)", so 256
  stays.
- Whether `CMD:ACT:DICT` reports the phrase found (useful for the device run), and in which form.

## 9. Risks

- **False idiom matches** in long articles with many bold phrases: the two-literal-words rule and "the tapped word
  among them" keep a lone *of* or *in* from matching. If device screenshots still show wrong jumps, raise the minimum
  to three literal words for phrases of four or more.
- **The speed gate** for A on large dictionaries: four starts are four searches. Fallback: only the starts whose
  first word is a headword prefix that the first probe's window shows exists.
- **Bold used for other things** (Oxford bolds sense numbers and examples' key words): only bold runs with two or
  more words, or one word plus a placeholder, are phrase candidates.
- **Short articles that end in a cue.** 336 of Oxford's 2,488 stub matches are full articles of under 256 bytes
  whose etymology ends in "(see X)" or "short for X"; in 167 of them the cue sits inside an open bracket. They are
  followed as stubs, with their own text kept above X's entry, so nothing is lost but the header changes to X.
  PONS has none.
