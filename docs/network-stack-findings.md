# Network stack: what we measured, and what turned out to be false

A running record of the TLS / WiFi / sync investigation on the X4 (ESP32-C3, 160 MHz, no PSRAM).

Its purpose is the second half of the title. Most of the time in this investigation went into
hypotheses that sounded obviously right and were wrong, and each one cost a device flash to
retire. Read the "Retired" section before proposing a fix — several of the obvious ones are
already in there, with the measurement that killed them.

**Rule that earned its place:** never trust a diagnostic until it has been calibrated on a
known-good path. The one diagnostic here that produced a clean answer did so because it also
logged the *successful* case, which is how we found out it could not measure anything at all.

## How to read the logs

| Log line | Emitted by | What it answers |
|---|---|---|
| `[TLS] connect host: tcp+dns=.. hs=.. = io .. + sleep .. + compute ..` | `SecureClient::connectWithMethod` | Is the handshake network-bound or CPU-bound? |
| `[TLS] handshake call ms: [..]` | same | Which `wolfSSL_connect()` call carries the cost — first entry is ClientHello/keygen, the fat one is the server flight |
| `[TLS] chain verify: N certs [d3:.. d2:.. ..]` | `verifyCallback` | Per-certificate verification cost, root-most first. Needs `-DCROSSPOINT_TLS_VERIFY_TIMING` |
| `[HTTP] GET host: conn=fresh/..ms send ..ms/..B ttfb=..ms polls=.. hdr=..B rssi=.. ps=.. -> rc=..` | `SecureHttpClient::logRequestTrace` | Everything after the handshake. `conn` is connect+handshake; `ttfb` starts at the end of the send, so `ttfb=0` means nothing ever arrived |
| `[WIFI] EVT disconnected .. reason=201 (NO_AP_FOUND) .. bssid=00-00-.. rssi=-128` | event handler | An all-zero BSSID means the scan produced no candidate at all |

`ps=` is `WiFi.getSleep()`: 0 = `WIFI_PS_NONE`, 1 = `WIFI_PS_MIN_MODEM`.

All of these are `LOG_DBG`, so they exist only in development builds — release builds set
`LOG_LEVEL=1` and the macro compiles to nothing. What still reaches a release log from the TLS
layer is failures alone: a certificate-date waiver, a trust-store that would not load, a handshake
timeout, a verification failure. Note that `CROSSPOINT_TLS_VERIFY_TIMING` is not in
`platformio.ini`, yet it is defined in the binaries built here (the string is in `firmware.bin`),
so it is coming from a cached PlatformIO build environment — worth clearing if you want the
per-certificate callback out of a build entirely, since that define also changes wolfSSL's
callback behaviour, not just the logging.

## Confirmed

### TLS

- **The handshake is compute, not network.** 3314 ms total with 10 ms of socket I/O and 25 ms of
  our own poll sleeps. Everything else is wolfSSL doing math.
- **The cost is certificate verification, and it is P-384.** `kosync.rustysoft.de` is served from
  Let's Encrypt's gen-y all-ECDSA hierarchy: a 4-certificate chain (leaf ← YE2 ← Root YE ← ISRG
  Root X2, cross-signed by X1) in which three signatures are ECDSA P-384. Measured
  `[d3:242 d2:871 d1:876 d0:881]` — roughly 880 ms per P-384 verify.
- **Skipping validation removes ~2.7 s of it.** Same host, same device, validation off: handshake
  594 ms, no chain verify at all. What remains is ~560 ms of P-256: 131 ms ClientHello keygen plus
  ~430 ms for the ECDH shared secret. That floor is untouched by any trust decision.
- **wolfSSL verifies every presented peer certificate regardless of what is trusted.**
  `AlreadySigner()` is consulted only *after* `ParseCertRelative()` has verified the signature
  (`internal.c:14189`), and `internal.c:35-39` documents the default as "require validation of all
  presented peer certificates". Trust-store changes cannot shorten a chain walk.
- **The SP comb tables are off, and the switch is not where it looks.**
  `WOLFSSL_LOW_MEMORY` → `RSA_LOW_MEM` (`settings.h:1313`) → `WOLFSSL_SP_SMALL`
  (`sp_c32.c:42-50`), which drops the P-256/P-384 precomputed point tables. Two traps: our
  overrides are *prepended* to `user_settings.h`, so an `#undef` there is re-defined by the stock
  file below it; and `settings.h` re-defines `RSA_LOW_MEM` after `user_settings.h` is included, so
  the undef must target `WOLFSSL_LOW_MEMORY` and sit at the *end* of `user_settings.h`.
  Measured cost of enabling: **+100,254 bytes of flash** (93.8% → 95.3%), essentially all in
  `sp_c32.c.o` (37,752 → 138,490 B). The tables stay off.
- **`WOLFSSL_RSA_PUBLIC_ONLY` links only together with `WOLFSSL_NO_CLIENT_AUTH`.** The client
  verifies RSA and RSA-PSS signatures but never signs with an RSA key; the one RSA signer left in a
  client build is client-certificate authentication. With both defines (`scripts/patch_wolfssl.py`)
  the RSA private paths compile out, about 21 KB of flash. A server that asks for a client
  certificate still gets the empty Certificate message.
- **GitHub's chain needs `WOLFSSL_SP_4096`.** raw, objects and release-assets.githubusercontent.com
  send Let's Encrypt YR1 ← ISRG Root YR, cross-signed by ISRG Root X1, which is RSA-4096. Without
  `WOLFSSL_SP_4096` a 4096-bit key goes through the generic `sp_int` path, and on an X3
  `ConfirmSignature` failed with -155 at depth 2 while `openssl verify` accepted the same chain
  against our roots. Why is not established: wolfSSL also reports an allocation failure inside the
  signature math as -155, so the verify-failure log prints the depth and the heap.
- **Every connection runs a full handshake.** TLS 1.3 session resumption was built to spare the X3
  a second RSA-4096 chain check, then dropped once downloads were chunked (below): a build without
  it passed the same device test. wolfSSL's session cache is compiled out (`NO_SESSION_CACHE`).

### Downloads

- **GitHub sends 16 KB TLS records, and the X3 cannot hold one with Wi-Fi up.** wolfSSL reads each
  record into one heap block of the record's length, 17,408 B on the C3 for a full record. GitHub
  ignores `max_fragment_length` (the client asks for 2 KB records anyway, for servers that honour
  it) and sizes records dynamically, so they grow to 16 KB once a connection has carried some data.
- **A Range request bounds the record.** The server cannot send more than the range asked for, so
  an https download comes in HTTP Range chunks (`lib/SecureNet`: `HttpRange`, `RangeDownload`)
  whenever the largest free block is under `http_range::STREAM_MIN_LARGEST_BLOCK` (40 KB), which in
  practice means the C3 boards. The X4 Pro and T5S3 stream whole responses.
  - Chunks come from `CHUNK_LADDER` (6, 4, 3, 2, 1 KB), sized from the largest free block before
    each request, over one kept-alive connection. 6 KB is the start because the first 8 KB request
    of every download failed on the X3.
  - A read that runs out of memory reconnects and keeps that chunk size out of reach for the rest
    of the session (`ChunkSession::ceiling`).
  - A server that ignores Range (answers 200) is streamed from then on, one whole-file attempt per
    file, restarting from byte 0.
    A file whose size changes between chunks fails rather than being spliced. An expired redirect
    target (GitHub's presigned release-asset URL) sends the next chunk back through the original URL.
  - Retry budget: `MAX_RECONNECTS_BASE` (20) reconnects per file plus one per 64 KB, and a run of
    stalls longer than `MAX_STALLED_RETRIES` ends the download.
- **The firmware update uses the same path** (`HttpDownloader::fetchVerifiedRestartable`), so a
  6 MB image arrives in about a thousand ranges. A Range-ignoring server restarts the OTA partition
  (`esp_ota_abort`, `esp_ota_begin`). The image is hashed with wolfCrypt SHA-256 as it is written
  and compared with the release asset's `digest` before `esp_ota_end()`; a mismatch is
  `CHECKSUM_ERROR` and never becomes the boot target. A release without a digest is checked by size
  alone.
- **Downloads need memory freed first.** The font and firmware screens call
  `releaseMemoryForDownload()` before Wi-Fi comes up and again before the transfer. On top of the
  network trim (secondary framebuffer, glyph caches) it unloads a loaded SD font and the global
  bookmark index, and drops the settings lists of the screens buried below
  (`ActivityManager::releaseBuriedActivityState`).

### WiFi

- **`min` is the dwell when nothing answers; `max` only applies once an AP has been heard** — and
  the two values come from *different* APIs. Three measurements pin it down:

  | default params (`esp_wifi_set_scan_parameters`) | per-scan config (`esp_wifi_scan_start`) | AP heard? | measured |
  |---|---|---|---|
  | `min=0 / max=120` | `min=100 / max=300` | no | **101 ms** |
  | `min=120 / max=120` | `min=300 / max=300` | no | **121 ms** |
  | `min=120 / max=120` | `min=300 / max=300` | yes | **302 ms** |

  The effective `min` tracks the *default params* every time, and the effective `max` tracks the
  *per-scan config*. So `WiFi.setScanActiveMinTime()` — which sets the per-scan `min` — is
  silently overridden whenever `esp_wifi_set_scan_parameters()` has been called, which
  `applyScanBudget()` does on every connect. To lengthen a probe's floor you must raise the
  *default* `min` and put it back afterwards.
- **The driver keeps no scan records from an internal connect scan.**
  `esp_wifi_scan_get_ap_num()` returned 0 immediately after a *successful* association — so a 0
  after a failure carries no information.
- **The SSID has two APs, and only the device can see that.** Survey from the reader's own radio:

  | channel | signal | BSSID | SSID |
  |---|---|---|---|
  | 11 | **-65 dBm** | `f0-9f-c2-31-b8-0c` | PYSY |
  | 1 | **-91 dBm** | `80-2a-a8-c7-5b-be` | PYSY |

  Two different vendors' OUIs, two APs, one SSID. The far node on channel 1 is the one the old
  connection hint used to attach to, and at -91 dBm it is barely usable — so removing that hint and
  sorting by signal was right, and remains right. An earlier desktop scan could not hear the
  channel-1 node at all and produced a confident "there is no mesh" that was simply wrong; see the
  retired table. **Off-device scans cannot answer device questions.**

  Two caveats on the survey itself. `WiFi.scanNetworks()` defaults to `show_hidden=false`, so the
  hidden BSSIDs a desktop sees are filtered out here — the list is a floor, not a census. And this
  scan ran with **15,432 bytes free**, since the network-list path keeps the reader's framebuffers
  allocated; heap that tight can also truncate the driver's AP record list.
- **A BSSID-pinned connect fails where an unpinned one succeeds.** The probe saw the
  AP (`1 AP(s) on channel, 1 for 'PYSY' ... at -69 dBm`), the pinned `begin()` spent 1939 ms and
  returned `NO_AP_FOUND`, and the unpinned `begin()` that followed associated **with that same
  BSSID on that same channel 58 ms later** — far too fast to have scanned, so the driver still held
  the record the pinned attempt had rejected. A pinned connect probes the address directly;
  something here does not answer a directed probe the way it answers a broadcast one.

### Sync / server

- **Session-reused requests are fast in every run: 26, 27, 29, 34, 39, 54 ms.** Only the *first*
  request on a fresh connection is ever slow (30, 100, 110, ~2900, ~3081 ms, one 5 s timeout). A
  congested link would degrade both equally. The problem is in connection setup.
- **The sync server is healthy and fast.** From a desktop, five consecutive fresh connections:
  DNS 0.5 ms, TCP 14 ms, TLS 31 ms, TTFB 47 ms. `/` and `/healthcheck` return 200. It answers a
  properly-formed kosync request with 401 in 46 ms; the 502 it returns to a request *without* auth
  headers is not what the device sees.
- **`rc=-4` is `ERR_TIMEOUT` from `readHeaders()`** — the request was sent and no status line
  arrived within the 5 s budget. The following retry's `tcp+dns=2831 ms` is the textbook
  1 s + 2 s SYN-retransmit shape, i.e. the first SYN went unanswered too.
- **A bodiless reply ends with its headers.** A 1xx, 204 or 304, and any reply to HEAD, carry no body (RFC 9112
  §6.3). `SecureHttpClient` read one as a body: with no Content-Length that meant reading until the server closed,
  which on our kept-alive connection waited out the 15 s timeout and returned `ERR_TIMEOUT`. KOSync servers built on
  Spring (BookLore, grimmory) answer with exactly that 204, so a sync that worked was reported as a network
  error. The status now returns at once with an empty body and the connection stays usable; interim 1xx replies are
  read past to the final one (`hasNoBody()`, `isInterim()` in `SecureHttpClient.cpp`; Free-Ink PRs #65 and #212).
- **Certificate dates are checked only against an authoritative clock.** A clock inside the roots'
  window (2025-2037) loads the trust store, but that says nothing about a server's own certificate,
  which may be days old. The X4 has no RTC: after a cold boot its clock is the epoch saved in NVS,
  after a wake or restart that epoch plus the LP timer's count, and either can be days behind.
  KOReader sync, which never falls back unverified, then reads a freshly renewed certificate as not
  yet valid: `sync.crosspointreader.com`'s current one has notBefore 2026-11-12 01:09 UTC, and
  issue #766 (X4 only, "connect failed" with 55 KB contiguous heap, from 2026-11-13, cured by a full
  install, which erases NVS) fits that; not yet reproduced on a device. Its NTP freshness check did
  not help, since it measured the time since the last sync against that same clock. So every
  verified request (KOReader sync and HttpDownloader alike) waives notBefore/notAfter, and nothing
  else, unless the clock came from NTP in this boot or a hardware RTC
  (`clock_authority::datesAuthoritative()`, `HalClock::waiveCertificateDates()`), and the KOReader
  paths run SNTP first for such a clock (`HalClock::syncNtpIfSoftwareClock()`; not again within
  10 minutes of a failure). The release log says so once per server and clock source:
  `[CLK] TLS host:443: certificate dates not checked (chain and name are): the clock, epoch N, is
  restored from NVS`; `[TLS] Ignoring certificate date error -150 at epoch N` when it mattered.

### Wi-Fi session end after the web server (X4 Pro, Murphy, 2026-11-27)

File Transfer and Calibre Wireless without the restart (`perf/wifi-exit-web-server`), internal free / largest block
at the session end. Every exit carried on, and `ServerSession` read again what a web session can change:

| Exit (log tag) | X4 Pro | Murphy |
|---|---|---|
| File Transfer, a file uploaded and deleted (`WEBACT`) | 168,432 / 65,524, Home | 169,148 / 69,620, Home |
| The theme and the timezone set through `/api/settings`, a language pack uploaded (`WEBACT`) | 167,540 / 65,524, Home in the new theme, zone and language | 168,444 / 69,620, same |
| A second session in the same boot (`WEBACT`) | 167,100 / 65,524 | 167,160 / 69,620 |
| The hotspot, a host joined and served (`WEBACT`) | 161,232 / 65,524, Home; 192.168.4.1 gone at once, a later join worked | 160,988 / 65,524, same |
| Calibre Wireless, Back (`CAL`) | 163,124 / 65,524, File Transfer on its Calibre row | 163,724 / 65,524, same |

- **The re-read is cheap.** The language took 5 ms (Murphy) to 35 ms (X4 Pro), or 246-277 ms when the uploaded
  pack was new and its flash slot was built; the UI look and the timezone 0-5 ms, the plugins 4-29 ms, the SD font
  rediscovery 37-58 ms.
- A screenshot during File Transfer returns the QR frame on these boards now: their framebuffers stay.
- **Sleep from a running File Transfer takes the direct sleep path now, and it holds.** With the framebuffers kept,
  `enterDeepSleep()` no longer restarts into sleep: the sleep path drops the radio, the server screen exits, and the
  sleep picture is drawn. A long press of Power slept straight from File Transfer joined (X4 Pro, Murphy) and in
  hotspot mode (X4 Pro); each woke normally on a press, with a clean `CMD:BOOTLOG`.

### Wi-Fi session end on an ESP32-S3 (Picco, 2026-11-27)

Measured for the in-place return after a Wi-Fi screen (`wifi_session::end()`,
`docs/superpowers/specs/2026-11-26-wifi-exit-without-reboot-design.md`). All figures are the internal heap
(`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`), free / largest block.

- **The first session costs about 18.8 KB once, and it is not a leak.** A heap map (`heap_caps_walk()` plus the
  task list, throwaway branch `spike/wifi-heap-map`) before Wi-Fi and after each session: 182,452 / 139,252 at
  boot, 163,636 / 90,100 after one join. 15,184 B of the new blocks are three tasks that start with the first
  radio-on and are never deleted: lwIP's `tiT` (4,864 B stack block), ESP-IDF's event loop `sys_evt` (4,864) and
  Arduino's `arduino_events` (4,352), each with a 368 B control block. `esp_netif_deinit()` is not supported, so
  they live until the next restart by design. The other ~3.5 KB are about 40 small blocks (16-640 B).
- **The largest block drops by 49 KB because of where those blocks sit.** The three stacks land at the head of the
  largest free region, and a dozen small long-lived blocks (16-104 B) sit further into it with free gaps between
  them. The free memory is still there, split up. It does not get worse: the largest block read 90,100 after every
  later session.
- **Later sessions keep one 28-byte block each** (32 B with the heap header), a join with an HTTPS request
  (api.github.com, TLS 1.3) as much as a join alone; the first HTTPS session also kept 224 B once. The 28-byte
  block's owner is not identified (heap tracing is compiled out). At 32 B a session an S3 needs thousands of
  sessions in one boot to reach the minimums.
- **Every migrated screen returned in the same boot**, PR 1 at c49ba72ce:

  | Screen (log tag) | At the session end | Back on |
  |---|---|---|
  | Sync Time (`NTP`) | 131,860 / 57,332 | Clock settings |
  | Detect Timezone (`TZ`) | 131,264 / 53,236 | Clock settings |
  | Wi-Fi Networks (`SET`) | 133,080 / 57,332 | Settings, same tab |
  | Weather (`WEATHER`) | 162,704 / 57,332 | Home |
  | KOReader account check (`KOAuth`) | 130,456 / 59,380 | KOReader settings, Settings below |
  | KOReader sync from a book (`KOSync`) | 163,012 / 59,380 | the book, same page |
  | OPDS browser (`OPDS`) | 163,708 / 90,100 | File Transfer, OPDS row |
  | Join backed out, then OPDS from the mode picker (`NETMODE`) | 161,020 / 86,004 | the OPDS browser |

  Five KOReader syncs in a row ended at 163,012, 162,912, 162,888, 162,868 and 162,828, the largest block at
  59,380 each time, and a Weather join after them fetched normally. The lowest readings (130,456 free, 53,236
  largest) are about twice the minimums (67,584 / 24,576).
- **The three ways out that still restart behaved.** Weather left while the background sync worker held the radio
  logged `the background sync holds the radio; restarting` and restarted to Home with no panic. A long press of
  Power on Detect Timezone with the radio up went to sleep without a restart or a second teardown, and woke
  normally. A test build with the free minimum at 500,000 logged `[ERR] ... under the minimums 500000/24576;
  restarting` and restarted into Clock settings.

## Retired hypotheses

Each of these was believed, acted on, and disproven. Do not re-propose without new evidence.

| Hypothesis | What killed it |
|---|---|
| The slow sync is the network or the server | Server measures 47 ms TTFB, consistently, from a desktop; reused-session requests on the device are always fast |
| Adding the missing ISRG roots will shorten the chain walk | It did not. wolfSSL validates every presented certificate. The roots are still correct to ship — they are insurance for when LE drops the X1 cross-sign — but they buy no time |
| `esp_wifi_scan_get_ap_num()` can tell a timing miss from probe suppression | The driver retains no records from a connect scan; it reads 0 even on success |
| `min=0 / max=120` gives a 120 ms per-channel dwell | `min` is the floor. With `min=0` the sweep was not waiting on a quiet channel at all |
| The 80 ms dwell caused the `NO_AP_FOUND` misses | The dwell number was never in effect either way, which is also why 80 vs 120 made so little difference when tested |
| The idle governor downclocks the CPU mid-sync and breaks WiFi | `HalPowerManager::setPowerSaving()` already refuses to downclock while `WiFi.getMode() != WIFI_MODE_NULL` |
| `WiFi.setSleep(WIFI_PS_NONE)` races the core's `STA_START` handler | It does not: `setSleep` stores `_sleepEnabled` even before the STA starts, and the `STA_START` handler applies `WiFi.getSleep()` — our value either way |
| The SSID has only one AP, so AP selection never mattered (claimed from a desktop scan) | Wrong, and the device said so: it sees a second PYSY AP on channel 1 at -91 dBm, a different vendor's OUI, invisible from the desktop. The "which node do we pick" problem is real |
| The AP's 2.4 GHz radio runs at reduced transmit power (a 9 dB deficit against its own 5 GHz radio, measured from a desktop) | Not supported from where the reader sits: it reads the channel-11 AP at -65 dBm, a healthy link. The desktop gap says something about the desktop's position, not the AP's power |
| Pinning a freshly-measured BSSID is safe because the risk was staleness | The pin failed on a BSSID seen 1 ms earlier. The channel is the part that pays; the address is not |
| Narrowing the connect to the probed *channel* (no BSSID) avoids that | It does not. The channel-restricted `begin()` also returned `NO_AP_FOUND` while the probe line directly above it reported the AP at -72 dBm. **Anything we narrow gets rejected**; only the driver's own unhinted sweep associates |
| A real `active.min` on the sweep will reduce the `NO_AP_FOUND` rate | The opposite was observed. With `min=0` a connect took ~3.5 s with at most one miss; with `min=max=120` two consecutive runs took 7.5 s with two and three misses. Equal `min`/`max` also removes the driver's room to extend a dwell when it *does* hear something. Reverted |
| Verify-then-fall-back-insecure provides partial security | It provides none against an on-path attacker — the bad certificate is accepted on the retry — while paying for the chain walk twice |

## Open

| Question | What would settle it |
|---|---|
| What stalls the *first* request on a fresh connection (5 s with no status line, then a lost SYN)? | Not reproduced since. Two clean runs showed `ttfb=710/762 ms` on a fresh connection and `25/62 ms` reused, with no failures — but the first trace measured `ttfb` from the top of `transact()`, folding the ~595 ms handshake in, so the real figure is ~115 ms. Corrected; the next occurrence will read straight |
| Why does the association itself take 6 s and 2-3 `NO_AP_FOUND` sweeps now, where it took 3.5 s and at most one? | RSSI moved from -63..-69 dBm to -72..-73 dBm across the same sessions, at the same desk, while our scan-budget change was also in. Both variables moved at once. The revert isolates the second; if 7.5 s connects persist, it is the link |
| Is the intermittent request failure ours or the path's? | One run pointed at a different kosync server (`sync.koreader.rocks`, already in the curated roots). If it follows the device, it is ours |

## The home-channel probe: tried, measured, reverted

The idea was sound on paper: a full sweep gives the AP one ~120 ms window out of ~1560 ms, so scan
its channel alone for longer and hand the driver a candidate. It was built, measured over four
device runs, and removed.

What the runs showed, in order:

1. The probe listened for 101 ms, not the 300 requested — the parameter model above was wrong.
2. With the dwell corrected it **did** find the AP (`1 AP(s) on channel, 1 for 'PYSY' at -69 dBm`),
   and the BSSID-pinned `begin()` that followed returned `NO_AP_FOUND` after 1939 ms, while the
   unhinted `begin()` after it associated with that same BSSID 58 ms later.
3. Dropping the BSSID and restricting only the channel changed nothing: `NO_AP_FOUND` again, with
   the probe reporting the AP at -72 dBm one line above.

Three narrowing strategies, three rejections, against a driver-run sweep that succeeds. Whatever
this mesh answers, it answers a broadcast sweep and not a targeted attempt — so the probe was
reverted along with the scan-budget change, leaving the sweep exactly as it was.

The stop rule was set before the last flash and honoured: *if a channel-restricted attempt reports
no AP while the probe above it saw one, stop narrowing.* Writing that down in advance is what kept
this from becoming a fourth variation.

## Process traps hit during this work

- **`scripts/gen_i18n.py` run by hand adds ~30 KB.** The PlatformIO hook passes `--strip-unused`;
  a manual run does not, and its up-to-date check only compares *yaml* mtimes — it never notices
  that the set of referenced `STR_*` keys changed. A stale unstripped table then survives the next
  build silently. Use `--strip-unused --force`, and never trust a flash delta measured across a
  manual regeneration.
- **The clang-format pre-commit hook `git add`s whole files.** Staging one hunk with
  `git apply --cached` and committing put the *other*, unstaged hunk in the same commit. Use
  `--no-verify` with a manual `clang-format --dry-run --Werror` when splitting a file across
  commits.
- **PlatformIO caches library objects against header changes it cannot see.** After changing
  `user_settings.h` via the patch script, delete `.pio/build/<env>/lib*/Arduino-wolfSSL` before
  measuring, or the old object is silently relinked.
