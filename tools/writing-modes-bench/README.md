# Writing-modes timing books

The real-prose books behind the build-time line of writing modes step 2: a cold chapter in a shaped script should
build in at most 2x its Latin twin's time on the X3. Each script's book has a Latin twin with the same paragraphs'
word counts. **Never commit the books**; regenerate them from Wikisource with these scripts.

| Script | Book | Measured (X3, chapter 2, median of three cold builds) |
|---|---|---|
| Hindi | Premchand's *Gaban* (1931), Hindi Wikisource | 6,908 against 2,648 ms (2.61x; PR 3, #642) |
| Arabic | Jurji Zaydan's travel letters, the England chapter, Arabic Wikisource | 7,880 against 2,489 ms (3.17x; #656); master without shaping 5,586 ms (2.22x) |

## Fetching

`ws_export.py` downloads a Wikisource page as EPUB 3 through ws-export and prints its sha256.
- The downloads are untrusted data. Put each one in its own new directory, and run every script with `python -I`.
- The page must be passed as `page=`; ws-export returns its HTML form for `title=`.
- Space the calls about 15 s apart, because the MediaWiki API rate-limits.

```
python -I ws_export.py ar "رحلة جرجي زيدان إلى أوربا/ثانياً: إنكلترا" <dl> england
python -I ws_export.py hi "प्रेमचंद रचनावली (खण्ड ५)/गबन/एक" <dl> gaban1   # and so on, one subpage per chapter
```

ws-export of the *Gaban* index does not follow its chapters, so fetch each subpage (एक, दो, तीन, ... आठ).

## Building

Both scripts import the repo's `test/epubs/make_test_shaped_indic.py`. The word limit cuts the text at the first
paragraph past it.

```
AR_WORDS=2500 python -I make_zaydan_books.py <repo> <dl> <out> england.epub
GABAN_WORDS=2500 python -I make_gaban_books.py <repo> <dl> <out> gaban1.epub gaban2.epub ...
```

## Measuring on a device

- **Setup:**
  - put the books in `/test/` and the font family in `/.crosspoint/fonts/<Family>/` **before** flashing, because
    families are discovered at boot;
  - NotoNaskhArabic or NotoSansDevanagari, 14 pt, portrait.
- **A cold build:** wipe the book's `/.crosspoint/epub_<murmur2 of its path>/spines/*/*.bin`, open at `@2:1`, and read
  `createSectionFile spine=1 done: total=` from the log.
  - Take one uncounted warm-up, then three runs of each book, alternating.
  - The same log block gives the `word cache:` and `arena:` lines.
- **Background-B's heap floor:** open the book at `@1:1` and wait. B builds chapter 2 in its borrowed buffer and logs
  `Background-B: returned secondary buffer (... lowest)`.
- See `docs/serial-test-api.md` and `tools/devctl.py` for the commands.
