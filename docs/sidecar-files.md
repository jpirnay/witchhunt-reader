# Sidecar files

A sidecar is a file sitting beside a book, sharing its name but with a different
extension. The firmware prefers it over the equivalent data embedded in the book,
so you can correct a cover or a title without rewriting the EPUB.

```
/Books/
    Some Book.epub      the book
    Some Book.jpg       cover sidecar     (optional)
    Some Book.opf       metadata sidecar  (optional)
```

The rule is the same for both: **a sidecar wins over what is inside the book.**

## Cover sidecar — `book.jpg`

Recognised extensions, in this order: `.jpg`, `.jpeg`, `.png`, `.bmp` (and their
uppercase forms). The first one found wins.

When present it replaces the book's embedded cover on the home screen and in the
book browser, and the firmware skips extracting the embedded cover entirely.
Implemented by `ReaderActivity::sidecarCoverPath()`.

The bundled `metadata-editor` plugin manages this from the web UI — preview the
current cover, replace it from a local file, paste an image copied from any
website, search Open Library / Goodreads / Google Books, or remove it. See
[sd-plugins.md](sd-plugins.md).

**Replacing a cover takes effect on the next load.** The rendered thumbnail is
cached per book, and `ReaderActivity::ensureCoverThumb()` compares the sidecar's
modification time against it: a newer sidecar wins and the thumbnail is
regenerated. Note this depends on the clock — files written while the RTC is
unsynced all carry 1980-01-01, so on hardware without a working RTC a replaced
cover may need a cache clear.

## Metadata sidecar — `book.opf`

A Calibre-style OPF. Calibre writes one beside every book it exports, so a
library exported from Calibre already has these and needs no extra work.

Fields taken from the sidecar, when non-empty:

| Field | Source in the OPF |
|---|---|
| title | `<dc:title>` |
| author | `<dc:creator>` |
| language | `<dc:language>` |
| series | `<meta name="calibre:series">` or `belongs-to-collection` |
| series index | `<meta name="calibre:series_index">` or `group-position` |
| description | `<dc:description>` |

The bundled `metadata-editor` plugin writes these from the web UI, so you do not
have to hand-edit XML — see [sd-plugins.md](sd-plugins.md). It edits an existing
sidecar in place rather than regenerating it, so publisher, identifiers, dates
and other fields it does not manage survive untouched.

A minimal sidecar:

```xml
<?xml version='1.0' encoding='utf-8'?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="uuid_id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>The Title As You Want It Shown</dc:title>
    <dc:creator opf:file-as="Lastname, First" opf:role="aut">First Lastname</dc:creator>
    <meta name="calibre:series" content="Series Name"/>
    <meta name="calibre:series_index" content="4"/>
  </metadata>
</package>
```

### Rules

- **Empty fields are ignored.** A sidecar supplying only an author leaves the
  book's own title alone, so a partial sidecar cannot blank out good metadata.
- **Cover references are not taken from it.** `coverItemHref` points inside the
  ZIP, which a sidecar cannot meaningfully supply — use a cover sidecar instead.
- **Only metadata.** The spine, manifest and reading order always come from the
  book itself. A sidecar cannot change what you read, only how it is labelled.
- **A malformed or oversized sidecar is ignored**, and the book opens normally
  with its embedded metadata. The cap is 16KB
  (`Epub::MAX_METADATA_SIDECAR_BYTES`); a real Calibre OPF is a couple of KB.

### When changes take effect

Immediately — on the next time the book's metadata is loaded.

The sidecar is deliberately **not** baked into the `book.bin` cache. That cache
is only rebuilt when the EPUB's own bytes change, so a baked value would leave an
edited sidecar silently ineffective. Instead the overlay is applied on every
load, including the cached path (`Epub::applyMetadataSidecar()`). Cost when no
sidecar exists is a single file-existence check.

Deleting the sidecar restores the book's embedded metadata, again on next load.

## Moving books

A sidecar is tied to its book by filename, so **moving or renaming a book must
carry its sidecars along** — otherwise the cover silently reverts to the embedded
one and metadata corrections are lost.

Who does this today:

- **Move to `/COMPLETED`** (the optional finished-book action) moves the book
  and every sidecar, resolving name collisions for each
  (`moveSidecarFilesToCompleted`).
- **The `organize-by-author` plugin** moves sidecars with the book — see
  [sd-plugins.md](sd-plugins.md).
- **Manual moves** through the web File Manager or a script are your own
  responsibility: move `book.epub`, `book.jpg` and `book.opf` together.

Adding another sidecar type means adding it to **one** place:
`lib/FsHelpers/SidecarFiles.h`. Cover resolution, metadata resolution and the
move-with-the-book path all read those tables — `ReaderActivity::sidecarCoverPath`
and `Epub::metadataSidecarPath` are one-line delegates that add only their own
logging.

This used to be three independent copies, which is exactly how `.opf` came to be
readable by the reader but left behind when a finished book moved. The copies had
also drifted: the move path derived its base name without checking for a path
separator, so a book with no extension inside a dotted folder (`/My.Books/untitled`)
took the dot from the folder.

Deleting a book does **not** currently delete its sidecars; they are left behind
as harmless orphans.
