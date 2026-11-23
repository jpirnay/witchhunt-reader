# Plugins

Status, 2026-11-20. Upstream's web and device plugins run here as far as the owner's decisions of 2026-11-16
reach: the relay and fetch with per-plugin host approval (#797), the Plugin Hub offered on the Plugins page and marked
by compatibility (#807), `device.json` catalogs run in the browser (#809), and sign-in, XML (WebDAV) catalogs and the
token and config files a `device.json` names (#814). This repository's catalog lists a `device.json`-only plugin
(#824), `/api/files` lists a dot folder named by its path, as upstream does, so plugins find their own files (#828),
a failed `/api/fetch` names the file server's answer (#827), and `api.crypto` runs upstream's crypto ops on the
device for the Plugin Hub's `protected-content` (Adobe loans and purchases, #857). The decisions: hosts are approved per
plugin on the Plugins page,
`device.json` catalogs run in the browser and never on the reader, events run on all boards, and the Hub's
plugins are offered (fetched at install, never copied here: the Hub and sd-plugins have no license).

## Open

- **`protected-content` with a real account**: run on devices up to its sign-in and end to end against a mock Adobe
  service; a real Adobe ID is open (the owner's go first). See [protected-content.md](protected-content.md).
- **Not tried live**: the sign-in flows (password grant, device code), for want of accounts; the WebDAV catalog
  against the owner's server (a LAN test server of our own was refused by the session's safety checks). Dropbox's
  `refresh` auth type is not supported and is shown as "does not run here".
- **Events, step 2: parked** (owner, 2026-11-20). Step 1 (the queue, `reader.open`/`exit`/`session`, `sleep.enter`,
  the drain on a Wi-Fi join and at sleep, on the C3s from a clean boot) is in, #861; see
  [sd-plugins.md](../sd-plugins.md#events-the-reader-sends-a-plugins-requests). Step 2 would bring:
  - the `<book>.meta.json` sidecar, written by the Plugins page's catalog runner after a download from the
    `device.json`'s `sidecar` field, which `require_meta` handlers need. Gain: Readeck sends reading progress back to
    the article (its `reader.exit` and `sleep.enter` handlers). Downloading from Readeck's catalog works already. The
    firmware side (`require_meta` matching, `{meta.*}` templates) is in;
  - `book.downloaded`, which the browser would report to the reader through a small endpoint after a catalog
    download. Gain: ybx's handler for it; its other events run already;
  - a password grant's token refresh on 401/403. No event plugin in the Hub uses one.

  Take it up when someone uses Readeck or ybx. Meanwhile the Plugins page marks those handlers "not sent yet".
- **Not done from the survey**: an `/api/settings?keys=` filter; the job queue (`registerAction` only warns). The
  job queue matters to `protected-content`: it renews a loan only as a queued job (`renew`, a new `.acsm` for a book
  already on the card), so renewing a loan does not work here; fetching a book again does.

Survey and decisions: `~/.local/state/witchhunt-work/plugin-survey/plugin-survey-2026-11-16.md`.
