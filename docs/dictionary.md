# Dictionary

*Ported from [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader), where the feature and this document originate: PR #2583 by Uri Tauber, with contributions from William Floyd, Thiago Kenji Okada, Justin Mitchell, Ryan Hitchman, Kurtis Grant, kemonine and DustinHu. Adapted here for this device — see the notes on background work, fonts and dictionary switching below.*

Look up words while reading an EPUB using an offline StarDict dictionary stored on the SD card.

## Supported Format

The reader supports **StarDict** dictionaries. When searching for dictionaries online, look for "StarDict format" or files with `.dict`, `.idx`, and `.ifo` extensions.

A dictionary folder must contain:

- `.idx` — word index (required, **must be uncompressed** — a `.idx.gz` will not work; decompress it on your computer with `gzip -d` first)
- `.dict` or `.dict.dz` — definition data (`.dict.dz` is supported as-is; entries are decompressed on the fly during lookup)
- `.syn` — synonym index (optional; maps alternate spellings and irregular forms to their headword)
- `.ifo` — metadata (optional)

Not supported: dictionaries with 64-bit index offsets (`idxoffsetbits=64` in the `.ifo` — rare, and rejected with an error).


## Setting Up a Dictionary

1. Copy your dictionary folder(s) to `/dictionaries/` on the SD card — one dictionary per folder, e.g. `/dictionaries/webster/webster.idx` + `webster.dict.dz`. A hidden `/.dictionaries/` folder (dot-prefixed) works the same way, for keeping it out of the file browser.
2. Open **Settings → Reader → Dictionary** on the device.
3. Select a dictionary from the list, or **None** to disable lookups.

Keep the folder name to 31 characters or fewer: the reader remembers the chosen dictionary by its folder name and keeps no more than that, so a longer one cannot be selected.

Folders containing more than one dictionary (multiple `.idx` stems) are skipped as ambiguous, as are folders holding an `.idx` with no matching `.dict`/`.dict.dz` next to it -- a dictionary that would only fail once you tried to use it.

If you can, decompress `.dict.dz` to `.dict` on your computer (`dictunzip yourdict.dict.dz`). A `.dict.dz` lookup needs a 32 KB contiguous inflate window, and that is the one allocation on this path a fragmented reading heap cannot always supply; a plain `.dict` needs none of it and is read directly.

## Install from the Web Interface

Start **File Transfer** on the device, open its address in a browser and go to **Dictionaries** (`http://<device-ip>/dictionary`). The page does the preparation above for you:

- Choose the archive you downloaded (`.zip`, `.tar.gz`, `.tgz`, `.tar.bz2` or `.tar`) or the dictionary's own files (`.ifo`, `.idx` or `.idx.gz`, `.dict` or `.dict.dz`, optional `.syn`), or drop them on the page. Other archive formats (`.tar.xz`, `.7z`, `.rar`) are refused: repack them as `.zip`.
- Your browser unpacks `.idx.gz` and `.syn.gz`, and offers to unpack `.dict.dz` to `.dict` (on by default, showing the size it will have). A dictionary with `idxoffsetbits=64`, or without an index or data file, is refused with the reason.
- Each dictionary goes into its own folder under `/dictionaries`, named after the title in its `.ifo` (or the archive), shortened to 31 characters — you can change the name before installing. An archive holding several dictionaries is split into one folder each. A folder that already exists is replaced only after you confirm.
- The list of installed dictionaries shows each one's title, word count, files and whether the reader can use it, and **Remove** deletes a dictionary's folder.

Then choose the dictionary on the device as usual (**Settings → Reader → Dictionary**). Unpacking happens in the browser, so a large dictionary needs a current Chrome, Edge, Firefox or Safari and some patience while it uploads; a `.tar.bz2` is decoded more slowly than the other formats.

## Looking Up a Word

Two ways to start a lookup while reading:

- Open the reader menu (**Confirm**) and choose **Dictionary**.
- Or map any button press to the **Dictionary** action under **Settings → Controls**.

Choosing it with no dictionary selected opens the picker rather than reporting the problem, so the setup above can be done from the book.

While a lookup is open the reader stops all of its background work — the look-ahead section build and the next-page pre-render — and hands back the memory they were holding: the 48 KB region the look-ahead borrows, the build arena inside it, and the pre-rendered next page. A lookup needs a large contiguous block at the point in a reading session where the heap is at its most fragmented, and nothing else should be competing for it.

No completed work is thrown away. A look-ahead build that was still running is aborted, but it keeps whatever it had already extracted, so the retry after the lookup skips that step; a build that had finished is left alone, because its result is the cache file on the card rather than anything held in RAM. Background work picks up where it left off once you leave the dictionary.

One word on the page becomes highlighted:

1. Use **Left/Right** to move between words in reading order, and the side **Up/Down** buttons to jump between lines. Moving the highlight repaints only the two word boxes that changed, so stepping through a line does not re-render the page. On a page set vertically (**Vertical Text**) the keys follow the columns: **Up/Down** move between words in reading order, down the column and on into the next one, **Left** moves to the next column (the one to the left) and **Right** to the previous one. The button hints name the direction each button moves the highlight on screen.
2. Press **Confirm** to look up the highlighted word.
3. Press **Back** to return to the reader.

On the very first lookup with a dictionary (and again whenever the `.idx` or `.syn` source file changes), the reader shows *"Indexing dictionary…"* while it builds small sidecar files next to them — a `.qidx` for the word index, and a `.sidx` when a `.syn` synonym file is present. Each sidecar is rebuilt independently, only when its own source changes. This takes a few seconds for large dictionaries and makes all subsequent lookups fast. The sidecars can be deleted safely at any time — they will simply be rebuilt.

### How Lookup Works

1. **Direct match** — the word is found as-is (case-insensitive) in the dictionary index. Surrounding punctuation is ignored, including guillemets (`«mot»`), `¿` and `¡`, and CJK and fullwidth punctuation. Where a dictionary holds both a common word and a name spelled alike (`husk` and `Husk`), the lowercase entry wins; a word that only exists capitalized (`Paris`) is still found. A word capitalized with a letter outside plain ASCII (`Über`, `STRAẞE`, `Привет`, `Γεια`) that has no entry as written is retried in lowercase, in Latin, Greek and Cyrillic scripts.
2. **Synonyms** — on a miss, if the dictionary ships a `.syn` file, alternate spellings and irregular forms recorded there are resolved to their headword (e.g. `oxen` → `ox`, `colour` → `color`). This step is skipped if the `.sidx` sidecar could not be built (e.g. transient low memory during indexing); the dictionary otherwise stays usable, and the build is retried the next time it is opened.
3. **Stemming** — still no match: common English word forms are retried automatically: possessives and plurals (`dogs` → `dog`, `stories` → `story`) and verb endings (`walked` → `walk`, `running` → `run`, `making` → `make`). A French elision with no entry of its own is looked up by the word after its apostrophe (`l’histoire` → `histoire`, `qu’après` → `après`); one the dictionary carries (`d'accord`) is found as it is.
4. **Not found** — a short popup appears and you return to word selection.

### Chinese, Japanese and Korean

Chinese and Japanese are written without spaces, so the word screen steps through them a character at a time (a punctuation mark rides along with the character before it). A lookup that starts on a Han or kana character therefore looks for the longest word the dictionary has at that point rather than the character alone:

1. Put the cursor on the **first character** of the word. On the second character of 大学 the search starts at 学.
2. After a moment the highlight grows over the longest dictionary word that starts there, with no extra press: on 大 in 大学生活, over 大学生. Moving on before then costs nothing; the search gives way to the button.
3. **Confirm** shows every entry the dictionary has for that word (CC-CEDICT's 行 three times: xíng, háng and héng), then up to two shorter words that start there too (大学, 大), as far as the 64 KB definition limit allows. Each entry stands under its own word, in your reading font. On a touch board a tap inside the highlight looks it up as well.

Where a word starts and ends:
- The search reads up to 12 characters from the cursor, across words and line ends, and stops at punctuation (。、「」（）…) or at anything that is not Han or kana. 々, 〆 and the long vowel mark ー count as letters, so コーヒー stays whole.
- Korean is written with spaces, so a lookup on Hangul takes the word under the cursor and finds the longest entry the word starts with: 학교에서 finds 학교, without the particle.
- A ruby reading (furigana) is not part of the text: on 吾輩 with わがはい above it, the search reads 吾輩 and on, and the highlight covers the base characters.
- A word lists at most eight of its entries.

What it does not do:
- **No deinflection.** 食べた has no entry of its own and finds 食; 먹었습니다 finds nothing in CC-KEDICT. Each entry is headed by the word it is for, so you can see what was matched.
- **The other script only where the dictionary has it.** The search covers the dictionary's word index and its `.syn`, and none of the Latin fallbacks above (no stemming, no lowercase retry). CC-CEDICT lists every traditional form as a synonym of its simplified headword, so 學校 finds 学校; a dictionary without them finds nothing for text in the other script.
- **One headword per synonym.** A synonym that several headwords share shows one of them only; which one is not defined. JMdict files a kana reading as a synonym of each kanji spelling, so はし, a reading of 橋, 箸, 端, 嘴 and four more, showed 嘴.
- **Characters your reading font lacks.** CC-CEDICT writes each word in both scripts (大学生 / 大學生). A font made for one region (Simplified Chinese, Japanese) lacks some characters of the other and draws them as a box; a font that carries both draws them all. The NotoSansCJK `.cpfont` family carries both scripts but not pinyin's ē, ǎ and ū, which drop out of the reading (dàxuéshēng shows as dàxuéshng); a TrueType family has them.
- **Greedy matches.** The longest word is not always the one the sentence means: 研究生命 starts with 研究生 (graduate student), not 研究 (research). The shorter matches follow it in the definition.

**Where to get them.** None of these ship as StarDict; [PyGlossary](https://github.com/ilius/pyglossary) converts them on a computer. Convert to a plain `.dict` (`dictzip=False`): it needs no 32 KB inflate window, which is hardest to find on a dense CJK page.

| Dictionary | Language | Licence | Source |
|---|---|---|---|
| CC-CEDICT | Chinese → English; simplified headwords, traditional forms as synonyms, pinyin | CC BY-SA 4.0 | `cedict_1_0_ts_utf-8_mdbg.txt.gz` from [MDBG](https://www.mdbg.net/chinese/dictionary?page=cc-cedict) |
| JMdict (JMdict_e) | Japanese → English; kanji headwords (kana for words written in kana), kana readings as synonyms | EDRDG licence (CC BY-SA 4.0) | `JMdict_e.gz` from the [EDRDG](http://ftp.edrdg.org/pub/Nihongo/) |
| CC-KEDICT | Korean → English | CC BY-SA 3.0 | `kedict.yml` from [mhagiwara/cc-kedict](https://github.com/mhagiwara/cc-kedict) |

```
pip install pyglossary lxml pyyaml      # without lxml the EDICT2 and JMDict readers are skipped; pyyaml is for CC-KEDICT
gzip -d cedict_1_0_ts_utf-8_mdbg.txt.gz JMdict_e.gz
pyglossary cedict_1_0_ts_utf-8_mdbg.txt cedict/cedict.ifo --read-format=EDICT2 --write-format=Stardict --read-options="colorize_tones=False" --write-options="dictzip=False"
pyglossary JMdict_e jmdict/jmdict.ifo --read-format=JMDict --write-format=Stardict --write-options="dictzip=False"
pyglossary kedict.yml kedict/kedict.ifo --read-format=cc-kedict --write-format=Stardict --write-options="dictzip=False"
```

Run the `pyglossary` command (`python -m pyglossary` does not start the converter). The conversions keep the entries of one word apart (CC-CEDICT's 行 is three entries, JMdict's 上 four), and write HTML definitions (`sametypesequence=h`); `colorize_tones=False` keeps CC-CEDICT's pinyin uncoloured. Then install each folder as above. Measured on the X4 (October 2026 releases):

| Dictionary | Words | Synonyms | `.idx` | `.syn` | `.dict` | First open ("Indexing dictionary…") |
|---|---|---|---|---|---|---|
| CC-CEDICT | 125,331 | 77,970 | 2.1 MB | 1.1 MB | 56.5 MB | 1.5 s (`.idx` 0.97 s, `.syn` 0.49 s) |
| JMdict_e | 218,975 | 550,503 | 4.7 MB | 15.8 MB | 62.0 MB | 9.2 s (`.idx` 2.1 s, `.syn` 7.1 s) |
| CC-KEDICT | 13,669 | none | 0.2 MB | none | 1.8 MB | 0.1 s |

## The Definition Screen

When a word is found, the definition screen shows the matched headword at the top and the definition text below, with a page counter for long definitions. A lookup on Chinese, Japanese or Korean text puts each entry's word in the body instead, above its text; the header keeps the dictionary name and the page counter.

HTML dictionaries that declare `sametypesequence=h` use the EPUB text-layout engine for semantic formatting such as headings, bold, italics, lists, and line breaks. Images and CSS styling are ignored. Definitions that are too large or cannot be laid out within the available memory fall back to plain text.

- **Left/Right** or side **Up/Down** — previous / next page
- **Power** — next or previous page too, when its Short Press is set to page forward or back (Settings → Controls → Button Actions → Power), as in the book
- **Confirm** — look the same word up in the next installed dictionary, cycling through them. Only offered when more than one dictionary is installed; the current one is named to the right of the headword. The choice is remembered, so later lookups use it too.
- **Back** — return to word selection

Definitions of Latin-script words render in Noto Sans at a fixed size rather than in your reading font. That is the one font carrying the phonetic alphabet — a dictionary's pronunciation notation needs about 100 codepoints that no reading font here ships, and putting them in every font and size would cost roughly five times the flash. Any character still missing falls back to the closest available shape (schwa as `e`, the primary-stress mark as an apostrophe) rather than an empty box.

A lookup that started on Chinese, Japanese or Korean text renders its definition in your reading font instead: Noto Sans has no Han, kana or Hangul, and the CJK font the book is read in has them (pinyin's tone marks too, in a TrueType family; see "Characters your reading font lacks" above).

## Where to find dictionaries

> credit to https://github.com/koreader/koreader/wiki/Dictionary-support for the list.

- The [reader.dict](https://www.reader-dict.com) (ex "BoboTiG/ebook-reader-dict") project provides StarDict version of daily dumps of [Wiktionary](https://www.wiktionary.org/) monolingual dictionaries for a variety of languages. It also provides [non-free multilingual](https://www.reader-dict.com) dictionaries.
- The [WikDict](https://www.wikdict.com) project provides free bilingual dictionaries based on [Wiktionary](https://www.wiktionary.org/) for a lot of language pairs. StarDict versions can be [downloaded from here](https://download.wikdict.com/dictionaries/stardict/).
- The [`Vuizur/Wiktionary-Dictionaries`](https://github.com/Vuizur/Wiktionary-Dictionaries) repository contains dictionaries based on [Wiktionary](https://www.wiktionary.org/) from many languages to English, including English-English.
- The [DictInfo](https://www.dictinfo.com/) website provides outdated monolingual dictionaries based on [Wiktionary](https://www.wiktionary.org/).
- The [Firedict site](https://tuxor1337.frama.io/firedict/dictionaries.html) contains a list of freely available dictionaries.
- [wiktionary_stardict](https://xxyzz.github.io/wiktionary_stardict/): update monthly.
- [Fictionaries](https://fictionary.gumroad.com/) provides dictionaries for various speculative fiction books and series.
- [World Factbooks Archive](https://github.com/MilkMp/CIA-World-Factbooks-Archive-1990-2025) provides 36 years of CIA's World Factbook dictionaries in StarDict format.
- [StarDict-Hebrew](https://github.com/Uri-Tauber/StarDict-Hebrew) Hebrew-English StarDict versions of Babylon dictionaries.
- Chinese, Japanese and Korean: CC-CEDICT, JMdict and CC-KEDICT, converted with PyGlossary as [Chinese, Japanese and Korean](#chinese-japanese-and-korean) describes.
