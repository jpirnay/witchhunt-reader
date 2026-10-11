# Web UI assets on the SD card

Status: analysed 2026-10-11, shelved. Nothing is built.

The web server's pages are compiled into the firmware as gzip byte arrays. Moving them to the SD card frees app-partition flash; the cost is a setup step on the web UI's most important flow. This file records the numbers, the design that avoids the obvious traps, and a smaller variant that keeps most of the benefit.

## What the pages cost today

`scripts/build_html.py` minifies each `src/network/html/**/*.html` and `.js`, compresses it with zopfli and writes a `*.generated.h` byte array, which `CrossPointWebServer.cpp` serves with `send_P` (`sendHtmlContent()`, `handleJszip()`). Sizes at `fdc89d200`:

| Asset | Route | gzip bytes |
|---|---|---|
| `FilesPage.html` | `/files` | 30,103 |
| `js/jszip.min.js` | `/js/jszip.min.js` | 27,346 |
| `SettingsPage.html` | `/settings` | 5,501 |
| `StatsPage.html` | `/stats` | 4,328 |
| `FontsPage.html` | `/fonts` | 3,816 |
| `HomePage.html` | `/systeminfo` | 2,825 |
| `WelcomePage.html` | `/` | 1,032 |
| **Total** | | **74,951** |

`env:default` was 6,289,824 B of the 6,553,600 B app slot, so 263,776 B of headroom. The assets are 1.2 % of the image and about 28 % of that headroom. After a stub page and the installer, the full move saves about 65–70 KB. For scale: built-in font tables are 2.52 MB and the hyphenation tries 350 KB.

## Design

**Pin the version with hashes built into the firmware.** `build_html.py` stops emitting byte arrays and emits two things instead:
- a table compiled into the firmware: `{name, size, sha256}` per asset, about 300 B;
- `web.bin`, the gzip files joined in table order. Sizes come from the table, so the bundle needs no header, manifest or archive format.

The firmware verifies every file against its own table while installing. Consequences:
- Where the bundle came from no longer matters: GitHub, a browser upload and a USB copy are equally trusted, because only the exact bytes this build expects are accepted.
- A bundle left over from before an OTA update fails the check, so the stub reports it as out of date. No separate version compatibility rule is needed.
- The pages hash differently in every release, because `%%VERSION%%` is substituted into them. That is fine; one bundle per release is the plan anyway.

**A stub at `/`, kept in flash.** It replaces `WelcomePage.html` and shows whether the web UI is installed, missing or out of date. It offers:
- **Download from GitHub.** Only works when the device is in station mode with internet access.
- **Upload bundle file.** A file input that posts `web.bin` to the installer. This is the path for hotspot mode, where the device has no internet.
- **A plain multipart form to `/upload`.** `handleUpload()` already accepts a bare form post with an optional `?path=`, so getting books onto the device never depends on the bundle.

**Storage.** `/.crosspoint/web/<name>.gz`. `ProtectedPaths` already refuses web writes to dot-named items, so `/upload`, `/rename`, `/move` and `/delete` cannot touch the installed pages; only the installer writes there. Install into a staging folder, verify every hash, then rename it over the live one, so a cut download or power loss never replaces a working set. Verify at install time only. A request checks only that the file exists and has the expected size.

**Serving.** Stream the `.gz` with `Content-Encoding: gzip` through `HttpFileStreamer::streamFileToClient()`, as `handlePluginFile()` does. Routes stay the same. A route whose file is missing or the wrong size redirects to `/`.

**Download.** `HttpDownloader` follows redirects (`isRedirect()`, `HttpDownloader.cpp:56`), and the GitHub root is already pinned for OTA, so `https://github.com/<repo>/releases/download/<tag>/web.bin` works from a request handler the way `handleFontDownload()` does. Stream it straight to the staging folder: split at the table's sizes and hash each part as it passes. Do not buffer the whole file.

**Release CI.** `release.yml` and `release_candidate.yml` attach one `web.bin` per tag. The pages are the same on every board, because the only placeholders are `%%CROSSPOINT%%` and `%%VERSION%%` (`get_version_string()`), so the X3/X4, X4 Pro and T5S3 builds share it. The asset must exist for every tag, RCs included, or that release has no full web UI.

**OTA.** `OtaUpdater` knows the new tag (`getLatestVersion()`) and has a GitHub session open. It can fetch the new tag's `web.bin` into the staging folder before rebooting. The new firmware verifies the staged files on first web-server start and promotes them, so an update does not leave the user on the stub.

**Dev builds.** `env:default` versions are `X-dev+<branch>`, and no release exists for them. The build writes `web.bin` next to `firmware.bin`, and the developer installs it through the stub's upload. Keeping an embed-in-firmware flag for development is possible, but then nobody runs the release behaviour day to day.

## Costs

- **First-run friction on the main flow.** A fresh install, a new or formatted card, or hotspot-only use leaves the user with the stub's basic upload until the bundle is installed.
- **More states to test.** Missing, out of date, corrupt and half-installed bundles, plus SD read errors, are failure modes that pages in flash do not have.
- **Heap.** Serving from SD claims the 4 KB stream buffer instead of reading from flash. The GitHub download needs a TLS session inside a request handler, as the font download does today. That works, but it is the tightest moment of a web session.
- **Security.** Not new. Pages served from SD run as the device's own site, but plugins already do that (`handlePluginFile()`), and the hash check is stricter than anything plugins get.

## Smaller variant: move only the optional assets

Keep `/files`, `/` and `/systeminfo` in flash (about 34 KB). Move the rest (about 41 KB) to SD, installed on demand with the same table, bundle and installer:

- **jszip** only powers the optional in-browser EPUB conversion and image picker. `FilesPage.html` already copes when it fails to load (`typeof JSZip === 'undefined'`, around line 3036).
- **Fonts** needs device internet to do anything, so it can always install itself first.
- **Settings and Stats** are also available on the device.

This drops the first-run step and keeps book upload working on every card. The full move saves about 30 KB more.

## Next step

When flash headroom becomes the binding constraint, start with the smaller variant:
1. Add the table and bundle output to `build_html.py`.
2. Build the installer and the SD serving path.
3. Attach `web.bin` in both release workflows.
4. Measure the image before and after with `scripts/firmware_size_history.py`, using `--env gh_release`, which is the build that ships.
