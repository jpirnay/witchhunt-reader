# SD-card web plugins

A plugin is a folder on the SD card holding a `plugin.js`. The web server lists
it, the Settings or File Manager page loads it, and the script renders into a
card on that page. Adding or changing a plugin never needs a firmware build.

> **Provenance.** This system is ported from
> [crosspoint-reader#2734](https://github.com/crosspoint-reader/crosspoint-reader/pull/2734)
> ("feat: Add browser-side plugin system with SD card support") by Justin
> Mitchell ([@itsthisjustin](https://github.com/itsthisjustin)). The folder
> layout, the `/api/plugins` and `/plugin` contract, the `registerPlugin`
> handshake and the `mount` convention are his design, and are kept compatible
> so a plugin written for either firmware works on both. This port deliberately
> carries none of that branch's device-capability endpoints, on-device catalog
> screens or content-protection work — see [Trust](#trust) for why.

Plugins extend the **web interface**, not the reader. Nothing runs on the
device: the firmware only enumerates the folders and serves their bytes, and the
script executes in the browser of whoever opened the page. Cost on the device is
one directory scan per page load and no resident RAM.

## Layout

```
<root>/<name>/
    plugin.js       the plugin (required - a folder without one is not listed)
    manifest.json   optional: { "title": "...", "mount": "settings" | "files" }
    ...assets       optional, served flat via api.pluginFile()
```

`<root>` is any of `/.crosspoint/plugins`, `/plugins`, or `/.plugins`. All three
are scanned; on a name collision the earliest in that order wins. The two
dot-free roots exist because a dot-prefixed folder is awkward to create in some
desktop file managers.

**Clear Cache does not remove plugins**, including from `/.crosspoint/plugins`.
That action is an allowlist: it deletes only the `epub_`/`xtc_`/`txt_` per-book
cache directories and leaves everything else in `/.crosspoint` alone. Note
though that `/plugins` is a normal visible folder, so unlike the two dot-prefixed
roots it can be deleted through the web file manager like any other.

Folders are flat. The server rejects any `name` or `file` containing `/`, `\`,
or `..`, so a plugin cannot ship subdirectories or read outside its own folder.

## Writing one

`plugin.js` calls `registerPlugin` at top level with a render function:

```js
CrossPoint.registerPlugin((container, api) => {
  container.innerHTML = '<h2>' + api.title + '</h2><button id="go">Scan</button>';
  container.querySelector('#go').onclick = async () => {
    const files = await (await fetch('/api/files?path=/Books')).json();
    // ...inspect, then POST to /move, /mkdir, /delete as needed
  };
});
```

The plugin is handed an empty `<div class="card">` and owns all of it, heading
included — the firmware renders no chrome of its own, so a plugin that draws no
heading shows an untitled card.

`api` carries three things:

| Field | Description |
|---|---|
| `name` | the folder name |
| `title` | `manifest.json`'s title, falling back to `name` — use it if you want the heading to track the manifest rather than hardcoding it |
| `pluginFile(filename)` | URL for another file in this plugin's folder |

Everything else is the web server's own
same-origin API — a plugin uses `fetch()` against the endpoints in
[webserver-endpoints.md](webserver-endpoints.md) exactly as the pages do:
`/api/files`, `/mkdir`, `/move`, `/rename`, `/delete`, `/upload`, `/download`.
The File Manager page also has JSZip already loaded, so a plugin mounted there
can open and rewrite an EPUB in the browser.

That set is deliberate. There is **no** device endpoint for outbound HTTP,
crypto, or arbitrary file writes: a plugin can reach the SD card through the
same doors the web UI already opens, and it cannot make the reader talk to the
internet. Anything needing a remote service must be done by the browser
directly, subject to normal CORS rules.

### Mount points

| `mount` | Page | Suits |
|---|---|---|
| `settings` (default) | Settings | configuration, one-off maintenance actions |
| `files` | File Manager | anything operating on the library; JSZip available |

### Conventions

**Declare nothing at top level.** Plugins are `<script>` tags in the shared page,
so a top-level `function escapeHtml()` collides with the page and with every
other plugin, last one wins. Keep all state and helpers inside the
`registerPlugin` closure, as the bundled plugins do.

**Request one thing at a time.** The device serves a single connection, so
concurrent `fetch()` calls only queue - and a burst can stall a response long
enough to trip the firmware watchdog. `await` in a loop.

**Move a book's sidecars with it.** The firmware resolves a cover as `book.jpg`
(or `.jpeg`/`.png`/`.bmp`) beside `book.epub` and prefers it over the embedded
cover, so a plugin that relocates a book and leaves those behind silently breaks
its cover. `book.opf` travels with it for the same reason - see below.

### The `book.opf` metadata sidecar

Calibre writes an OPF beside each exported book. Where `book.opf` sits next to
`book.epub`, treat it as authoritative for that book's metadata: it is a few KB
against a multi-megabyte download, so reading it instead of the book is the
difference between a library organising instantly and one grinding through
every file. `organize-by-author` prefers it and falls back to the book's own
OPF, reporting which it used per book.

Prefer `opf:file-as` over the element text when deriving a folder name - that is
the sortable form, and what Calibre's own author folders use.

This mirrors the existing cover-sidecar rule, and the same priority applies:
sidecar wins over what is embedded in the book. Note the firmware itself does
not read `book.opf` today; this is a convention between plugins for now.

## Bundled plugins

`plugins/` in this repository. Copy a folder onto the card to install it; they
are never compiled, so they cost no flash.

| Plugin | Mount | What it does |
|---|---|---|
| `hello-plugin` | settings | Minimal reference and smoke test - renders a card and lists `/` |
| `find-duplicates` | files | Reports files sharing an exact size. Report only: never writes, and never downloads a book |
| `organize-by-author` | files | Sorts loose EPUBs into `<Author>/` folders. Preview first, moves only on Apply, carries sidecars along |

## Loading order and failures

Plugins load **last** on a page, strictly one at a time. The device serves one
HTTP connection at a time, so a plugin fetching in parallel with the page's own
loaders can stall a response long enough to trip the firmware watchdog — which
is also why a plugin should not fire concurrent requests of its own.

A plugin that fails to load, or that never calls `registerPlugin`, renders an
error in its own card and does not affect the rest of the page or the other
plugins.

## Trust

A plugin is unsandboxed JavaScript running with the page's full authority: it
can read, rewrite, and delete anything on the SD card the web UI can reach.
Installing one is equivalent to running a script on your library — only use
plugins you would trust with the card. This is inherent to the design, not a
gap to be closed: the plugin is code you chose to copy onto your own card.

Note also that the web server has no authentication of its own, so it should
only be enabled on networks you trust — that is true with or without plugins.

## Limits

- `manifest.json` is read with a 4KB cap; a larger file is ignored (the plugin
  still loads, falling back to its folder name and the `settings` mount).
- An unparseable manifest is logged (`[WEB]`) and ignored the same way.
- A plugin whose listing entry would exceed 384 bytes of JSON is skipped —
  in practice this means keeping `title` short.
- At most 64 directories are examined per root. The scan runs inside one
  request and each candidate costs two SD stats, so an unbounded walk over a
  folder holding hundreds of subdirectories could outlast the task watchdog.
  Hitting the bound logs to serial (`[PLG]`) and ignores the remainder — which
  is why a plugins root should be a dedicated folder, not a general-purpose one.
- `/api/plugins` and `/plugin` return 503 with `Retry-After` when the heap is
  too fragmented to serve them, like the other allocating endpoints.
- The scan reports the folders it finds; there is no install or update
  mechanism. Copy a folder onto the card and reload the page.

## Debugging

1. Copy the folder to `/.crosspoint/plugins/<name>/` on the card.
2. Open Settings (or the File Manager) and check the card appears.
3. `GET /api/plugins` shows exactly what the firmware discovered — if a plugin
   is missing from that list, the folder has no `plugin.js` or a same-named
   folder in an earlier root shadowed it.
4. Script errors surface in the browser console, not on the device. Watch
   serial (`[WEB]`) only for manifest and file-serving problems.
