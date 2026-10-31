# The web interface on the SD card — design

> **Status.** Implemented. `src/network/WebAssets.h` installs and serves the bundle,
> `scripts/build_html.py` builds it, `src/network/html/WebSetupPage.html` is the page that stays in
> flash. Not yet validated on a device.

## Problem

The app image is the constraint, not RAM: `env:default` was 6,289,824 B of the 6,553,600 B app slot
(96 %). The slot cannot grow: the partition table is shared with upstream CrossPoint and is not
updated by OTA, and on the C3 a bigger image also takes 64 KB map pages from the flash font cache
(see `docs/contributing/flash-font-partition.md`).

The web interface was 74,951 B of gzip in that image: seven assets compiled into byte arrays and
served with `send_P`. None of it is needed to read a book, and all of it is only ever used while a
browser is connected to the device, which is when the SD card is right there.

| Asset | Route | gzip bytes |
|---|---|---|
| `FilesPage.html` | `/files` | 30,103 |
| `js/jszip.min.js` | `/js/jszip.min.js` | 27,346 |
| `SettingsPage.html` | `/settings` | 5,501 |
| `StatsPage.html` | `/stats` | 4,328 |
| `FontsPage.html` | `/fonts` | 3,816 |
| `HomePage.html` | `/systeminfo` | 2,825 |
| `WelcomePage.html` | `/` | 1,032 |

Moving them out saved 68,096 B of `firmware.bin`; the setup page that replaces them is 1,831 B.

## Design

### One bundle, pinned by hash

`build_html.py` builds every asset as before, but only `FLASH_ASSETS` (the setup page) becomes a
header. The rest are joined, in sorted order, into `web-<first 12 hex of SHA-256>.bin`, written to
the env's build folder. `html/WebBundle.generated.h` gives the firmware the bundle's table (file
name, content type and size per asset, plus an `Index` enum the routes use) and its SHA-256.

Installing checks a bundle against that hash before writing anything. That one check settles three
questions at once:

- **Where the bundle came from.** A download from the release, an upload through the setup page and
  a copy made in a card reader are accepted on the same terms, because only the exact bytes this
  build expects pass. The download therefore uses unverified TLS: the hash authenticates the bytes
  more strictly than a certificate chain, and skipping the chain verify keeps the handshake cheap.
- **Whether it matches the firmware.** There is no version-compatibility rule to get wrong.
- **Whether a firmware update needs new pages.** The installed stamp (`bundle.id`) holds
  `web_bundle::PAGES_ID`, not the bundle's hash. The two answer different questions: `DIGEST` (the
  hash of the bytes) says whether a file is the bundle this build would install, and `PAGES_ID`
  says whether the installed pages are the pages this build serves. `PAGES_ID` is the SHA-256 of
  the page sources (includes resolved, line endings normalised, build placeholders unsubstituted),
  the other bundled files, the product name and `WEB_API_VERSION` (`scripts/build_html.py`). It
  does not cover the compressed bytes, so a build with another terser, zopfli or host line endings,
  and any firmware version or branch, accepts the installed pages. A page edit changes it
  automatically; `WEB_API_VERSION` is bumped by hand when a page would misbehave against the new
  server although no page changed (an endpoint a page calls was renamed, removed or changed its
  reply incompatibly). Forgetting the bump is the one way this goes wrong, which is why the
  sources are in the identity too.
- **What the pages know of the build.** Nothing. Decision: a bundled page keeps `%%VERSION%%` (and
  any other build-specific value) as a placeholder, and fills it in at run time from the server
  (`/api/status`, as `WelcomePage.html` does), starting from neutral text and setting it on
  `DOMContentLoaded`. `build_html.py` substitutes the version only into the setup page, which is
  in flash. Otherwise every version string would change the bundle for every firmware build.

A card stamped by a firmware from before `PAGES_ID` holds a bundle hash, which no `PAGES_ID`
equals: it reads as stale and is reinstalled once.

Installed files may therefore differ in size from this build's `ENTRIES[i].size` (same pages,
different compression); `serve()` uses the file's own size, and `ENTRIES` sizes are used only to
split a bundle being installed, which has been hash-verified.

The bundle has no header or manifest: the firmware already knows every file's size, so splitting
it is a loop over `web_bundle::ENTRIES`. A given source tree builds byte-identical bundles on a
given toolchain (zopfli and terser are deterministic, the order is sorted, text files' line endings
are normalised), which is what lets one release asset serve every board. The release asset is
`web-<DIGEST>.bin` under each release's tag; two releases with the same bytes simply each carry an
asset of the same name, and the lookup is by tag and name, so it is unaffected.

### Installing

`WebAssets::installFromFile()` reads the bundle twice: once to check its size and hash, then to
split it into `/.crosspoint/web.new/`, write the stamp and rename that folder over
`/.crosspoint/web/`. A cut download or a wrong file never touches the installed set. A power cut
between the delete and the rename leaves no folder, which reads as Missing and is reinstalled.

Two sources:

- **The release:** `https://github.com/<repo>/releases/download/<tag>/<asset>`, fetched with
  `HttpDownloader::downloadToFile` (Range chunks on a tight heap) into
  `/.crosspoint/web-download.bin`. The tag is `WEB_BUNDLE_RELEASE_TAG`, set per env by
  `build_html.py`: for the `*gh_release` envs the tag `release.yml` publishes under
  (`CROSSPOINT_RELEASE_TAG`, falling back to the base version in a local build), the CI tag for the
  `*gh_release_rc` envs, empty for everything that is never published. The tag need not be the
  base version: 2.41 went out as `2.41RC`, and the base version sent every download to a 404 (#581).
- **The card:** `/<asset name>` in the top folder. The setup page uploads a chosen file there
  through the ordinary `/upload`, under the asset's name; a user with a card reader or USB Drive can
  put it there directly. Installed copies are deleted, wrong ones are left for the user to see.

`/.crosspoint` is already off limits to the web file endpoints (`ProtectedPaths`), so nothing but
the installer writes the installed pages.

### Serving

`WebAssets::serve()` streams a file with `Content-Encoding: gzip` through
`HttpFileStreamer::streamFileToClient`, as plugin files are served. It returns false before sending
anything when the bundle is not installed, and marks it Missing when a file is gone or has the
wrong size, so a damaged set heals itself through the setup page.

- A page that is not installed redirects to `/setup`.
- `/` is the Welcome page when installed and the setup page otherwise, served `no-cache`, and the
  setup page itself `no-store`: whichever one a browser kept would otherwise outlive the switch.
- `/js/jszip.min.js` answers `404` instead of redirecting, since it is a script.

### The setup page

`/setup` (and `/` while nothing is installed). On load it asks `/api/web/status`, then:

- installed: says so and links to `/`;
- a bundle is on the card: installs it;
- on a normal network in a published build: downloads it, with no click needed, so the first visit
  after an OTA update that changed the pages fixes itself;
- otherwise (hotspot mode, development build, or a failed install): offers the bundle upload, and a
  plain book upload that works with nothing installed, so getting books onto the device never
  depends on the bundle.

### Release pipeline

`ci.yml`, `release.yml` and `release_candidate.yml` check that each leg built exactly one
`web-*.bin` and add it to the leg's artifact. Every leg builds the same file, so the merged
download holds one copy, and the publish step's `release-assets/*.bin` attaches it.

## Ruled out

- **Keeping the File Manager in flash** (~41 KB saved instead of ~68 KB). Chosen against because
  the setup page keeps a plain book upload, and on a normal network installing needs no click.
- **A release-version check** instead of a hash: it would need a compatibility rule, would force a
  new bundle on every release, and would not cover uploads or dev builds.
- **Fetching the pages from the browser** (a GitHub-hosted UI calling the device): a page served
  over https may not call `http://` on the device, and the device cannot serve https.
- **Prefetching the bundle during an OTA update.** Not needed while the setup page installs by
  itself, and it would add a second download to the OTA path.

## Validation

Build-verified on `env:default`; the bundle rebuilt from scratch has the same hash. Still to do on
a device: first install from the release (needs a published release carrying the asset), install
by upload in hotspot mode, install from a file copied to the card, a deleted page file healing
through the setup page, and the book upload from the setup page.
