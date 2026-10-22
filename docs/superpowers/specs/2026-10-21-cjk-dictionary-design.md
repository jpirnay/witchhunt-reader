# Dictionary lookups on CJK pages: design

Status (2026-10-22): **Decided 2026-10-22: the maintainer answered the questions below; the implementation plan comes next.**

Working material, like every spec here: when the work merges, lasting parts move to `docs/dictionary.md`,
open items to `docs/future_work/cjk.md`, and this file is deleted.

## Maintainer decisions (2026-10-22)

1. A CJK definition draws in the reader's CJK font now, in this work, and does not wait for roadmap step 6. A lookup that started on CJK text draws its definition in the reader's font and puts the headword at the top of the body instead of in the UI header (5.3).
2. The selection grows automatically to the longest dictionary match from the selected character, with no extra press (5.1). Upstream #3932's manual start/end range is not taken.
3. The definition lists up to two shorter matches after the longest (大学生, then 大学, then 大). There is no shrink gesture.
4. No Japanese or Korean deinflection this round (5.5).
5. StarDict only, converted on a computer with PyGlossary, documented; no dictionaries shipped and no conversion in the web page. My call (controller): the existing reader and installer need no new format code.
6. Three PRs, in this order. My call (controller): the first two stand alone and shrink the risk of the third.
   - the log-noise fix (L1);
   - releasing the word arrays while a definition is open (H1);
   - the CJK lookup.

## 1. Intent and why

A reader of a Chinese or Japanese book highlights a character, presses Confirm, and gets either "Not found"
or the meaning of one character. They wanted the word. Chinese and Japanese are written without spaces,
so a word is two, three or four characters the layout does not mark as a unit. Learners need the
dictionary most, and they cannot always tell where a word ends.

The fix uses what is already on the card. A StarDict index is sorted, and the reader already searches it
with one descent and a short scan. The same scan also passes every dictionary word that starts with the
cursor's character. The dictionary itself is the word list, and segmentation costs about one lookup.

SCOPE.md lists "Reference Tools: local dictionary lookup" and "Language Support" as in scope. This
feature adds no screen and no setting, and needs no steady-state RAM.

## 2. Goals, non-goals and anti-goals

**Goals.**
- On a Han or kana character, one Confirm looks up the longest dictionary word that starts there.
- It works on boards without touch (X3, X4) with no more presses than today, and on touch boards (X4 Pro,
  T5S3) with the existing tap, then tap again.
- The definition shows Han, kana and pinyin readably.
- No steady-state RAM. Transient RAM is no larger than the existing lookup session, plus a few bytes.
- Lookup time stays close to today's exact lookup (section 7 states the target).
- CC-CEDICT and JMdict in StarDict form work, and the docs say how to get them.
- An HTML dictionary's definition is styled on a dense CJK page whenever the heap allows it (section 5.6
  measures whether it can).
- No error-level log line on a path that recovers.
- Latin-script lookups behave exactly as today, and the existing host tests stay green.

**Non-goals (not this round).**
- Japanese and Korean deinflection (食べた → 食べる).
- A manual range or phrase selection, and selection across pages.
- Moving the cursor word by word instead of character by character.
- Looking a word up from inside a definition (upstream #4063).
- Drawing CJK in the UI header (roadmap step 6).
- Converting between simplified and traditional Chinese. We rely on the dictionary carrying both scripts.
- Vertical text.

**Anti-goals (never).**
- A word segmenter or morphological analyzer on the device (jieba, MeCab, Sudachi). Their lexicons are tens
  of MB, and the StarDict index already is a lexicon.
- Reading formats other than StarDict on the device or in the firmware's web page (EPWING, MDict, Yomitan
  zips, the CC-CEDICT text file).
- Shipping a dictionary in flash or in the firmware image.
- Online lookups.
- A "CJK mode" setting. The script of the selected text decides.

## 3. How it works today

### 3.1 Lookup

- **The index.** `/dictionaries/<folder>/<stem>.idx` is a list of `headword NUL, BE32 offset, BE32 size`
  entries, sorted by StarDict's ASCII-case-folded byte compare. `StringUtils::asciiCaseCmp`
  (`src/util/StringUtils.h:20-32`) is the same compare. For CJK keys it is plain byte order, which UTF-8
  makes code point order. That is what makes a prefix search valid (section 5.2).
- **The search.** A sidecar `.qidx` holds the byte offset of every 256th entry (`SAMPLE_INTERVAL`,
  `Dictionary.h:132`). `locate()` (`Dictionary.cpp:548-587`) bisects the samples to the last one at or
  below the target. It then scans forward through at most 256 entries and stops at the first headword above
  the target (`Dictionary.cpp:575`). Reads go through two 512-byte buffers held for the session
  (`SCAN_BUF_BYTES`, `Dictionary.h:138`). A `.syn` file gets the same treatment through `.sidx`, and its
  ordinals resolve back through `.qidx` (`locateSynonym`, `locateByOrdinal`).
- **Matching is exact**, with fallbacks (`Dictionary::resolve`, `Dictionary.cpp:907-1008`): the word, then
  `.syn`, then English stem variants, then the word lowercased (non-ASCII), then a French elision's
  remainder, then the parts of a hyphenated compound. None of them helps a Han character.
- **Cleaning.** `cleanWord()` strips CJK and fullwidth punctuation from the edges (`EDGE_PUNCTUATION`,
  `Dictionary.cpp:76-90`) and folds ASCII case.
- **Reading.** `readDefinition()` reads at most 64 KB (`MAX_DEFINITION_BYTES`) and refuses when the largest
  free block is under size + 8 KB (`Dictionary.cpp:698`). A `.dict.dz` needs a 32 KB inflate window held for
  the session (`DictZip::Scratch`). A plain `.dict` needs none, and the docs recommend it
  (`docs/dictionary.md:31`).

### 3.2 Selection

- `DictionaryWordSelectActivity` owns a copy of the page. After the first render it collects every token
  into a `Fragment` (16 bytes, `static_assert` at `DictionaryWordSelectActivity.h:74`) and groups glued
  fragments into a `Word` (10 bytes by its fields) (`extractWords`, `DictionaryWordSelectActivity.cpp:138-316`).
- **Glue is the only grouping.** `TextBlock` keeps only the `continues` half of a token boundary, in bit 7
  of the style byte (`TextBlock.h:237`). A CJK gap is `continues=false, noSpaceBefore=true`
  (`TokenBoundary.h:34-37`), so every Han or kana character is its own `Word`. Only kinsoku glue joins
  punctuation to a character (大。). That is the "selection is per token" of `docs/future_work/cjk.md`.
- **A page carries no paragraph marks.** A `PageLine` holds its line's own `TextBlock` (`Page.h:46-57`),
  so the word screen cannot tell a line wrap from a paragraph end.
- **Arrays are reserved from a real count**, and on a CJK page they compete with the font cache. The code
  records the case: 600 and more tokens on a dense page, ~29 KB of glyph bitmaps, and no block over
  4-7 KB on the X3 with Hong Lou Meng at 14 pt (`DictionaryWordSelectActivity.cpp:152-156`). The fallback
  releases the font caches and tries again.
- **Speculation.** Once the cursor has been still for 300 ms (`SPECULATIVE_DEBOUNCE_MS`), the screen runs
  `resolve()` (index only). A known miss is drawn as an outline instead of a fill (`speculateForSelection`,
  `DictionaryWordSelectActivity.cpp:451-486`). Confirm reuses the result (`performLookup`, 564-572).
- **Highlight repaint** is differential. The pixels under the box are saved (at most 2 runs, 2048 bytes,
  `DictionaryWordSelectActivity.h:235-236`) and restored on the next move. A box that does not fit falls
  back to the full repaint (`render`, 886-892).

### 3.3 The definition screen

- **Font.** It is pinned to `NOTOSANS_14_FONT_ID` (`DictionaryDefinitionActivity.h:28`) for the phonetic
  alphabet. That face is built from `NotoSans-Regular.ttf`: Latin, Cyrillic, IPA and Latin Extended-B,
  which has the pinyin tone vowels ǎ ǐ ǒ ǔ (`fontconvert.py:67-110`, `notosans_14_regular.h:7`). It has no
  CJK. The CJK UI faces are attached only to `notosans_8` and Inter 10/12/14 (`CjkUiFonts.cpp:71`), and they
  hold only the translations' characters (`CjkUiFonts.h:24-26`). A missing glyph draws `REPLACEMENT_GLYPH`
  (`EpdFont.cpp:265-281`). **So today a CJK definition cannot be read on this screen.**
- **Header.** The headword is drawn by `GUI.drawHeader` in the UI font
  (`DictionaryDefinitionActivity.cpp:353-354`), which shows boxes for CJK it does not cover.
- **Styled path.** An HTML definition (`sametypesequence=h`) up to 16 KB goes through the chapter parser
  (`buildDictionaryHtmlPages`). It needs 40 KB free and a 20 KB block to start (`DictHtmlPages.cpp:16-17`).
  Otherwise it logs `DHTML` "Low heap for styled definition" and falls back to plain text. The gate was
  measured: a 2,554-byte definition's layout costs ~21 KB while it runs (`DictHtmlPages.cpp:21-26`).

### 3.4 Input

- **X3 and X4 (ESP32-C3):** no touch. Front Back, Confirm, Left, Right; side Up and Down; Power. The word
  screen moves the cursor by logical direction (`DictionaryWordSelectActivity.cpp:703-713`).
- **Long presses** fire at 1,000 ms (`ButtonEventManager.h:51`) and are configurable per button.
  `btnLongConfirm` defaults to "default" (it falls through to the screen). `btnLongLeft` and `btnLongRight`
  default to previous and next section (`CrossPointSettings.h:876-878`). Those are reader-scoped, so they
  fall through outside the reader. A user who binds a global action to a long press takes it from every
  screen (`main.cpp:1812-1906`).
- **X4 Pro and T5S3 (ESP32-S3, 8 MB PSRAM):** GT911 touch, plus two nav keys or one user key
  (`docs/contributing/board-support.md:11-12`). A tap moves the cursor, and a second tap on the selected
  word looks it up (`consumeWordTouch`, `DictionaryWordSelectActivity.cpp:392-416`).

### 3.5 Upstream (crosspoint-reader `develop` at f8936ba6, 2026-10-21)

- **#3932** (merged 2026-10-10) replaced word selection with the clipping selector in a Lookup mode. Confirm
  anchors, the cursor extends the range, and Confirm looks it up. There is no segmentation. It closed issue
  **#2656**, which names Chinese, Japanese, Vietnamese and Thai as the reason. The selector caps a page set
  at 240 selectable words (`MAX_SELECTABLE_WORDS` in its `ClipSelectionActivity.h`), so a 600-character
  CJK page is selectable only up to roughly its first 240 characters.
- **#2942** (closed, unmerged) proposed a longest-prefix fallback for Turkish and German. During the exact
  scan it remembers the longest headword that is a proper prefix of the target, with zero extra reads.
  Section 5.2 uses the same idea.
- **#3624** (open) adds a dictionary font setting. Upstream draws definitions in the reading font
  (upstream `docs/dictionary.md:65`).
- **#4112** (draft, 2026-10-21) changes the same SD-font prewarm failure path as the log noise
  (section 5.7). We should watch it.
- Nothing upstream does CJK segmentation, longest match or deinflection.

## 4. Constraints

- **Heap (C3, ~380 KB, no PSRAM).** On a dense CJK page the word screen holds the page copy, the two
  arrays (~600 × 16 B + ~600 × 10 B ≈ 16 KB, estimated from the struct sizes) and the 2 KB snapshot,
  on top of the reader. The largest block can be 4-7 KB before the font cache is released (section 3.2).
  Anything the lookup adds must live in the existing session buffers or be a few bytes.
- **No paragraph marks** on the page (section 3.2). A run of characters can cross from one paragraph into
  the next.
- **Two refreshes per cursor stop.** The highlight moves at once, and the speculative verdict repaints it
  (`speculationRepaintPending`). A grown span is the same second repaint, not a new one.
- **Snapshot size.** A span drawn on the differential path must fit 2 runs and 2,048 bytes (16,384 px).
  Larger spans take the full repaint, which works but is slower.
- **Long presses are contested.** They are user-configurable, and on the C3 boards a hold costs a second
  before anything happens (section 3.4).
- **The index order is fixed by StarDict.** We cannot reorder or re-key a reader's dictionary on the device.
  We can only search the order it has.

## 5. Options and the recommendation

### 5.1 How the selection grows

| Option | Presses per lookup (buttons) | Who finds the word end | Cost | Verdict |
|---|---|---|---|---|
| **A. Manual range** (upstream #3932) | +2 (anchor, then confirm) plus one Right per extra character | the reader | new selection mode | no |
| **B. Hold to grow** (the idea in `docs/future_work/cjk.md`) | hold Confirm or Right, +1 s per character | the reader | contests configured long presses; a refresh per step | no |
| **C. Longest dictionary match from the cursor** | 0 extra | the dictionary | about one index search (section 5.2) | **decision 2** |
| **D. Segment the whole run up front** (forward maximum matching over the page) | 0 extra, and enables word-wise movement | the dictionary | one search per word on the page (~300+ on a dense page) before the first move | not now |

**Decided: C.** The reader puts the cursor on a word's first character. After the 300 ms debounce the
highlight grows rightwards over the longest dictionary word, and Confirm looks it up.
- A and B ask the reader for the very thing they are looking up: where the word ends.
- A also costs every lookup two presses on boards that only have buttons.
- B fights the long-press bindings and repaints once per character.
- D is the right shape for word-wise cursor movement, but it front-loads hundreds of searches. It is not
  needed to answer "what does this word mean".

**When C applies.** It depends on the selected word's first code point, not on the book's language:
- **Han or kana** (`utf8IsUnspacedCjk`, `Utf8.h:131`): the run is built across words, as below.
- **Hangul:** the run is the token itself. Korean words are already whole (`Utf8.h:119-127`).
- **Anything else** takes today's path, unchanged.

**The run.** Starting at the cursor's word, the screen concatenates the text of the following words:
- in reading order, across a line end;
- up to 12 code points;
- stopping at the first code point that is not unspaced CJK, or that `cleanWord` would strip as
  punctuation (。、「」（）…).

Kept as letters: 々 U+3005, 〆 U+3006 and ー U+30FC, which `EDGE_PUNCTUATION` leaves alone, and kana.
Each code point remembers which word it came from, so a match length maps back to a span of words.

**Touch.** A tap on any character inside the highlighted span looks the span up. A tap elsewhere moves the
cursor, as today.

**Over-long greedy matches** (研究生 in 研究生命, where 研究 was meant) are handled by the definition, which
also lists the shorter matches (section 5.3).

### 5.2 The longest-match search

Two ways to find the longest headword that is a prefix of the run S:

- **C1. Shortest first.** Look up S's first character, then its first two, and so on. After each step, the
  headword the scan stopped on tells whether any longer entry exists, so the walk stops early. This costs
  one full `locate()` per step: up to log2(samples) sample reads, plus a scan of up to 256 entries. For a
  ~120,000-entry dictionary that is 9 sample reads per step, and a 2-character word takes 2-3 steps.
- **C2. One descent to S, then read the window (my call).** This is #2942's idea, extended for CJK.
  1. Search for S itself, as an exact lookup does: one descent, then the forward scan up to the first
     headword above S. While scanning, remember each entry that is a prefix of S (byte compare,
     ASCII-folded, ending on a code point boundary).
  2. If one or more were seen, **the last one seen is the longest match**, and the others are the shorter
     matches for section 5.3.
  3. If none was seen, any match sorts before the window. It must then be a prefix of the longest common
     prefix of S and the window's first headword. That headword is already read: `SampleWindow::low`,
     `Dictionary.cpp:531-532`. Look those up exactly, longest first, usually one or two. The first hit wins.
  4. With a `.syn`, do the same over `.syn` and `.sidx` and take the longer result (ties go to `.idx`). This
     is how a traditional-script book finds a dictionary keyed in simplified, if the dictionary has the
     other script in `.syn`.

  **Why it is correct.** Take a prefix P of S, and any headword t with P < t < S. Then t starts with P. If t
  first differed from P at an earlier byte k, then t[k] > P[k] = S[k], which would put t above S. Two
  things follow:
  - The headwords that are prefixes of S sort by length, so the last one before S is the longest.
  - If the longest prefix lies before the window's first headword w0, then w0 sits between it and S, so the
    prefix is also a prefix of w0.

  The same argument holds on ASCII-folded bytes, so `asciiCaseCmp` order is enough.

  **Cost.** About one exact lookup, plus one or two in step 3. That is what speculation already pays per
  word. The search adds no allocation: a fixed array of three hits (offset, size, length) and their
  headwords, which fit `std::string`'s short buffer at up to five CJK characters.

**Also:**
- The stem, lowercase, elision and hyphen fallbacks of `resolve()` are skipped for a CJK run. They cannot
  match, and each one costs a search.
- Between probes the search checks `mappedInput.hasPendingInput()`. If input is waiting, it gives up and
  leaves the speculation "unknown", as the cover decoder does (`lib/CooperativeAbort`).
- A Latin dictionary on a CJK page costs one search and reports "Missing", as today.
- The serial `CMD:ACT:DICT` (`SerialTestApi.cpp:347-380`) takes the same path when the word is CJK. It
  reports the matched length, the hits, the number of reads and the time, so section 7 can measure without
  the screen.

### 5.3 What the definition shows, and in which font

**Shorter matches.** The definition holds the longest match, then up to two shorter ones from the same
search (≤ 3 entries, longest first, within `MAX_DEFINITION_BYTES`).
- **Plain dictionaries:** each entry is `headword` on its own line, then its text.
- **HTML dictionaries:** each entry is a `<p><b>headword</b></p>` (escaped) followed by its HTML.
- **Buffer:** the index gives every size before reading, so the buffer is sized once and filled in place,
  with no growth.
- **Rejected (decision 3):** a gesture that shrinks the selection one match at a time. It needs a button, and on the
  C3 boards every front button is already used.

**Font (decision 1).** A lookup that started on CJK text passes the reader's effective font id
(`EpubReaderActivity.cpp:2728-2733` already hands it to the word screen) to the definition, instead of
`DICTIONARY_FONT_ID`.
- The font that drew the page has its characters, and a full CJK font also covers traditional forms and,
  normally, pinyin vowels.
- The headword moves into the body's first line, and the header keeps the dictionary name and page counter.
- The prewarm scope already in `render()` loads the glyphs.
- Latin lookups keep Noto Sans and its IPA.

Once roadmap step 6 chains the reader's CJK font behind the UI faces, the headword can return to the
header. Upstream draws every definition in the reading font (section 3.5), so this matches upstream for
CJK.

### 5.4 Dictionary formats and sources

**StarDict only**, through the existing reader and web installer. The installer already unpacks `.idx.gz`
and offers `.dict.dz` → `.dict` (`docs/dictionary.md:37-40`). Plain `.dict` matters more on CJK pages,
where the 32 KB inflate window is hardest to find.

| Dictionary | Languages | Licence | Entries | How to get StarDict |
|---|---|---|---|---|
| CC-CEDICT | Chinese → English (simplified and traditional, pinyin) | CC BY-SA 4.0 | ~120,000 | convert `cedict_ts.u8` with PyGlossary (EDICT2 reader → StarDict) |
| JMdict (JMdict_e) | Japanese → English (kanji and kana keys) | EDRDG licence (CC BY-SA 4.0) | ~200,000 | convert the XML with PyGlossary (JMDict reader → StarDict) |
| CC-KEDICT | Korean → English | CC BY-SA | smaller | PyGlossary has a reader for it |
| Wiktionary-based sets | various, incl. zh/ja/ko | CC BY-SA | varies | already listed in `docs/dictionary.md:87-99`; check which ones carry CJK before naming them |

Entry counts are approximate: check the release used. Verify the PyGlossary format names and options when
writing the user doc. To check per conversion:
- whether the other script (traditional or simplified, kana or kanji) lands in `.idx` or `.syn`;
- whether definitions are HTML (`sametypesequence=h`);
- that tone colouring is off.

**Sizes, estimated, to measure (section 7).**
- **CC-CEDICT `.idx`:** ~120,000 × (~7 bytes of headword + NUL + 8) ≈ 2 MB.
- **JMdict `.idx`:** with kanji and kana keys, several MB.
- **`.dict`:** a few MB for CC-CEDICT and tens of MB for JMdict.
- **`.qidx`:** 4 bytes per 256 entries, ~2 KB for CC-CEDICT.

Card space is not the issue. The first open streams the whole `.idx` once to build the sidecar
(`buildSidecar`, `Dictionary.cpp:352-433`, 4 KB reads), behind the "Indexing" popup, and that time is worth
measuring.

**Not supported:** EPWING, MDict, Yomitan zips, Lingvo DSL and Kindle dictionaries. They are converted on
a computer or not at all.

### 5.5 Japanese and Korean inflection

**Without deinflection.** 食べた has no headword. Step 3 of section 5.2 then finds 食 ("food"), and the
headword on the definition screen shows the reader what matched. Korean verbs (먹었습니다) behave the same.
Korean nouns with particles work, because the particle is a suffix (학교에서 → 학교).

| Option | Cost | Verdict |
|---|---|---|
| **None** | 0 | **decision 4, for this round** |
| **A dictionary whose `.syn` lists inflected forms** | 0 code: the `.syn` pass of section 5.2 finds 食べた as a prefix of the run and resolves it to 食べる | check which JMdict builds do this; document them if any |
| **A rule table** (kana suffix in → suffix out, chained) | a few KB of flash; several exact lookups per Confirm; a test corpus; clean-room, because the deinflectors we know (Yomitan GPL-3.0, KOReader AGPL-3.0) cannot be copied into MIT code | a later design, if readers ask |

**Why not now.** It roughly doubles the work. It needs Japanese grammar we cannot review ourselves, the
same bar the A3 spec set ("not 'looks right to us'"). And the `.syn` route may make it unnecessary. The
search in section 5.2 is the seam: a deinflector would add candidates to step 3.

### 5.6 Styled definitions on dense pages (the heap)

On a dense CJK page, `DHTML`'s entry gate (40 KB free, 20 KB block) fails while the word screen holds its
arrays, so the definition falls back to plain text.

| Option | What it frees or changes | Risk | Verdict |
|---|---|---|---|
| **H1. Release the word arrays while a definition is open**, then rebuild them on return | ~16 KB in two blocks on a 600-token page, plus the 2 KB snapshot | rebuilding costs one measuring pass, and can fail like the first build (handled: the low-memory popup) | **my call** |
| H2. Also release the page copy, and have the reader hand it back | ~8 KB more (estimated from the `TextBlock` arenas) | needs the reader as a party; larger change | only if H1 is not enough |
| H3. Lower or scale the gate for short definitions | depends on the parser's fixed cost, which is not measured apart from the ~21 KB layout cost | an allocation failure mid-layout | no, not without that measurement |
| H4. Accept plain text on dense pages | nothing; CC-CEDICT reads fine as plain text | formatting lost | the fallback if H1 measures short |

**How H1 works.**
- `performLookup` already drops the session buffers before starting the definition (`releaseCaches`,
  `DictionaryWordSelectActivity.cpp:589`). It would also swap away `fragments` and `words` and clear
  `wordsExtracted`.
- On return the full repaint runs `extractWords()` again (`render`, 905). It is deterministic: same page,
  same metrics, same order. It must keep `selected`, the span and the speculation instead of re-centring
  the cursor (`extractWords`, 310-315).
- The definition's `onExit` already clears the font cache (`DictionaryDefinitionActivity.cpp:59-71`), so
  the rebuild meets a heap at least as good as the first build did.
- **Stop rule:** if H1 does not get the gate to pass on the X3 dense page (section 7), take H4 and leave
  the gates alone.

### 5.7 The log noise

`SdCardFont::prewarmStyle` logs `LOG_ERR` "Failed to allocate mini bitmap" when the page's glyph arena does
not fit one block, and returns `PREWARM_ARENA_TOO_LARGE` (`SdCardFont.cpp:1693-1701`). `prewarm()` then
retries with as many glyphs as the largest block holds, halving as needed (`SdCardFont.cpp:1210-1242`). On
the word screen's full repaints this happens every time and succeeds, so the error line is noise. The bug
is real only when the retry ends with nothing resident.

| Option | Verdict |
|---|---|
| **L1. The first miss logs at debug level**; `prewarm()` logs one line after the retry: info with the glyph count kept and dropped, error only when nothing is resident | **my call**; a small, separate PR |
| L2. Check `heapHasBlockFor(totalBitmapSize)` before allocating and go straight to the retry | same effect, one fewer failed malloc; can fold into L1 |
| L3. Avoid the full-page prewarm on the word screen | no: the full repaint must draw the page |
| L4. Wait: the CJK bitmap families leave the font manager (roadmap step 5), and TrueType fonts have no per-page arena | no: installed `.cpfont` families keep working and keep logging |

## 6. What changes where (sketch, for the plan)

- **`Dictionary`:**
  - a prefix search over a run (section 5.2) that returns up to three hits: length, location and headword;
  - shares `LookupSession` and the scan buffers;
  - a reader that fills one caller buffer with several definitions.
- **`DictionaryWordSelectActivity`:**
  - the run builder;
  - speculation and Confirm through the prefix search;
  - a highlight over a span of words: up to 2 rows on the differential path, the full repaint otherwise;
  - a tap inside the span activates it;
  - the arrays released for the definition (H1).
- **`DictionaryDefinitionActivity`:**
  - a font id parameter in place of the `DICTIONARY_FONT_ID` macro at its three uses (`DictionaryDefinitionActivity.cpp:87, 140, 388`);
  - the headword in the body for CJK;
  - several entries.
- **`SdCardFont`:** log levels (L1).
- **`SerialTestApi`:** `ACT:DICT` reports the prefix search.
- **Tests:**
  - a CJK fixture from `test/fixtures/dictionary/make_fixture.py`, synthetic so nothing CC BY-SA is checked
    in, with entries placed so a longest match falls inside, at the edge of and before a sample window, in
    `.idx` and in `.syn`;
  - the correctness argument of section 5.2 as table-driven cases;
  - the existing `DictionaryTest` suite unchanged.
- **Docs:** a CJK section in `docs/dictionary.md` (sources, conversion, put the cursor on a word's first
  character); `docs/future_work/cjk.md` updated.

## 7. What to measure on the devices

Devices: X3 and X4 (C3, buttons), X4 Pro and T5S3 (S3, touch). Books: Hong Lou Meng (the X3 reference for
dense pages), a Japanese public-domain novel, and a Korean text. Tools: `tools/devctl.py`, `STATE` (heap,
largest block), `HEAP` ballast to reproduce low heap, `SCREENSHOT`, and `ACT:DICT` extended as in section
5.2.

| # | What | How | Pass |
|---|---|---|---|
| 1 | File sizes and first-open indexing time, CC-CEDICT and JMdict | the existing "Indexed N entries … in M ms" log | recorded; the "Indexing" popup stays up the whole time |
| 2 | Prefix search cost: reads and ms, speculative and on Confirm | `ACT:DICT` over ~50 words per book | median within 1.5× today's exact lookup on the same dictionary (target to agree) |
| 3 | Responsiveness | hold Right across a line | the cursor never waits on a search |
| 4 | Accuracy | host-side: run the search as forward maximum matching over a chapter and compare with jieba (Chinese) or MeCab (Japanese) on the same text (dev tooling, not shipped) | agreement reported; no target before we see it |
| 5 | Heap at the definition's entry, dense page, before and after H1 | `STATE` plus the `DHTML` log | the styled gate passes on the X3, or we take H4 |
| 6 | Rebuild cost on return (H1) | timestamps around `extractWords` | recorded; no lost cursor |
| 7 | Definition in the reader font | prewarm ms and arena bytes for a CJK definition page, SD `.cpfont` and TrueType | readable Han, kana and pinyin on a screenshot |
| 8 | Span highlight | count full-repaint fallbacks at the default and largest reader sizes | the default size stays differential |
| 9 | Log noise | "Failed to allocate mini bitmap" per word-screen session, before and after L1 | none at error level when the retry succeeds |
| 10 | Size | `firmware_size_history.py`, RAM delta | flash a few KB, no steady-state RAM |
| 11 | Latin regression | host tests; an English lookup on the X4 | unchanged |
| 12 | Touch | tap in span looks up, tap outside moves (X4 Pro, T5S3) | as described |

## 8. Risks

- **Greedy matching is wrong sometimes** (研究生命). The shorter matches in the definition soften it. The
  host comparison (row 4) tells us how often.
- **The cursor must be on the first character.** On the second character of 大学 the search starts at 学.
  The docs have to say so, and the highlight shows it at once.
- **Runs cross paragraph ends** (section 4). CJK prose ends paragraphs on 。！？」, which stop the run. A
  heading with no punctuation can glue to the next paragraph, which can at worst lengthen a match.
- **Dictionaries differ.** Which script is in `.idx` and which in `.syn` is up to the converter (section 5.4).
  The search covers both, but a dictionary with only one script will miss text in the other.
- **Ruby.** Readings flow inline today as 吾輩（わがはい）, and the fullwidth parentheses stop the run. PR B
  stores ruby per line rather than as tokens. The run builder must keep skipping it, which needs a test
  when PR B lands.
- **Upstream divergence.** Upstream went with the manual range (#3932). Ours stays the per-word screen plus
  the prefix search, so future ports of upstream's selection code will not apply cleanly.

## 9. Order of work

1. **L1, the log noise.** Independent of everything else.
2. **H1, the arrays released for the definition.** Measure rows 5 and 6. If the gate still fails, take H4
   and drop H1.
3. **The CJK lookup:** the prefix search, the span highlight, the definition's font and entries, `ACT:DICT`,
   docs and tests. Measure rows 1-4, 7, 8 and 10-12.

Each is its own plan and PR, in `docs/future_work/cjk.md`'s roadmap as step 9's parts.
