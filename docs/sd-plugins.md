# SD-card web plugins

A plugin is a folder on the SD card holding a `plugin.js`, a `device.json`, or
both. The web server lists it; the Settings or File Manager page loads a
`plugin.js`, which renders into a card on that page, and the Plugins page opens a
`device.json` catalog (see [Catalogs](#catalogs-upstreams-devicejson)). Adding
or changing a plugin never needs a firmware build.

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
> manifest lists, or that you approved when it asked. Upstream's on-device catalog
> screens (`device.json`) run in the browser here, on the Plugins page. Its crypto
> endpoint (`api.crypto`) and its device events are here, the events from
> #3114 and #4200 (Arthur Tazhitdinov) without `book.downloaded` so far (see
> [Events](#events-the-reader-sends-a-plugins-requests)); its job queue is not —
> see [Trust](#trust) and [Reaching the internet](#reaching-the-internet-allowedhosts-and-approvals)
> below.

Plugins extend the **web interface**, not the reader. No plugin code runs on the
device: the firmware enumerates the folders and serves their bytes, and the
script, or the catalog, runs in the browser of whoever opened the page. Cost on the device is
one directory scan per page load and no resident RAM. The one thing the reader
does for a plugin by itself is send the HTTP requests its `device.json` declares
for reading and sleep events ([Events](#events-the-reader-sends-a-plugins-requests)).

## Layout

```
<root>/<name>/
    plugin.js       a web part: a card on Settings or the File Manager
    device.json     a catalog in upstream's format, opened on the Plugins page
                    (a folder with neither is not listed)
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

**`api.crypto(op, fields)`** runs on the device, as upstream's does: the page has
no `crypto.subtle`, which exists only in a secure context, and the reader serves
`http://`. It posts the op and its base64 fields to `POST /api/crypto` (random,
sha1, sha256, aesenc/aesdec, rsadec, sign, keygen, pubencrypt, pkcs12; see
[webserver-endpoints.md](webserver-endpoints.md)) and resolves to the device's
JSON; a failed op answers `{error}`. Keys come and go with each call: the device
keeps nothing.

A plugin may declare what it needs, and one asking for something this firmware
does not implement is refused with a visible message instead of failing somewhere
inside its own code:

```jsonc
{ "requires": ["relay", "fetchToSd"] }
```

Supported: `relay`, `fetchToSd`, `writeFile`, `pluginFile`, `dir`, `crypto`.

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

No list and no approval means no access. `relay(url)` and `fetchToSd` follow a
redirect only to a host the plugin may reach too. One to any other host is
refused like a first request, naming the host, so the page asks for it as well
(a GitHub release file redirects to GitHub's CDN); list the hosts a service
redirects to in `allowedHosts` to skip the question. The four-argument relay
follows no redirect at all.
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

## Catalogs: upstream's `device.json`

Upstream crosspoint-reader describes a catalog of books or files in a plugin's
`device.json` and browses it on the reader (`PluginCatalogActivity`, #3114 by
Justin Mitchell, and the PRs after it). Here the Plugins page browses it: an
installed plugin with a `device.json` has an **Open catalog** button, and the
catalog opens in a card of its own with its lists, a search field when it has
one, its rows with a **Download** button each, and page buttons. Upstream's
[docs/sd-plugins.md](https://github.com/crosspoint-reader/crosspoint-reader/blob/develop/docs/sd-plugins.md),
"Surface 3", defines the fields; `src/network/html/js/plugin-catalog.js` runs
them.

Every request goes through the plugin's own `relay` and `fetchToSd` (the same
`api` a `plugin.js` gets), so the device fetches for it and only reaches the
hosts its manifest lists or you approve when it asks
([below](#reaching-the-internet-allowedhosts-and-approvals)). Downloads go
straight to the card.

What runs:

- JSON catalogs: `browse.url`, `method`, `headers`, `body`, `items`, `fields`
  (`title`, `author`, `id`, `url`, `description`), `page_size` (1 to 16).
- The four paging modes, chosen by the variables the request uses: `{offset}`
  (with one lookahead row), `{page}` with `{limit}` (a full page means another),
  `{page}` alone (a row beyond `page_size` means another), and neither (one
  answer, paged in the browser after filtering).
- `lists` with their own `url`, `body` and `id` (`{list_id}`, `{list_id_enc}`),
  `notice` (asked before the list opens; `once` with an `id` is remembered in
  that browser), `requires`; `lists_url` with `lists_headers`, `lists_items` and
  `lists_fields`, and server paths (`/files/1`) joined to the request's host.
- `search.url` (and `body`) with `{query}` (encoded) and `{query_raw}`.
- `browse.compatibility`: rows for another `firmware_family`, a newer
  `plugin_api_min` or a capability this firmware lacks are left out.
- Downloads: `download.url` (default `{url}`), `method`, `headers`, `body`,
  `username`/`password` (HTTP Basic), `url_path` (the file's URL from a first
  answer; a full URL gets no headers, a server path does), `dest_dir`,
  `filename` (default `{title}.epub`, sanitized as upstream does), and `bundle`
  (`base` and `files` from the row, into `dest_dir/<subdir>`, default `{id}`).
- Template variables: the row's `{id}`, `{title}`, `{author}`, `{url}` and
  their `_enc` forms, `{page}`, `{offset}`, `{limit}`, `{firmware_family}`
  (`witchhunt`), `{firmware_version}`, `{plugin_api}` (1),
  `{plugin_capabilities}`, `{plugin_name}`, `{cfg.KEY}` from the plugin's
  `config.file` (whose `dest_dir` overrides the manifest's), and `{token}`.
- Sign-in. `{token}` comes from `token.file` at the dotted `token.path`
  (default `token`). Without one:
  - `auth.type` `password` mints it: `auth.request`, filled from the config,
    and the token at `auth.token_path` (default `access_token`). A 401 or 403
    while browsing, fetching the lists, asking a `url_path` or downloading
    mints a new one and tries once more.
  - `auth.type` `device_code` (upstream's default) shows the code from
    `auth.request` (`code_path`, `verify_url_path`) and a link to the address,
    then asks `auth.poll` with `{device_code}` every `interval` seconds (at
    least 3) until it answers a token. `slow_down` adds 5 seconds;
    `access_denied`, `expired_token` and the code's expiry (`expires_in`, at
    least 60 seconds) end it. A refused token later starts a new sign-in.
  - Otherwise the plugin's web card signs in and writes the file; the catalog
    says so and opens once it has.

  A token is saved as upstream saves it: `token.file` rewritten to hold only
  the token, nested at `token.path`, through the plugin's `writeFile`.
- XML catalogs (`browse.format` `xml`, a WebDAV `PROPFIND` listing, say),
  by the rules of the SDK's `XmlListParser` (Justin Mitchell). Each element
  named `browse.item` is a row. `fields` are selectors: `elem` (the leading
  text of the first descendant `<elem>`), `elem@attr` or `@attr`, matched by
  local name. A row needs a `url`, and a missing `title` is the url's last
  path segment. `skip_self` drops the listing's own entry, `extensions` keeps
  only files with those endings, and `resolve_urls` joins a relative href to
  the server. A row holding a `container_element` is a folder: **Open** lists
  it, **Up** goes back. No lists or search, as upstream; the download URL
  defaults to the row's `url`.

The capabilities a catalog may require (`compatibility.requires`, a list's
`requires`): `catalog.lists`, `catalog.notices`, `catalog.bundle_install`,
`catalog.compatibility`, `catalog.encoded_item_fields`,
`catalog.list_index_mapping`, `catalog.list_requirements`,
`catalog.server_paths`. A `firmware_family` of `crosspoint` passes as well.

Differences from upstream:

- A `dest_dir` of `/.sleep` or `/.dictionaries` saves to `/sleep` or
  `/dictionaries`. The firmware reads both spellings, and the web server refuses
  dot folders while **Show Hidden Files** is off.
- A relative `dest_dir` is in the plugin's own folder, as upstream's is. In
  `/.crosspoint/plugins` the page can make a folder there only while **Show
  Hidden Files** is on; without the folder, the download fails.
- A bundle is written file by file, not staged: one cut off half-way keeps the
  files it got, and downloading it again finishes it.
- A `token.file`, `token.refresh_file` or `config.file` may sit outside the
  plugin's folder (`/.crosspoint/lingoblend.json`, say), as upstream allows: the
  web server lets the plugin read that exact file with `/download` and write it
  with `writeFile` (`PluginLocations::deviceJsonNames()`). A name with `..` in
  it names nothing. A credential store, a system folder and a path spelled by its
  8.3 alias stay refused, as for every request.
- Not yet: Details pages, the `sidecar` file, and sign-in types upstream does
  not define (`refresh`). A `device.json`'s `events` are the reader's business:
  [Events](#events-the-reader-sends-a-plugins-requests).

## Events: the reader sends a plugin's requests

A `device.json` may declare an `events` section, in upstream's format
(crosspoint-reader #3114, Justin Mitchell; `require_meta` from #4200, Arthur
Tazhitdinov): per event, one HTTP request or one download that the reader sends
when the event has happened. No plugin code runs for it: the reader queues the
event on the card and later fills in the declared templates and sends them. A
progress sync (`kosync-autopush`) and a sleep image (`monthwallpaper`) are the
usual uses.

```json
"events": {
  "reader.session": { "connect": true, "toast": "Progress pushed",
    "request": { "method": "PUT", "url": "{cfg.server}/syncs/progress",
                 "headers": { "x-auth-user": "{cfg.user}" },
                 "body": "{\"document\":\"{event.document}\",\"percentage\":{event.end_progress_bp}e-4}" } },
  "sleep.enter": { "download": { "url": "https://example.org/today.bmp", "dest": "/sleep.bmp" } }
}
```

| Event | When | Variables (`{event.NAME}`) |
|---|---|---|
| `reader.open` | a book opens | `book` (its path) |
| `reader.exit` | a book closes, also when sleeping out of it | `book`, `percent`, `end_percent` |
| `reader.session` | a book closes or the reader sleeps, after at least 10 s of reading | `book`, `document`, `start_time`, `end_time`, `duration_seconds`, `start_progress_bp`, `end_progress_bp`, `progress_scale` |
| `sleep.enter` | the reader goes to sleep | `book`, `percent`, `end_percent` when sleeping out of a book, else none |

- **Reading time** counts the time on a page before a forward turn, from 2 s to
  30 min a page, as upstream does; backward turns and skimming add nothing.
  `start_time` and `end_time` are Unix seconds, so a session is sent only while
  the clock is set (a hardware clock, or network time). `document` is KOReader's
  partial MD5 of the book file, the id KOSync servers match books by. Progress
  goes in basis points of `progress_scale` (10000).
- **Not sent yet**: `book.downloaded`, and a handler with `require_meta` (it
  fires only for books whose `<book>.meta.json` has that key, and nothing writes
  those files here yet). The Plugins page names such events.
- **Templates**: `{cfg.KEY}` from the plugin's `config.file`, `{token}` from its
  `token.file` (at `token.path`), `{meta.KEY}` from the book's
  `<book>.meta.json`, and `{event.ts}`, `{event.id}` and `{event.NAME}`. Strings
  and whole numbers go in as text; a key that is missing stays as written. One
  pass: a filled-in value is not read for placeholders again.

**The queue.** Each subscribing plugin has its own, `events.jsonl` in its folder,
one line per event (`{"e", "id", "ts", "vars"}`). A line is at most 512 bytes;
a queue over 4 KB is dropped whole, as its events are stale by then. Delivery is
at least once and in order per plugin: an event leaves the queue when its
request answers 2xx (a download: when its file is in place), and the first
failure stops that plugin's queue until the next try. A plugin whose handlers
this firmware cannot run has its queue removed.

**When it is sent.** Whenever the reader joins Wi-Fi through its network picker
(File Transfer's **Join a Network**, among others), and at sleep entry: over a
connection that is up, or by joining the last network with its saved password
when a queued event's handler says `"connect": true` (`sleep.enter` always
does). The join waits 10 s at most, and not under 20 % battery; a failed join
or request does not hold up the sleep, and the queue waits for the next
connection. At most 4 events go per drain. A `toast` shows as a popup after its
request succeeded. The subscriptions are read at boot; plugins are installed
over File Transfer, whose end restarts the reader.

**X3 and X4.** With two framebuffers allocated, joining Wi-Fi leaves the C3 too
little for TLS (`HalCapabilities::sendsSleepEventsFromCleanBoot`). There the
sleep screen is painted first, then the reader restarts, sends the queue before
the display takes its buffers (about 210 KB free), and restarts into the sleep.
The two restarts take about 1.7 s each on the X3, on top of the network time.
The sleep screen is drawn again only when a download replaced a file and the
screen is made of custom images alone (Custom, or Cover with no book open);
Quick Resume and Overlay keep what was painted. No toasts there.

**Trust.** A request, and any download with headers, needs a verified server
certificate, since it carries the user's data; a plain download falls back as a
public read does. A download goes to an absolute path outside the plugin
folders, the credential stores and the system folders, through a `.part` file
that replaces the target only after a clean answer. A request's answer is read
to 8 KB and dropped. The serial log (`[PEVT]`) names the URL and status of each
delivery, never headers or bodies.

**Limits.** 8 subscribing plugins; a `device.json` up to 8 KB; token, config and
`<book>.meta.json` files up to 2 KB.

**Made by the Home Screen page.** A picture from the web on Home is a plugin of
this kind that the Home Screen page writes, `home-picture-<n>`: one download into
`/home/web` on `sleep.enter` or `reader.exit`
([home-layout.md](home-layout.md#pictures-from-the-web)). The page replaces and
removes them; the Plugins page lists them like any other.

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
  out of date. A plugin folder with a `plugin.js`, a `device.json` or both is
  listed; a manifest with `"catalog": false` stays out of it.

  Entries have the shape of crosspoint-reader's plugin catalog: `name`, `title`,
  `description`, `author`, `version`, `base` (the folder the files are read
  from) and `files` (their names), plus this firmware's `mount`, `requires` and
  `allowedHosts`. `base` is resolved against the catalog's own URL. The script
  gives this repository's plugins a relative one (`find-duplicates/`), so a
  catalog read from another branch installs that branch's files. A plugin hosted
  elsewhere is added to `catalog.json` by hand with an absolute `base`; the
  script keeps such entries, and the page shows where they come from.
- **From the CrossPoint community catalog**: the page's **CrossPoint community**
  switch lists CrossPoint's Plugin Hub
  (`crosspoint-reader/crosspoint-plugin-hub`, `catalog.json`), which carries the
  `itsthisjustin/sd-plugins` plugins too, often in newer versions. The browser
  reads it from GitHub when the switch is first chosen. Neither repository has a
  license, so nothing of them is in this repository: the page links to their
  files, and an install fetches them. The plugin stores (`pluginhub`,
  `plugin-store`) are left out; this page is the store here. Each entry says
  what of it runs on this firmware, judged from the catalog and from the entry's
  `device.json`, which the browser reads from GitHub as well:
  - **runs here**: a web part (`plugin.js`), a catalog that opens here
    ([Catalogs](#catalogs-upstreams-devicejson)), events the reader sends
    ([Events](#events-the-reader-sends-a-plugins-requests)), or several of them;
  - **partly runs here**: one of them runs, and something does not yet: an
    event the reader does not send yet (`book.downloaded`, a handler with
    `require_meta`), say;
  - **does not run here**: nothing of it runs (a `device.json` whose only
    events the reader does not send, for example), another `firmware_family`, a requirement this firmware lacks
    (named), a `device.json` that could not be read, or a reason the catalog
    does not state. These have no Install button.
  `protected-content` (Adobe-protected loans and purchases) is marked **partly
  runs here** with a note: it runs up to the account sign-in on a device, while
  activation and fetching have been tried only against a mock Adobe service.

  Files a community entry lists in subfolders (the dictionaries catalog's own
  pages, which its plugin reads from GitHub) are left out of the install, and the
  page says how many: plugin folders here are flat.

  An entry sharing a folder name with one of ours (`organize-by-author`) says
  so: installing one replaces the other. A community install asks once, naming
  the author, and the plugin then reaches the internet only through hosts you
  approve when it asks (see
  [Reaching the internet](#reaching-the-internet-allowedhosts-and-approvals)).
- **From a `.zip`**: the plugin's files side by side, or inside one folder, with
  a `plugin.js` or a `device.json`. The page shows the manifest's title,
  description and `allowedHosts` and asks before installing.

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
That includes a plugin's own folder, and the token and config files its
`device.json` names elsewhere: anyone on the network can read them with
`/download` while File Transfer runs, as on upstream. A plugin can also overwrite
any file its own `device.json` names; only the credential stores, the system
folders and 8.3 alias spellings are off limits.

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
  request and each candidate costs four SD stats, so an unbounded walk over a
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
   is missing from that list, the folder has neither `plugin.js` nor
   `device.json`, or a same-named folder in an earlier root shadowed it.
4. Script errors surface in the browser console, not on the device. Watch
   serial (`[WEB]`) only for manifest and file-serving problems.
