# SD-card web plugins

A plugin is a folder on the SD card holding a `plugin.js`. The web server lists
it, the Settings or File Manager page loads it, and the script renders into a
card on that page. Adding or changing a plugin never needs a firmware build.

> **Provenance.** This system is ported from
> [crosspoint-reader#2734](https://github.com/crosspoint-reader/crosspoint-reader/pull/2734)
> ("feat: Add browser-side plugin system with SD card support") by Justin
> Mitchell ([@itsthisjustin](https://github.com/itsthisjustin)). The folder
> layout, the `/api/plugins` and `/plugin` contract, the `registerPlugin`
> handshake and the `mount` convention are his design and are kept compatible: a
> plugin that only uses the page and these two endpoints runs on both firmwares.
> The device-side `api` calls follow upstream's later contract too (its plugin
> system, #3114, and the PRs after it): `api.dir`,
> `relay(method, url, headers, body)` answering `{status, headers, body}`, a
> base64 `writeFile`, and `fetchToSd(url, dest, headers, {method, body})`. Unlike
> upstream, the device reaches only the hosts a plugin may reach: those its
> manifest lists, or that you approved when it asked. Upstream's crypto endpoint,
> job queue, on-device catalog screens and content-protection work are not here —
> see [Trust](#trust) and [Reaching the internet](#reaching-the-internet-allowedhosts-and-approvals)
> below.

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

`api` carries:

| Field | Description |
|---|---|
| `name` | the folder name |
| `title` | `manifest.json`'s title, falling back to `name` — use it if you want the heading to track the manifest rather than hardcoding it |
| `dir` | the plugin's own folder, e.g. `/.crosspoint/plugins/<name>`. A plugin may keep its settings and token there: `writeFile` writes inside it, and `/download?path=<dir>/<file>` reads a file back, though the folder is hidden from every listing |
| `pluginFile(filename)` | URL for another file in this plugin's folder |
| `relay(url)` | GET a URL the browser cannot reach itself; resolves to a `Response`, redirects followed within the hosts the plugin may reach. See below |
| `relay(method, url, headers, body)` | upstream's form: any of GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS and the WebDAV methods; `headers` an object; resolves to `{status, headers: [[name, value], ...], body}` with the body as text. A 3xx comes back unfollowed, with its `Location`. Headers make the request a credentialed one: the server's certificate must verify |
| `fetchToSd(url, dest, headers, {method, body})` | download straight to the card, one transfer instead of two; `headers`, `method` and `body` optional. Resolves to `{ok, complete, status, bytes, dest}` |
| `writeFile(path, data)` | write one small file (≤64KB) where `/upload` may write or in the plugin's own folder; `data` is base64 when a string (upstream's API), or a Blob or ArrayBuffer |
| `registerAction(name, fn)` | accepted and ignored with a console warning: there is no job queue here |
| `currentPath` | the folder the File Manager is showing. Default to it rather than asking for a path the user has already navigated to; always `/` on Settings |

**`api.crypto` is not provided.** The browser's own `crypto.subtle` covers
hashing, HMAC, AES and RSA, so a device-side implementation would only duplicate
it. A plugin written against the upstream API that calls `api.crypto` gets a
readable error rather than `is not a function`.

A plugin may declare what it needs, and one asking for something this firmware
does not implement is refused with a visible message instead of failing somewhere
inside its own code:

```jsonc
{ "requires": ["relay", "fetchToSd"] }
```

Supported: `relay`, `fetchToSd`, `writeFile`, `pluginFile`, `dir`.

Everything else is the web server's own
same-origin API — a plugin uses `fetch()` against the endpoints in
[webserver-endpoints.md](webserver-endpoints.md) exactly as the pages do:
`/api/files`, `/mkdir`, `/move`, `/rename`, `/delete`, `/upload`, `/download`.
The File Manager page also has JSZip already loaded, so a plugin mounted there
can open and rewrite an EPUB in the browser.

Prefer these same-origin endpoints wherever they suffice — they cost the device
nothing beyond the request itself. A plugin reaches the SD card through the same
doors the web UI already opens, so it gains no privilege the page does not have.

Reaching *outward* is different, and is fenced separately: see
[below](#reaching-the-internet-allowedhosts-and-approvals). Where a remote site sends CORS headers, call it directly with `fetch()`
and leave the device out of it entirely.

### Reaching the internet: `allowedHosts` and approvals

A plugin runs in a browser page served by the device, so it can only read a
cross-origin response when the remote site sends CORS headers. Most do not —
Goodreads and `books.google.com` among them — and an image without CORS taints a
canvas, so its bytes can be displayed but never saved.

The relay (`/api/relay`) fetches on the page's behalf and answers same-origin.
The device reaches a host for a plugin only when the plugin's own
`manifest.json` lists it, so its reach is readable before you install it, or
when you approved that host for that plugin:

- a refused request names the host (`403 {"error": ..., "host": ...}`), and the
  plugin host asks you "Allow the plugin … to connect to …?"; a yes is stored on
  the device (`/.crosspoint/plugin-hosts.json`, per plugin, kept across updates)
  and the request runs again;
- the Plugins page lists each plugin's approved hosts with a **Take back** button;
- removing a plugin removes its approvals.

Plugins written for upstream crosspoint-reader declare no `allowedHosts`, so
this is how they reach their services.

The relay and `/api/fetch` check the server's certificate against the reader's
roots. A request without headers goes on unverified when that fails, as an OPDS
catalog without a login does, so such a response is no more trustworthy than
plain http: treat it as untrusted input, and never put a token or other secret
in a URL. A request with headers (an `Authorization` header, a cookie) is a
credentialed one and fails instead; a self-hosted server with a self-signed
certificate needs Settings' "Skip HTTPS validation".

```jsonc
{
  "title": "Cover Finder",
  "mount": "files",
  "allowedHosts": [
    "openlibrary.org",        // exact match
    ".openlibrary.org"        // any subdomain, e.g. covers.openlibrary.org
  ]
}
```

No list and no approval means no access. `relay(url)` is a GET, and follows a
redirect only to a host the plugin may reach too; one to any other host fails
the request, so add the hosts a service redirects to (a CDN, say) to the list.
The four-argument form follows no redirect at all.
The body streams back rather than being buffered, so a large image does not have
to fit in the device's largest free block.

Prefer a direct `fetch()` when the site sends CORS (Open Library does) and keep
the relay for sites that do not — it costs the device a network round trip.

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
sidecar wins over what is embedded in the book. **The firmware reads it too** -
the title, author and series shown on the device come from `book.opf` when one
is present, so a plugin writing one changes what the reader displays. See
[sidecar-files.md](sidecar-files.md) for the device-side rules.

## Installing and managing plugins: the Plugins page

`/plugins` in the web interface (linked from every page) lists every installed
plugin with its description, the page it adds itself to and the hosts it may
reach (`allowedHosts`), and offers **Switch off** / **Switch on** and **Remove**.
Plugin management lives here and nowhere on the reader: it is only ever useful
while a browser is connected.

It installs in two ways, both done by the browser; the reader only stores what
it is sent:

- **From the catalog**: `plugins/catalog.json` in this repository, read with the
  plugins' files straight from `raw.githubusercontent.com` (which allows
  cross-origin reads). It needs a browser with internet, so not hotspot mode.
  `?ref=<branch>` on the page reads another branch's catalog, to try plugins
  before they are merged. Regenerate the catalog with
  `scripts/build_plugin_catalog.py` after changing a plugin; CI fails when it is
  out of date. A manifest with `"catalog": false` stays out of it.

  Entries have the shape of crosspoint-reader's plugin catalog: `name`, `title`,
  `description`, `author`, `version`, `base` (the folder the files are read
  from) and `files` (their names), plus this firmware's `mount`, `requires` and
  `allowedHosts`. `base` is resolved against the catalog's own URL. The script
  gives this repository's plugins a relative one (`find-duplicates/`), so a
  catalog read from another branch installs that branch's files. A plugin hosted
  elsewhere is added to `catalog.json` by hand with an absolute `base`; the
  script keeps such entries, and the page shows where they come from. The
  crosspoint-reader catalog itself is not offered: most of its plugins need
  on-device screens (`device.json`) or a relay this firmware does not have.
- **From a `.zip`**: the plugin's files side by side, or inside one folder. The
  page shows the manifest's title, description and `allowedHosts` and asks
  before installing.

Installs go to `/.crosspoint/plugins/<name>`, through endpoints of their own
(`/api/plugins/begin`, `/file`, `/commit`; see
[webserver-endpoints.md](webserver-endpoints.md)): files are staged in
`<name>.new` and switched in with one rename, so a plugin cut off half-way never
runs. A folder copied by hand into `/plugins` or `/.plugins` still works and is
listed and managed the same way.

**Switch off** puts a `disabled` file in the plugin's folder: the pages skip the
plugin, and the setting holds for every browser. It only stops the pages loading
it: `/plugin`, `/api/relay` and `/api/fetch` still answer for it. That is no
loss of protection, since any script in a page already has the same reach.

Manifest fields the page shows: `title`, `version`, `description`, `mount`,
`allowedHosts` (see [Layout](#layout) for the rest).

## Bundled plugins

`plugins/` in this repository, offered by the Plugins page's catalog. They are
never compiled, so they cost no flash.

| Plugin | Mount | What it does |
|---|---|---|
| `hello-plugin` | settings | Minimal reference and smoke test - renders a card and lists `/` |
| `find-duplicates` | files | Reports files sharing an exact size. Report only: never writes, and never downloads a book |
| `organize-by-author` | files | Sorts loose EPUBs into `<Author>/` folders. Preview first, moves only on Apply, carries sidecars along |

The metadata editor that used to be a plugin here is now part of the web
interface: the File Manager's **Details** button on a book
(`src/network/html/js/metadata-editor.js`). A copy installed as a plugin still
loads; remove it on the Plugins page.

### Cover sources, and why they differ

The Details dialog is the worked example of the CORS rules above. It is not a
plugin, so its relay requests use the scope `:details`, whose host list is built
into the firmware (`www.goodreads.com`, `.gr-assets.com`, `books.google.com`); a
`:` cannot appear in a folder name, so no plugin can claim it.

| Source | Search | Cover bytes |
|---|---|---|
| Open Library | direct — it sends CORS | direct |
| Google Books | direct — the API sends CORS | **relay** — images are on `books.google.com`, which does not |
| Goodreads | **relay** — no API since 2020, so the search page is scraped | **relay** — images are on `i.gr-assets.com` |

Result thumbnails are plain `<img>` tags pointing at the remote host, because
*displaying* a cross-origin image was never restricted — only reading its pixels
is. The relay is used for one thing: reading the bytes of the cover you actually
pick. The device does work for one image, not for the whole grid.

Two caveats. The Goodreads path scrapes HTML and will break if they change their
markup — it looks for `img.bookCover` and strips the `._SX50_` size segment to
get the full-size image. And Google's covers are small (~128px), so Open Library
or Goodreads give a better result on the device.

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
That includes a plugin's own folder: its token and settings can be read with
`/download` by anyone on the network while File Transfer runs, as on upstream.

The JSON endpoints a plugin drives (`POST /api/relay`, `/api/fetch` in its
JSON form, `/api/plugins/hosts`) take only an `application/json` body. A
browser then asks before sending a cross-site request, and the device answers no
such question, so another website you visit cannot drive them through your
browser.

## Limits

- `manifest.json` is read with a 4KB cap; a larger file is ignored (the plugin
  still loads, falling back to its folder name and the `settings` mount).
- An unparseable manifest is logged (`[WEB]`) and ignored the same way.
- A plugin whose listing entry would exceed 1536 bytes of JSON is skipped —
  in practice this means keeping `description` to a sentence or two.
- At most 64 directories are examined per root. The scan runs inside one
  request and each candidate costs two SD stats, so an unbounded walk over a
  folder holding hundreds of subdirectories could outlast the task watchdog.
  Hitting the bound logs to serial (`[PLG]`) and ignores the remainder — which
  is why a plugins root should be a dedicated folder, not a general-purpose one.
- `/api/plugins` and `/plugin` return 503 with `Retry-After` when the heap is
  too fragmented to serve them, like the other allocating endpoints.
- Install, update and removal go through the Plugins page (above); copying a
  folder onto the card works too.

## Debugging

1. Copy the folder to `/.crosspoint/plugins/<name>/` on the card.
2. Open Settings (or the File Manager) and check the card appears.
3. `GET /api/plugins` shows exactly what the firmware discovered — if a plugin
   is missing from that list, the folder has no `plugin.js` or a same-named
   folder in an earlier root shadowed it.
4. Script errors surface in the browser console, not on the device. Watch
   serial (`[WEB]`) only for manifest and file-serving problems.
