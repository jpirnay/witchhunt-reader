# Plugins

Status, 2026-11-19. Upstream's web and device plugins run here as far as the owner's decisions of 2026-11-16
reach: the relay and fetch with per-plugin host approval (#797), the Plugin Hub offered on the Plugins page and marked
by compatibility (#807), `device.json` catalogs run in the browser (#809), and sign-in, XML (WebDAV) catalogs and the
token and config files a `device.json` names (#814). This repository's catalog lists a `device.json`-only plugin
(#824), and `/api/files` lists a dot folder named by its path, as upstream does, so plugins find their own files
(the pull request that adds this file). The decisions: hosts are approved per plugin on the Plugins page,
`device.json` catalogs run in the browser and never on the reader, events come later on all boards, and the Hub's
plugins are offered (fetched at install, never copied here: the Hub and sd-plugins have no license).

## Open

- **`api.crypto`**: absent; the Plugin Hub's `protected-content` (Libby, Onleihe) needs it at every step. It is D3 in
  [protected-content.md](protected-content.md), with the reason the comment in `PluginHost.js.inc` is wrong
  (`crypto.subtle` needs a secure context; the reader serves `http://`).
- **Not tried live**: the sign-in flows (password grant, device code), for want of accounts; the WebDAV catalog
  against the owner's server (a LAN test server of our own was refused by the session's safety checks). Dropbox's
  `refresh` auth type is not supported and is shown as "does not run here".
- **`/api/fetch` errors**: a failed download answers only "download failed", without the server's status. Hacker
  News's story text through `r.jina.ai` fails that way for github.com pages (a third-party block). #827 passes the
  status through (X3-checked); delete this item when it merges.
- **Events** (upstream's outbox, the sleep drain, `reader.session`): owner, 2026-11-16: later, on all boards (the C3
  joins Wi-Fi at sleep too). Not started.
- **Not done from the survey**: an `/api/settings?keys=` filter; the job queue (`registerAction` only warns).

Survey and decisions: `~/.local/state/witchhunt-work/plugin-survey/plugin-survey-2026-11-16.md`.
