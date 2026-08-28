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
| `[HTTP] GET host: conn=.. send ..ms/..B ttfb=..ms polls=.. hdr=..B attempts=.. rssi=.. ps=.. -> rc=..` | `SecureHttpClient::logRequestTrace` | Everything *after* the handshake. `ttfb=0` means nothing ever arrived |
| `[WIFI] Home-channel probe: ch=.. active ..ms -> N AP(s) on channel, M for 'SSID'` | `processHomeChannelProbe` | Did we hear the AP, and did anyone else answer on that channel |
| `[WIFI] EVT disconnected .. reason=201 (NO_AP_FOUND) .. bssid=00-00-.. rssi=-128` | event handler | An all-zero BSSID means the scan produced no candidate at all |

`ps=` is `WiFi.getSleep()`: 0 = `WIFI_PS_NONE`, 1 = `WIFI_PS_MIN_MODEM`.

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
  `sp_c32.c.o` (37,752 → 138,490 B). `WOLFSSL_RSA_PUBLIC_ONLY` would claw some back but does not
  compile in wolfSSL 5.7.2 (`implicit declaration of sp_3072_norm_56`).

### WiFi

- **`scan_time.active.min` is the dwell, not `max`.** A station leaves a channel after `min` ms and
  only stays as long as `max` once it has already heard an AP. Measured directly: a scan requesting
  `min=100 / max=300` returned in **101 ms**. Arduino's `WiFiScanClass` default for `min` is 100 ms
  and it passes that unless told otherwise (`setScanActiveMinTime`).
- **The driver keeps no scan records from an internal connect scan.**
  `esp_wifi_scan_get_ap_num()` returned 0 immediately after a *successful* association — so a 0
  after a failure carries no information.
- **A BSSID-pinned connect fails on this mesh where an unpinned one succeeds.** The probe saw the
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
| Pinning a freshly-measured BSSID is safe because the risk was staleness | The pin failed on a BSSID seen 1 ms earlier. The channel is the part that pays; the address is not |
| Verify-then-fall-back-insecure provides partial security | It provides none against an on-path attacker — the bad certificate is accepted on the retry — while paying for the chain walk twice |

## Open

| Question | What would settle it |
|---|---|
| What stalls the *first* request on a fresh connection (5 s with no status line, then a lost SYN)? | The new `[HTTP] ... ttfb=.. polls=..` trace. `ttfb=0` with high polls = nothing ever came back; a non-zero `ttfb` with short `hdr` = it answered and we lost the rest |
| Does the channel-only fast connect work on this mesh? | One flash. Look for `WiFi.begin -> SSID (ch=N only, fast scan)` followed by a prompt associate |
| Does a real `active.min` on the sweep reduce the `NO_AP_FOUND` rate? | Several connects. The failures were never frequent, so this needs a run of them |
| Is the intermittent failure ours or the path's? | One run pointed at a different kosync server (`sync.koreader.rocks`, already in the curated roots). If it follows the device, it is ours |

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
