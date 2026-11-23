# Plugins

Status: upstream's web and device plugins run here as far as the owner's decisions reach: the relay and fetch with
per-plugin host approval (#797), the Plugin Hub on the Plugins page, marked by compatibility (#807), `device.json`
catalogs in the browser (#809), their sign-in, XML (WebDAV) catalogs and plugin-named files (#814), `device.json`-only
plugins in our catalog (#824), a failed `/api/fetch` naming the server's answer (#827), dot folders listed by path
(#828), `api.crypto` for the Hub's `protected-content` (#857) and the device events, step 1 (#861). What runs and how is
in [sd-plugins.md](../sd-plugins.md). The decisions: hosts are approved per plugin on the Plugins page, `device.json`
catalogs run in the browser and never on the reader, events run on all boards, and the Hub's plugins are offered
(fetched at install, never copied here: the Hub and sd-plugins have no license).

## Open

- **`protected-content` with a real account**: run on devices up to its sign-in and end to end against a mock Adobe
  service; a real Adobe ID is open (the owner's go first). Tracked in [protected-content.md](protected-content.md).
- **Not tried live**: the sign-in flows (password grant, device code), for want of accounts; the WebDAV catalog
  against the owner's server (a LAN test server of our own was refused by the session's safety checks). Dropbox's
  `refresh` auth type is not supported and is shown as "does not run here" (`src/network/html/js/plugin-catalog.js`).
- **Events, step 2: parked** (owner). Step 1 (the queue, `reader.open`/`exit`/`session`, `sleep.enter`, the drain on
  a Wi-Fi join and at sleep, on the C3s from a clean boot) is in, #861; see
  [sd-plugins.md](../sd-plugins.md#events-the-reader-sends-a-plugins-requests). Step 2 would bring:
  - the `<book>.meta.json` sidecar, written by the Plugins page's catalog runner after a download from the
    `device.json`'s `sidecar` field, which `require_meta` handlers need. Gain: Readeck sends reading progress back to
    the article (its `reader.exit` and `sleep.enter` handlers). Downloading from Readeck's catalog works already. The
    firmware side (`require_meta` matching, `{meta.*}` templates, `src/util/PluginEventCore.h`) is in;
  - `book.downloaded`, which the browser would report to the reader through a small endpoint after a catalog
    download. Gain: ybx's handler for it; its other events run already. The event name is known to the queue
    (`PluginEventCore.h`) but nothing emits it;
  - a password grant's token refresh on 401/403. No event plugin in the Hub uses one.

  Take it up when someone uses Readeck or ybx. Meanwhile the Plugins page marks those handlers "not sent yet".
- **Not done from the survey**: an `/api/settings?keys=` filter (`CrossPointWebServer::handleGetSettings` reads no
  arguments); the job queue (`registerAction` only warns, `src/network/html/shared/PluginHost.js.inc`). The job queue
  matters to `protected-content` ([protected-content.md](protected-content.md)): it renews a loan only as a queued job
  (`renew`, a new `.acsm` for a book already on the card), so renewing a loan does not work here; fetching the book
  again does.

Survey and decisions: `~/.local/state/witchhunt-work/plugin-survey/plugin-survey-2026-11-16.md`.
