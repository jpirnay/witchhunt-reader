# TXT on the EPUB pipeline

Status: decided 2026-11-06, not started. Code references are to master at `17e2ba942`, by symbol.

## Decisions

The maintainer's, 2026-11-06:
- TXT moves onto the EPUB pipeline (`Epub`, `Section`, `EpubReaderActivity`). `TxtReaderActivity` goes.
- Firmware has no codepage tables. Files in other encodings are converted to UTF-8 in the browser, by a tool beside the
  EPUB optimizer.

Everything else below is a proposal, checked against the code but not yet agreed.

## Why

A Chinese novel is often a `.txt`, and that is where our CJK work stops. `TxtReaderActivity` has none of it:
- no kinsoku, no punctuation widths, no first-line indent, and `JUSTIFIED` falls back to left-aligned;
- no chapters (`TXT files have no headings/chapters`), and the setting for line spacing is ignored;
- no language, so `ReaderActivity` calls `clearReaderLanguage()`: Chinese text in a Latin font draws boxes;
- UTF-8 only;
- the whole file is indexed on open (`buildPageIndex`), and `parseAndWrapLines` measures a line backwards, one character
  at a time, with a temporary `substr` each step.

**The wrap's cost,** from a simulation of that loop at 28 characters a line, on a 1M-character novel with no ASCII spaces.
These are counts, not device times:

| Paragraph length | Glyph lookups | Temporary strings |
|---|---|---|
| 120 characters | 109 M | 1.7 M |
| 300 characters | 609 M | 4.9 M |
| 600 characters | 2.3 G | 10.3 M |

The same work repeats for the page on every turn. A line longer than the 8 KB chunk (about 2,700 CJK characters) is cut
there, possibly mid-character.

## Reference code

Port by hand: the histories are unrelated. Both are MIT. Credit with Co-authored-by and a provenance line in the file
header; no AI trailers.
- **Upstream #3704** (`9c103c87`, Phạm Bình An with Uri Tauber): TXT and Markdown through `Epub`, `TxtToHtml`,
  `Txt::buildTxtCache`. 23 files, +611/−890. One spine for the whole file, which we cannot copy (see [Spines](#spines)).
- **crossmux** at `0ab0af23` (0x1abin): `TxtChapterIndex.cpp` for headings, `TxtCache.cpp` and `TxtProgress.cpp` for
  chapters and old progress.
- **Do not port:** crossmux's `TxtEncoding` (its GBK table, 47,880 B) and `TxtParagraph.h`, dead there since #3704, used
  only by its own test.

## Pipeline

### First open: one scan

`Epub::load` runs a scan in `Txt` where a ZIP book parses its OPF: one sequential pass, in fixed chunks, no per-line
allocation. It does five things at once:
1. checks the UTF-8 and counts bad sequences;
2. finds headings and chooses the spine boundaries;
3. counts lines and blank lines, for the paragraph shape ([below](#html-for-a-spine));
4. counts scripts over the first 64 KB, for the language;
5. writes `book.bin` through the usual calls: `beginWrite`, `beginContentOpfPass`, `createSpineEntry`,
   `endContentOpfPass`, `beginTocPass`, `createTocEntry`, `endWrite`, `buildBookBin`.

No HTML is written to the card: a spine is turned into HTML as it is read.

### Spines

**A spine is a byte range of the `.txt`.** A long file cannot be one spine: there is no partial section build, so opening
it would lay out the whole novel first, and `Page::MAX_PAGES_PER_SECTION` stops at 10,000 pages.
- **Boundaries:** at each heading, and at a line end once a spine reaches a size cap. 64 KB (about 21,000 CJK characters,
  60-70 pages) is a first guess, to tune on a device. The optimizer's own limit is 32 KB.
- **The range lives in the href,** `txt/<start>-<end>.html`, with the same string in the TOC, so the existing href pass
  matches TOC to spine and `book.bin` keeps its format. `SpineEntry`'s `zip*` fields stay 0. `getSpineItemStat` reads an
  offset of 0 as "not found", so a TXT must branch before it.
- **Many spines are fine:** King's Avatar has 1,732, and `spines/<n / 32>/` was built for that. `tocIndex` is `int16_t`.
- **No headings at all:** size-cut spines and one TOC entry, the title. crossmux notes that an empty TOC breaks chapter
  stepping (`gotoChapterIndex`, `clamp` on an empty range); check ours.

**Headings** are a small table, from `TxtChapterIndex.cpp`: `第` + number + unit, and `Chapter`/`Book`/`Part` + number.
Theirs needs extending and guarding:
- **Units it lacks:** `節` (it has only `节`), `話`, `话`, `集`. Also Korean `제N장`/`제N화`, full-width digits, and unnumbered
  headings such as `序章`, `楔子`, `后记`, `番外`, `Prologue`.
- **False positives:** it matches on the prefix alone, so a short line starting `第一回合` or `第二部分` is a chapter. Limit
  the length to about 30 characters and refuse a line that ends in sentence punctuation.
- **A contents list at the top of the file:** many files open with every heading in a row. A heading followed by another
  within a few hundred bytes is not a boundary.

### HTML for a spine

`<p>` per paragraph and `<h2>` for a heading, not crossmux's `<br />` per line: a block made by `<br>` takes no first-line
indent (`fromBrElement`). Escape `&`, `<`, `>`.

**The bytes,** tested through `epub_pipeline_dump` with a chapter holding each:

| In the text | Result |
|---|---|
| GBK bytes in a file read as UTF-8 | opens, garbled, boxes |
| CP1252 accented letters | opens, a box for each letter, as TXT does today |
| C0 controls | opens |
| U+FFFE, U+FFFF | opens |
| **NUL** | **section build fails** (`SPINE 0 ERROR build failed`) |

`SaxParser` is yxml, and `yxml_isChar` accepts every byte: invalid UTF-8 is not a parse error, and `utf8NextCodepoint`
draws it as U+FFFD. So the writer must drop NUL, and should turn other C0 controls into a space (keeping tab and
newlines), skip a BOM, and write U+FFFD for a bad sequence, so layout only ever sees valid text.

**Paragraphs.** TXT files come in two shapes, chosen per file from the scan's counts:
- **A line is a paragraph,** the usual Chinese form.
- **Hard-wrapped,** blank lines between paragraphs and lines of nearly equal length, the usual Gutenberg form. Lines join
  until a blank line. Nothing needs joining by hand: write the newline inside the `<p>`, and the segment-break rule (#497)
  makes it a space in Latin text and nothing between CJK characters.

The rule that picks the shape needs real files to tune. A wrong "hard-wrapped" runs poems together, so when in doubt keep
lines.

**Leading whitespace,** U+3000 included, is stripped at a paragraph start in CJK text, as the indent comes from the profile.
What Latin text keeps is open: upstream turns leading spaces into NBSP, which keeps ASCII layouts and makes every indented
paragraph look typed.

**Indent.** A `<p>` with no CSS takes `ParsedText::applyParagraphIndent`'s fallback: one em, and only with Extra Paragraph
Spacing off, which is not the default. So a Chinese TXT would get no indent until `TypographyProfile` carries a default:
2 em for Chinese, 1 em for Japanese and Korean. crossmux gives every CJK language 2 em and does not look for a literal
U+3000; ours should, for EPUBs: U+3000 opens 96% of the paragraphs in *Bei Jing Gu Shi* and 53% in the Hayakawa book.

### Language and font

The scan writes a language into `book.bin`, and `TypographyProfile::forLanguage` and `setReaderLanguage` work unchanged:
kana in the sample gives `ja`, mostly Hangul `ko`, mostly Han `zh`, else none. For Chinese, counting characters unique to
each script (这们说个 against 這們說個) gives `zh-Hans` or `zh-Hant`.

`ReaderActivity`'s `hasEpubExtension` check before the load then covers `.txt`, and the regional font switch follows.

**Font settings.** TXT has its own: `txtSdFontFamilyName`, `txtFontPointSize`, `getTxtReaderFontId`, and an `isTxtMd`
branch in `ensureSdFontLoadedForPath` that says TXT has no per-book override. On the pipeline TXT takes the reader's font
and per-book overrides. The `txt*` settings stay for Markdown. A custom TXT size is lost: decide whether to carry it over.

### `Epub` and `BookMetadataCache`

| Where | TXT |
|---|---|
| `load`, `loadForMetadata`, `loadForCover` | the scan instead of the OPF and TOC parsers; `book.bin` as usual |
| `clearCacheIfContentChanged`, `computeZipFingerprint` | file size and `getModifyDateTime` instead of the central directory |
| `readSpineItemToStream`, `getSpineItemInflatedSize` | the byte range through the HTML writer; the range's length |
| `buildBookBin` | no ZIP walk: sizes from the hrefs (upstream's `isZip`) |
| `discoverCssFilesFromZip`, `parseCssFiles`, `loadImageManifest` | nothing to do |
| `readItemContentsToStream`, `getItemSize`, `extractItemToFile`, `readItemHeaderBytes`, `getStoredItemRange` | no items: false |
| `ensureCoverImageCached`, `generateCoverBmp`, `generateThumbBmp*`, `beginThumbSession`, `openStoredCoverInPlace` | a companion image, as `Txt::findCoverImage`, else today's placeholder |
| `applyMetadataSidecar` | unchanged: sidecar titles keep working |

**The cache folder stays.** Have `Epub::cachePathFor` keep `/txt_<hash>` for `.txt`: the hash is the same, no folder moves,
and every `Txt(path, ...).getCachePath()` call keeps working. `Txt` stays for Markdown in any case.

### The reader

`.txt` goes through `onGoToEpubReader`, and `TxtReaderActivity` is deleted. `LineReaderActivity` stays for Markdown,
then with one subclass. Markdown stays out: our reader has its own tables and code frames, which the EPUB path would lose.

**Sites that tell the formats apart,** each to review:
- `ReaderActivity`: `isTxtFile`, `onGoToTxtReader`, `loadTxt`, and about eight `hasEpubExtension` checks;
- `SleepActivity`: the overlay calls `TxtReaderActivity::drawCurrentPageToBuffer`; the progress and cover branches;
- `FinishedBookActivity`, `BookProgressPresentation`, `BookDetails`, `HomeActivity`, `LyraCarouselTheme`, `RecentBooksStore`;
- `FileBrowserActivity` (it writes `progress.bin`), `BookRelocation`, `CrossPointWebServer`, `WebDAVHandler`;
- `main.cpp`: the `isTxtMd` branch.

**To hide:** the Sync tab (`MenuTab::Sync`), as KOSync positions name items in a ZIP.

**Comes free:** kinsoku, punctuation, the grid, justification, line spacing, hyphenation, the dictionary, pre-render,
background builds, the chapter menu, per-book overrides.

### Reading state

Today's `progress.bin` is 6 bytes, page and byte offset, beside `index.bin` with every page's offset. A leftover `index.bin`
marks an old cache. On the first open after the move:
- **Progress:** take the offset, find the spine whose range holds it, open at that fraction of the spine, delete `index.bin`.
  Landing within a page of the old place is enough, once. Check that the old file cannot be read as the EPUB format first.
- **Starred pages:** stored as spine 0 plus a page number, for `BookmarkStore` and `GLOBAL_BOOKMARKS`. Their offsets come
  from `index.bin` the same way, but a new page number needs the spine laid out. Converting when that spine is first built
  costs code; dropping them costs the reader's bookmarks. Decide.

### A file that is not UTF-8

Files copied over USB never meet the web page, so the device must say what to do. One string, `STR_TXT_NOT_UTF8`, along the
lines of "This file is not UTF-8. Convert it on the Files page of the web interface."
- **Show it** for a UTF-16 BOM, for many NULs (UTF-16 with no BOM), or when bad sequences pass a share of the sample, 5% as
  a first guess. A GBK file is unreadable anyway.
- **Otherwise open it,** with boxes for the bad bytes. A CP1252 file does that today, so it is no regression; a 64-byte table
  for 0x80-0x9F plus Latin-1 would read it properly, and is optional.

## Converter

Runs in the browser, so firmware pays nothing. `TextDecoder` holds every legacy table, and web pages sit in the SD bundle
(`docs/design/web-assets-on-sd.md`), out of flash.

| Tables in firmware instead | Raw | Compressed whole |
|---|---|---|
| GBK | 47,880 B | 16,408 B |
| Big5 | 39,564 B | 17,259 B |
| Shift-JIS (cp932) | 22,560 B | 11,056 B |
| EUC-KR (cp949) | 47,880 B | 13,190 B |
| All four | 157,884 B | 57,913 B |

Against 277,952 B free in the `default` app slot on 2026-11-06. Compressed means delta plus zlib over a whole table;
random access would need chunks and cost more. If a device-only path is ever wanted, the tables can be an optional download
to the card, still no flash.

### Where it sits

A new `src/network/html/js/txt-converter.js`, picked up by `build_html.py`'s walk, served at `/js/txt-converter.js`, loaded on
first use as `metadata-editor.js` is.
- **On upload** (`uploadFile`): read the first 64 KB of a `.txt`. If it is not UTF-8, offer the conversion before sending.
- **On the card:** the Optimize flow already downloads, transforms and puts back, keeping a `.bak` (`optimizeSelected`,
  `/download`, `/api/book/replace`). `optimizeCandidates` and `bookReplaceTarget` take `.epub` only and need `.txt`.
  `handleBookReplace`'s `dropLayoutCache` builds an `Epub`, so this comes after the pipeline.
- **`.md`** can use it too at no cost.

Offsets change with the encoding, so a converted file starts over. That loses nothing: such a file was unreadable before.

### Detection

A strict decode is not enough. Shift-JIS and EUC-KR text decodes cleanly as `gb18030`, as valid but rare characters, and
GBK passes as EUC-KR. Ranking by common characters separates them. Node 22, the core of it:

```js
const COMMON = new Set([...'的一是不了在人有我他这个们中来上大为和国地到以说时要就出会可也你对生能而子那得于着下自之年过发后作里用道行所然家种事成方多经么去法学如都同现当没动面起看定天分还进好小部其些主样理心她本前开但因只从想实日月水火山手口長時國來為個們這說過樣對學開關見書車馬門問間無與發經當後從會還進東西南北今思言語食読話聞']);
const HANGUL = new Set([...'이다는의에을를가고은한하지로서게도과나기그있어아리것으사라대자시들해인만수내면니더말보던']);
const CANDIDATES = ['gb18030', 'big5', 'shift_jis', 'euc-jp', 'euc-kr'];

function rate(text) {           // the share of non-ASCII characters that read as a language
  let all = 0, good = 0;
  for (const ch of text) {
    const c = ch.codePointAt(0);
    if (c < 0x80) continue;
    all++;
    if (COMMON.has(ch) || HANGUL.has(ch) || (c >= 0x3040 && c <= 0x30FF) || (c >= 0x3000 && c <= 0x303F) ||
        (c >= 0xFF01 && c <= 0xFF5E) || (c >= 0x2018 && c <= 0x201D) || c === 0x2014 || c === 0x2026) good++;
  }
  return all ? good / all : 0;
}

function detect(bytes) {
  if (bytes[0] === 0xFF && bytes[1] === 0xFE) return 'utf-16le';
  if (bytes[0] === 0xFE && bytes[1] === 0xFF) return 'utf-16be';
  try { new TextDecoder('utf-8', {fatal: true}).decode(bytes, {stream: true}); return 'utf-8'; } catch {}
  let best = null, bestRate = 0;
  for (const label of CANDIDATES) {
    try {
      const r = rate(new TextDecoder(label, {fatal: true}).decode(bytes, {stream: true}));
      if (r > bestRate) { best = label; bestRate = r; }
    } catch {}
  }
  return best;                  // null: no candidate decodes, so offer the list
}
```

`{stream: true}` keeps a character cut at the end of the sample from counting as an error.

**Result:** 36 of 36, on 18 files at 4 KB and 64 KB: 5 texts from our test books in the encodings each can be written in.
Treat it as a smoke test: the Hangul list was tuned after a first version with all syllables scored 33 of 36, GBK taken for
EUC-KR three times, and no real-world legacy file was tried. EUC-JP against `gb18030` stays close, 0.82 to 0.73.

**So the user decides.** The dialog shows the suggested encoding, a list to change it, and a preview of the first lines that
redraws on a change. Wrong text is obvious to a reader of the language and the preview costs nothing, which also covers what
cannot be detected: `windows-1252`, `windows-1251`, `koi8-r` belong in the list, with `iso-2022-jp`, and `windows-1252` is the
suggestion when few bytes are high. Convert the file in chunks with `{stream: true}`, count U+FFFD, warn when it is more
than a few, and write UTF-8 with no BOM. `gb18030` covers GB2312 and GBK; `euc-kr` covers cp949.

Node 22 accepted all twelve labels. The Encoding Standard requires them of browsers, but none was tried.

## Order of work

Each is its own plan and PR.
1. The scan and the HTML writer in `lib/Txt`, with host tests, nothing routed to them.
2. The indent default in `TypographyProfile`, with U+3000 detection. Useful to EPUBs alone, and independent of the rest.
3. The move: the `Epub` branches, the reader, the state migration, the device message, `TxtReaderActivity` deleted.
4. The converter.

## Tests

**Host.**
- The scan: boundaries never mid-character, every guard on headings, contents lists, a file with no headings, a file of one
  long line, empty and BOM-only files.
- The writer: escaping, NUL, controls, bad sequences, both paragraph shapes, no output ever failing a build.
- TXT fixtures through `epub_pipeline_dump` goldens and the CJK rule checker (`CjkRulesTest`).
- The migration of an old cache.
- The converter beside `test/web/epub_optimizer_test.mjs`, from fixtures in each encoding, with real files once there are some.

**Device.** On the X3 and an X4:
- a 10 MB Chinese TXT: time and heap of the first open, the size cap's effect on a chapter open;
- a turn inside a long paragraph, against today's;
- the sleep overlay, old progress and bookmarks on a card that used the old reader;
- a GBK file copied by USB: the message, then the conversion on the card.

**Browsers.** Desktop Chrome and Firefox, Chrome on Android, Safari on iOS: the labels, a large file on a phone.

## Open

- Whether a custom TXT font size carries over.
- Bookmarks: convert or drop.
- Leading spaces in Latin text.
- The size cap, the shape rule, the 5% message threshold: all first guesses, to set on real files.
- Is there a test file set? All detection so far used text re-encoded from our EPUBs. Real GBK, Big5 and Shift-JIS novels from
  the wild would test both the converter and the heading table.
