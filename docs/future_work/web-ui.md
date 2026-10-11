# The web interface, now that its pages cost no flash

Status: analysed 2026-10-11. Builds on PR #439, which moves the web pages from the firmware image
into a bundle on the SD card. Once that is in, page and script size no longer costs firmware
flash, only bundle size, which nobody pays for while reading.

Sources compared:

| Source | Version |
|---|---|
| crosspoint-reader | `upstream/develop` `aad48b40d` |
| CrossInk | `uxjulia/CrossInk` `main`; `web/pages/files.js` last changed in `1b1156027` |
| Our code | the plugin host, the four bundled plugins and the Font Manager |
| TTF conversion | a prototype of a TTF → `.cpfont` converter running FreeType compiled to WASM |

The items are in the suggested order. Effort figures are estimates unless marked measured.

## 0. Fix first: bugs found during the analysis

**Open.** Small and independent of everything below. All found by reading the code; none was
reproduced on a device.

**File Manager: actions run once per redraw.** `renderFileTable()` (`FilesPage.html`, about line
2453) replaces `fileTable.innerHTML` and then adds its `change` and `click` listeners to the same
`#file-table` element again. After a sort or a refresh, one click on Delete, Move or Rename runs
its handler once for every redraw so far. Fix: add the two delegated listeners once, outside the
render.

**Renaming or moving a book through the web interface loses its state.** A book's cache folder is
named after a hash of its path (`Epub.h`, `epub_<hash>`), and `handleRename` / `handleMove` call
`clearBookCacheIfNeeded()` on the old path. Progress, stats and bookmarks are left behind, and the
book starts over. CrossInk moves the state with the book (`src/util/BookMoveUtils.cpp`, 12.7 KB).
Do the same here: rename the cache folder and re-key the stores that hold the path (progress,
reading stats, bookmarks, recent.json, the library index).

**An upload silently overwrites a file of the same name** (`handleUpload`, "Overwriting existing
file"). Upstream (#2534) refuses the clash and the page adds a suffix. Decide on a policy before
porting the rename-on-upload feature in section 3.

**The plugin relay follows redirects.** `SecureHttpClient` follows up to five by default
(`SecureHttpClient.h`, `_maxRedirects = 5`), and `HttpDownloader` never lowers that. So
`/api/relay` and `/api/fetch` follow them, although the comment on `handleRelay` and
`docs/sd-plugins.md` say they do not. An allowed host can therefore send the device to any host.
Fix: `setMaxRedirects(0)` for these two paths, and return the 3xx to the plugin as documented.

**`/api/plugin-fs` truncates binary data.** It reads `server->arg("plain")`, which the framework
builds with `String(plainBuf)`, cut short at the first NUL byte. The handler still reports success.
Upstream moved this to multipart for this reason. No bundled plugin writes binary data yet.

**The plugin capability check never runs.** `/api/plugins` sends only `name`, `title` and `mount`,
so `missingCapabilities()` in `PluginHost.js.inc` never sees `requires`.

**Stale documentation.**
- The header of `PluginHost.js.inc` says relay, fetch and plugin-fs were dropped.
- `docs/sd-plugins.md` says one plugin works on both firmwares. Upstream's plugin API has since
  changed (`relay(method, url, headers, body)`, base64 `writeFile`), so plugins no longer carry
  across.

## 1. A Plugins page; no plugin screens on the device

**Open.** Plugins work, but nothing tells a user they exist:
- nothing ships them: neither the release nor the bundle carries `plugins/`;
- there is no install path: the docs say "copy a folder";
- with none installed, the pages show nothing at all;
- the device never mentions them.

Upstream made them discoverable on the device: a Home entry, Settings → System → Plugins with a
README viewer, and a catalog that downloads plugins (`PluginCatalogActivity`, 1,201 lines).

**Decision: managing plugins belongs on the web, not on the reader.** It is only ever useful while a
browser is connected, and on the device it would cost flash and screens. So, a dedicated Plugins
page:
- **Installed:** title, description, which page the plugin adds itself to, and which internet
  hosts it may reach (`allowedHosts`). Each plugin has Remove and Enable/Disable.
- **Add from a file:** a `.zip` unpacked in the browser with JSZip, which is already in the bundle.
- **Add from the catalog:** a `plugins.json` in this repository listing the curated plugins, as
  `assets/sd-fonts/fonts.json` does for fonts, with an Install button for each.
- **Empty state:** say what plugins are and how to add one.

**Firmware needed:**
- `/api/plugins` also reports `description`, `requires`, `allowedHosts`, `version`, the root the
  plugin was found in, and whether it is disabled.
- Install and remove endpoints for `/.crosspoint/plugins`. The web file endpoints refuse dot
  folders, and installing there keeps plugins out of the Library.
- A `disabled` marker file per plugin, skipped by `PluginLocations::scanPlugins`. A per-browser
  setting would not follow the plugin to another browser.
- Catalog install has two options: the device fetches the plugin zip itself (the relay allowlist
  must then cover the catalog host), or the bundled plugins travel in the web bundle and are
  copied out of it.

**From upstream's plugin API v2, take only:**
- relay with a method and headers, keeping our per-plugin host allowlist;
- `api.dir`;
- multipart `plugin-fs`, which fixes the bug in section 0;
- the form CSS for plugin cards.

Skip `api.crypto`, book keys / DRM and the job queue: they are out of scope for a reader.

**Bundled plugins:**
- **metadata-editor** becomes built in (section 2).
- **organize-by-author** and **find-duplicates** stay optional catalog entries.
  organize-by-author has known bugs: it misses upper-case sidecars, ignores a failed sidecar move,
  and takes the first `dc:creator` whatever its role.
- **hello-plugin** stays a developer example.

## 2. The metadata and cover editor, built in

**Open.** The metadata-editor plugin (9 KB gzip) handles EPUB, XTC, TXT and Markdown, and writes
only a `<book>.opf` sidecar and a `<book>.jpg/png` cover sidecar; it never changes the book.
Covers come from a file, the clipboard, or a search (Open Library directly; Google Books and
Goodreads through the relay).

**Built in:** a **Details** action on each book row of the File Manager, opening a dialog for that
book. Its code lives in a separate bundle asset (`js/metadata-editor.js`) loaded on first use, so
`/files` stays the size it is.

**Firmware needed:**
1. **TXT and Markdown edits must reach Home and the reader.** `recent.json` is refreshed from the
   sidecar for EPUB only (`RecentBooksStore`). The TXT/MD reader stores the file name and shows
   the book's own title. Refresh all formats through `BookDetailsLookup`, and give TXT/MD a
   sidecar-aware title.
2. **A new cover must reach the thumbnails.** They are rebuilt only when the sidecar's mtime is
   newer, which a board without a clock may not see, and `clearBookCacheIfNeeded()` ignores
   `.opf` and image uploads. Delete a book's thumbnails when its cover sidecar is uploaded.
3. **`GET /api/book/meta?path=`**, so the dialog does not download a whole EPUB to read its OPF.
   It needs a "without the sidecar" mode, because `loadForMetadata` always applies the sidecar.
4. **A built-in relay allowlist** for the cover search, or a first version with Open Library and
   file/paste only, which needs no relay.

**Plugin bugs to fix on the way:**
- It looks only for a lower-case `.opf`; the firmware also accepts `.OPF`.
- `book.epub` and `book.txt` in one folder share a single sidecar.
- Seeding from an EPUB copies the full description, which can push the sidecar past the
  firmware's 16 KB cap, and the firmware then ignores the whole sidecar.

## 3. EPUB optimizer: port the browser-only additions

**Open.** The File Manager already has upstream's optimizer: baseline JPEG, resizing to the panel,
an image picker. CrossInk and upstream have since added more, all in the browser, all MIT:

| Addition | From | Size | Value here |
|---|---|---|---|
| **Split long chapters** at natural breaks (8,000 words / 32 KB preferred, 48 KB hard), rewriting the spine and anchors | CrossInk `splitLongXhtmlSections` | ~460 lines | High. Long chapters are what strain the heap when a chapter is laid out; this removes the problem at upload. |
| Collapse empty chapter stubs | CrossInk | ~60 | Medium |
| Refuse DRM-encrypted books; allow font obfuscation | CrossInk `assertEpubHasNoContentEncryption` | ~30 | Clear error instead of a broken book |
| Strip XHTML comments; scrub invisible code points | CrossInk; upstream #3028 | ~165 | Complements the zero-width fix |
| Strip embedded fonts | upstream #3085 | ~140 | Smaller books |
| Auto-crop images | upstream #2139 | ~290 | Medium |
| Keep the cover in colour, resized only | CrossInk `preserveColor` | ~25 | Low |
| Rename upload to "Title - Author"; remember upload options | upstream #2534 / CrossInk | ~350 | Medium; needs the overwrite policy (section 0) |

**A caveat:** an optimized book is a different file, so KOReader's document hash, the partial MD5
of the file, differs from an unmodified copy on another device. That is as true of today's
optimizer.

**Do not port:**
- **Inky's tools.** Inky is CrossInk's companion app; it is GPL-3.0, and its Python pipeline
  derives from an unlicensed repository.
- **CrossInk's stable page numbers** (`META-INF/x-locations.json`): a format of its own, while
  we already honour the publisher page list.
- **CrossInk's pre-dithered `.pxc2` images**: a different format from our `.pxc`.
- **CrossInk's status-bar slot editor.**

**Attribution:** a file-header provenance line pointing to
`uxjulia/CrossInk web/pages/files.js @1b1156027`, and a Co-authored-by trailer for Julia Nguyen.

## 4. Convert a TTF/OTF to `.cpfont` in the browser

**Open.** Today a font is converted offline, by `lib/EpdFont/scripts/fontconvert_sdcard.py` (1,118
lines: FreeType with forced auto-hinting at 150 DPI, 2-bit coverage, GPOS/legacy kerning,
GSUB ligatures). The Font Manager points users at upstream's hosted builder, which by every sign
produces unhinted output, the stem defect of #149.

**Approach: FreeType compiled to WebAssembly, plus a JS port of the converter, in a Web Worker on
the Fonts page.**

**Measured with a prototype on Literata 14, all four styles:**
- With FreeType **2.13.3**, 99.6 % of glyphs (3,596 of 3,612) are byte-identical to the Python
  output; only four arrows differ.
- With 2.14.3, 92 % are: the auto-hinter changed for accented letters. **Pin FreeType to the same
  version on both sides.**
- A whole family (4 styles × 5 sizes, 17,620 glyphs) rasterises in 139 ms on a desktop.

**The other ways were rejected:**

| Approach | Why not |
|---|---|
| Pyodide running the Python script | freetype-py is not a Pyodide package; the runtime is about 13 MB; minutes per family |
| On-device with the SDK's FreeInkFont | FreeType alone is about 157 KB of flash, against 263 KB of headroom, and it would also cost font-cache map pages; slow on a C3 |
| Upstream's hosted builder | Unhinted output; a page hosted off the device cannot post to it over http |
| opentype.js for kerning | Cannot read GPOS extension lookups (type 9), so 10 of the 25 hosted source families would lose their kerning |

**Cost:**
- The FreeType `.wasm` adds about 344 KB (zopfli-gzip) to the web bundle. It has to be in the
  bundle, because hotspot mode has no internet.
- No firmware change: the result goes to the existing `/api/fonts/upload`, which checks only the
  magic bytes.
- `build_html.py` must learn the `.wasm` asset, or the page instantiates it from an ArrayBuffer.

**First version:**
- One file per style: regular required, bold, italic and bold-italic optional.
- Sizes 10–18, the hosted interval set, auto-hinting always on.
- Kerning from the legacy table and GPOS formats 1 and 2, unwrapping extension lookups.
- `liga`/`rlig` ligatures.
- Variable fonts refused with an explanation.

Effort: about 600–800 lines of JS plus the UI, 4–6 days, including a Node test in CI that compares
the JS output byte for byte with the Python script. The prototype's comparison script is a
starting point.

**Risks:**
- The two FreeType versions drifting apart.
- Kerning details: Python sums values across subtables, and drops kerning above 255 classes.
- Large CJK fonts in phone memory.
- FreeType's licence needs a credit line on the page.

## 5. Other candidates

| Candidate | What it would do | Needs firmware? |
|---|---|---|
| **Dictionary installer** | Upload a StarDict archive (zip, tar.gz, tar.bz2), unpack it in the browser, decompress `.idx.gz` → `.idx` (required) and `.dict.dz` → `.dict`, which `docs/dictionary.md` recommends for heap reasons. Reject `idxoffsetbits=64`. Write to `/dictionaries/<name>/`; list and remove installed ones. | No; bz2 needs a small JS library |
| **Diagnostics page** | Show the boot/sleep event ring (`BootDiagRing`, already on the card) and system info, with one "download a report" button for bug reports without a serial cable | A read endpoint for the ring |
| **Storage and caches** | Show what `/.crosspoint` holds (book caches, thumbnails, library index); remove caches of books no longer on the card; clear all | A scan/clean endpoint |
| **Sleep-screen images** | Crop and scale a picture to the panel, dither it to the panel's grey levels, and save a BMP to `/sleep`. Opening a large JPEG on the device is slow (an X4 cover open measured 14.9 s). | No |
| **Settings backup** | Download and restore the non-credential configuration | Read access to `/.crosspoint` files |

**Not recommended:**
- **Upstream's `.ttf` upload rendered on the device:** it needs the on-device TrueType engine,
  which is S3 and PSRAM only.
- **A web view of the Library index:** large, and it duplicates the device.

## Suggested order

| Step | Content | Effort |
|---|---|---|
| 1 | Section 0 | ~1 day; the rename/move state item is the largest |
| 2 | Plugins page and endpoints (section 1) together with the built-in metadata editor (section 2): the same endpoints, and the editor is the first catalog-free plugin to graduate | |
| 3 | Optimizer ports (section 3), starting with chapter splitting | JS only, ~2–3 days |
| 4 | TTF converter (section 4) | 4–6 days |
| 5 | Dictionary installer, then diagnostics (section 5) | |
