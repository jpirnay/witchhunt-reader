# Protected content (DRM)

Status, 2026-11-17 night: D1 (the reader opens protected books) is merged (#821) and has run on the X4 Pro and the X3.
The listing change the Adobe plugin needs is on `fix/web-list-dot-folders` (pushed, no PR). D2 and D3 are not started.
This file replaces the September plan on `feat/protected-content-phase0` (`docs/protected-content-plan.md` and
`docs/protected-content-handover-2026-09-18.md` there).

Owner decisions of 2026-11-17:
- Adobe ADEPT is supported. It is what Libby, Onleihe and the other loan services mostly use, so the Plugin Hub's
  `protected-content` plugin is meant to run here.
- LCP: Free-Ink is asked whether other firmwares may use `lcp.freeink.org` (see LCP below).
- No second library: decryption runs on our own `ZipFile` and our own uzlib inflate (`lib/InflateReader`), never on
  the SDK's vendored miniz or its own zip reader. (In the end none of the SDK library is linked.)
- D2's cipher must be the fastest and leanest available and must not change how reading feels (see D2).
- `/api/files` lists folders as upstream does (for the plugin, see D3).

## Handover

Where things stand:
- `master` has D1: `ZipFile` decrypts beneath our uzlib, `lib/BookProtection` (the manifest `protect.bin`, AES on the
  chip's peripheral, the book key and device secret ported from upstream), `HalClock::loanNow()`, the reader's
  messages, `POST /api/book-key` and `deviceId`. Cost on the C3 at merge: +18,114 B flash, +312 B static RAM; the C3's
  map stayed at 49 code and 44 rodata pages (10.4 KB left in the last code page then).
- `fix/web-list-dot-folders` (7e5b6f926, master merged in): `/api/files` lists a dot folder named by its path whatever
  **Show Hidden Files** says. Checked on the X3 (below). It needs its PR.
- Nothing obtains real keys yet: the Adobe plugin needs D3.

Next, in this order unless the owner reorders:
1. The PR for `fix/web-list-dot-folders`.
2. The `STR_PROTECTED_*` messages in the other languages (release translation pass; English only so far, so a German
   UI shows them in English).
3. D3 (the Adobe plugin): measure the C3 cost of wolfSSL's RSA private-key operations and PKCS#12 first.
4. D2 (the cache at rest): measure the cipher first.
5. A real loan end to end, once D3 runs: needs the owner's Adobe ID and a library loan; activating may use one of the
   account's device slots, so only with the owner's go.

Tools and procedures:
- `tools/make_protected_epub.py in.epub out.epub --key <hex> [--scheme lcp]` makes Adobe-shaped (AES-128, deflated,
  no sizes) and LCP-shaped (AES-256, sizes given) books. The device test books and keys:
  `~/.local/state/witchhunt-work/protected-content/` (moby-adobe, moby-lcp, moby-nokey; Adobe key
  `2b7e151628aed2a6abf7158809cf4f3c`, LCP key `603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4`).
- A device run: post the keys with `curl` to `/api/book-key` (base64 key) after a Wi-Fi join (`ACT:FILE_TRANSFER`,
  Confirm), open the books with `tools/devctl.py --script` (`ACT:READER:<path>@<spine>:<page>`, `page:` dumps), compare
  the dumps with the plain book's. Leaving File Transfer reboots the C3: start the next script after the boot. The
  steps files and logs of both runs: `x3run/` and `run/` in that folder; backups, gate scripts and READMEs in
  `~/.local/state/witchhunt-device-backups/2026-11-17-x4pro-protected/` and `2026-11-17-x3-protected/`.
- Host tests: `test/book_protection` (OpenSSL stands in for `esp_aes` and wolfSSL), `EpubProtectedPipelineTest`,
  `ZipEncryptedEntryTest`.

Both device runs left the device secret (`devid/secret`) and probably the loan floor (`cptime/floor`) in NVS; firmware
that does not use them ignores both.

## D1: what is still open

- Device results, 2026-11-17, Moby-Dick chapter 1 (spine 3), the owner's settings on each board:
  - X4 Pro (a68dfdf1f): the Adobe and LCP copies give the plain book's page dumps byte for byte, the covers decrypt,
    the no-key copy shows its message. Adobe's first open builds the manifest in 2.3 s (it measures the text's sizes),
    LCP's in 0.26 s; a cached reopen registers in about 60 ms; the chapter builds in 5.94 s as LCP, 5.88 s plain.
  - X3 (master d81e79e07 + 7e5b6f926): page dumps identical to plain (Adobe pages 1-2, LCP page 1), the no-key copy
    shows its message. Manifest 2.95 s (Adobe), 0.37 s (LCP). Chapter build 4,631 ms Adobe, 4,499 ms LCP, 4,493 ms
    plain. Lowest free heap since boot 16,408 B after the Adobe build, 15,460 B after the plain one (the LCP build did
    not go lower); free heap at the first page's pre-render 63,984 B Adobe, 63,988 B plain, 63,156 B LCP.
- An `aes128-cbc` entry whose plaintext is not a DEFLATE stream fails here: the manifest build fails, or, for an image
  (which the build does not measure), its read. DeDRM's `ineptepub.py` takes such an entry as it is when inflating
  fails ("possibly not compressed by zip"). How often real books do this is not known; a real loan will tell.
  (`aes128-cbc-uncompressed` is handled.)
- No real Adobe or LCP book has been opened yet (no key source until D3).

## D2: the cache at rest

Owner rule (2026-11-17): the cipher must be the fastest and leanest available and must not change how reading feels.
Measure first, on the C3: a page turn and a chapter build of a protected book against the same book unprotected, plus
flash. If even the fastest cipher is noticeable, fall back to a cheap obfuscation (a keyed XOR stream, say) or leave
the cache plain, and say which in the release notes. The September numbers are the starting point: ChaCha20 through
wolfSSL 4.6 MB/s, a portable ChaCha20 1.8 MB/s, AES-128-CBC 1.5 MB/s; a 4 KB page record 1.1 ms. The C3 has an AES
peripheral too (D1 uses it through `esp_aes`): measure AES-CTR on it beside ChaCha20.

Like upstream, D1 writes the decrypted chapter to the cache (`spines/<bucket>/html_<n>.bin`) and the extracted
images. Carry over Phase 0 from `feat/protected-content-phase0` by hand (the branch is 3,345 commits behind and
conflicts in 12 files): `lib/CacheCipher` (ChaCha20 through wolfSSL, length-preserving and addressable by offset), the
storage scope that enciphers a protected book's cache files, and its host tests. Its keys come from the device secret
of D1 instead of a second store, and the `.rights` stand-in becomes `ZipFile::isProtected()`.

## D3: the Adobe plugin

The plugin for library loans (Libby, Onleihe) is the Plugin Hub's `protected-content` (Diirge, 1.0.5, served from
Railway, no repository, no license: never copy it into this repository). It fulfils the `.acsm` file a loan service
hands out: an ADEPT client in the page that activates the reader with Adobe's activation service and fetches a loan
from the distributor's ACS server. Adobe's service is DTS ByteBooks' now: `adeactivate.adobe.com` answers with a
`dtsbytebooks.com` certificate that names it, and the plugin links there for an account. No other Libby or Onleihe
plugin exists (Hub, sd-plugins, GitHub; 2026-11-17). Version 1.0.5 against this firmware:

- Every step needs `api.crypto` (random, sha1, sha256, aesenc/aesdec, rsadec, keygen, pubencrypt, sign, pkcs12); the
  plugin host throws. The page cannot use `crypto.subtle` instead, as `PluginHost.js.inc` says it can: `SubtleCrypto`
  exists only in a secure context, and the reader serves `http://` on a LAN address. Correct that comment with D3.
  Upstream's contract is `handleCrypto()` in its `CrossPointWebServer.cpp` (base64 fields in and out, PKCS#8 and SPKI
  DER keys, `sign` takes a 20-byte hash).
- It lists its own folder and `/.crosspoint` (`/api/files`) before anything else, looking for `content.key` in both.
  Both answered 403 while hidden files were off. `fix/web-list-dot-folders` lists them; on the X3 with hidden files
  off: the plugin folder, `/.crosspoint` and `/.crosspoint/plugins` list (no dot names), `System Volume Information`
  and `/CROSSP~1` stay 403, `/download` of the plugin's file 200, of `settings.json` and `wifi.json` 403.
- The rest is in place: the relay (four arguments; a 3xx comes back unfollowed and the plugin follows it), `fetchToSd`
  (http first, then https), `writeFile` of the `.rights` sidecar beside the book (where `/upload` may write) and of its
  account files in its folder, exact reads there, `/delete` with `path`, `deviceId`, and `/api/book-key`, which it
  calls before the download (the endpoint needs no book at that path yet). Adobe's certificate chains to USERTrust RSA,
  which the bundle carries (the chain has a cross-sign; `WOLFSSL_ALT_CERT_CHAINS` is on). The distributors' ACS hosts
  are not known until a real `.acsm`. `registerAction` only serves fulfilling and renewing by job.
- It keeps the account's license private key in plain base64 in `content.key` in its folder and reads it back with
  `/download`, so while File Transfer runs any client on the network can read it too, as on upstream.
- Not done: running the plugin's own page against a device. A jsdom run of the File Manager page did not load the
  plugin card (only `/api/web/status` was requested); the calls it makes were checked with `curl` instead.

`POST /api/crypto` needs wolfSSL's RSA private-key operations and PKCS#12, which `scripts/patch_wolfssl.py` removed to
save flash (`WOLFSSL_RSA_PUBLIC_ONLY`, `NO_PWDBASED`): measure that cost on the C3 first, and decide with the owner if
it is large (an S3-only `/api/crypto` behind a capability is one option).

## LCP

Upstream fulfils LCP loans through Free-Ink's service (`lcp.freeink.org`: `/enroll`, `/fulfill`, `/unlock`), and
enrolment needs a release key that only CrossPoint's official builds carry (`src/network/LcpDevice.cpp`). The SDK
keeps the production LCP profile off the device (`LcpLicense.h`), so the September plan's LCP on the device does not
work for real loans. Asked of Free-Ink on 2026-11-17: may another firmware's release builds use the service. No
answer recorded yet.

## What is lost, what is kept

The SDK commit the September branch pins (`d66c7a9e`, branch `feat/injectable-inflate`) was never pushed; it is not
in any clone here. The SDK fork branch `feat/content-protection-seams` (36cdcc8, 4e418ce: an injectable pull inflater
and entry index for `ProtectedBook`, two GCC 15 test fixes) is pushed but unused here; it is an offer to Free-Ink.
