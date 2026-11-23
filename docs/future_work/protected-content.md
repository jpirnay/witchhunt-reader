# Protected content (DRM)

Status, 2026-11-20: D1 (the reader opens protected books, #821), the listing the Adobe plugin needs (#828) and D3
(`/api/crypto` for the Adobe plugin, #857) are merged and have run on devices. Open: a real Adobe account end to end,
the messages' translations, LCP (asked of Free-Ink). D2 (the cache at rest) is built.
This file replaces the September plan (`docs/protected-content-plan.md` and
`docs/protected-content-handover-2026-09-18.md` on the branch `feat/protected-content-phase0`). That branch is
deleted; D2 took its cache cipher over.

Owner decisions:
- Adobe ADEPT is supported (2026-11-17). It is what Libby, Onleihe and the other loan services mostly use, and what
  shops such as Thalia sell as "Adobe DRM", so the Plugin Hub's `protected-content` plugin is meant to run here.
- LCP: Free-Ink is asked whether other firmwares may use `lcp.freeink.org` (see LCP below).
- No second library: decryption runs on our own `ZipFile` and our own uzlib inflate (`lib/InflateReader`), never on
  the SDK's vendored miniz or its own zip reader.
- D2's cipher must be the fastest and leanest available and must not change how reading feels (see D2).
- `/api/files` lists folders as upstream does (2026-11-17).
- `/api/crypto` runs the Adobe plugin's crypto on the device, on all boards (2026-11-20), with the RSA private-key code
  the flow does not use cut away. The alternative, a JavaScript crypto library in the browser's plugin host (no
  firmware flash), was declined: it would be a second crypto implementation beside our wolfSSL.

## What is in master

- D1 (#821): `ZipFile` decrypts beneath our uzlib (`METHOD_ENCRYPTED` stats, a registry of two protected archives),
  `lib/BookProtection` (`encryption.xml` into `<cache>/protect.bin`, AES on the chip's peripheral, Adobe's
  `aes128-cbc-uncompressed`), upstream's book key and device secret (`<book>.epub.key`, NVS `devid/secret`),
  `HalClock::loanNow()` (NVS `cptime/floor`), the reader's messages, `POST /api/book-key`, `deviceId`. Cost on the C3:
  +18,114 B flash, +312 B static RAM. The key and the plugin's licence (`<book>.epub.rights`, which it derives the key
  from again after a flash erase) move and go with their book (`SidecarFiles`), as upstream's do.
- #828: `/api/files` lists a dot folder named by its path whatever **Show Hidden Files** says.
- D3 (#857): `POST /api/crypto` (upstream's handler, #3114, on the SDK's `WolfsslCrypto`, only that file built:
  `lib/PluginCrypto`), `api.crypto` in the plugin host, wolfSSL with its RSA private-key paths, password-based
  encryption, 3DES and RC2 back in, and a source patch (`CROSSPOINT_SP_RSA_PUBLIC_ONLY` in `scripts/patch_wolfssl.py`,
  gated on wolfSSL 5.7.2) that keeps SP math's 2048 to 4096-bit RSA code for public-key operations only. Cost against
  master b1a0fe019: C3 +38,212 B flash, +8 B static RAM (map unchanged at 50 code and 44 rodata pages, 19.3 KB left in
  the last code page); X4 Pro +35,084 B. The Plugins page marks `protected-content` "partly runs here" and offers it.

Measured on devices (records in `~/.local/state/witchhunt-device-backups/`, scripts in
`~/.local/state/witchhunt-work/protected-content/`):
- Reading (2026-11-17, Moby-Dick chapter 1, the owner's settings): Adobe-shaped and LCP-shaped copies give the plain
  book's page dumps byte for byte on the X4 Pro and the X3, covers decrypt, a copy with no key shows its message. First
  open (the manifest) 2.3 s Adobe, 0.26 s LCP on the X4 Pro; 2.95 s and 0.37 s on the X3; a cached reopen about 60 ms.
  Chapter build as LCP 5.94 s against 5.88 s plain (X4 Pro); 4,631 ms Adobe, 4,499 ms LCP, 4,493 ms plain (X3), the
  X3's lowest free heap no lower than the plain book's.
- `/api/crypto` (2026-11-20, `crypto_check.py`, every op against OpenSSL): 18 of 18 on the Murphy and on the X3. X3:
  RSA-1024 `keygen` 0.54 to 0.88 s, `sign` 467 ms, `rsadec` 466 ms, `pubencrypt` to RSA-4096 153 ms, `pkcs12` (RC2 and
  3DES bags) 225 ms, no watchdog reset, about 110 KB of heap free during the ops. TLS after the trim, through the
  relay, on both boards: `adeactivate.adobe.com` (ECDHE-RSA) and `raw.githubusercontent.com` verify (`insecure=0`).
- The plugin end to end against a mock ADEPT service (2026-11-20, Murphy, `adept-mock/adept_mock.mjs`: the plugin's
  page in jsdom, its relay calls to Adobe's and the distributor's hosts answered in process, its download turned into
  an upload): 22 checks pass. The device encrypts the sign-in to the authentication certificate, makes both key pairs
  and wraps their private halves, opens the RC2/3DES PKCS#12, signs Activate, InitLicenseService and Fulfill (each
  signature verified over the request's canonical SHA-1), unwraps the content key with `rsadec`, and the plugin stores
  it with the loan end. The loan opens on the reader, its pages byte-identical to the plain copy's.

Tools: `tools/make_protected_epub.py in.epub out.epub --key <hex> [--scheme lcp]` makes Adobe-shaped and LCP-shaped
books; the test books and their keys are in `~/.local/state/witchhunt-work/protected-content/`. Host tests:
`test/book_protection`, `EpubProtectedPipelineTest`, `ZipEncryptedEntryTest`; web: `plugin_host_test` (`api.crypto`),
`plugins_catalog_test`. A device run leaves the device secret and probably the loan floor in NVS; firmware that does not
use them ignores both.

## Open

1. **A real account end to end.** Needs the owner's Adobe ID; Adobe's free sample eBook library still hands out `.acsm`
   loan tokens (2026-11-20, fulfilled by DTS ByteBooks' content server), so no library card is needed. Only with the
   owner's go: an activation may take one of the account's device slots. What a real run settles:
   - the distributors' fulfilment hosts must chain to a root in our bundle (`lib/SecureNet/include/CrossPointRoots.pem`).
     A shop's or library's `.acsm` names its host (`operatorURL`), so this can be checked from the file alone, before
     any activation (Thalia, Onleihe, OverDrive: unknown yet);
   - whether Adobe's signing bundle needs 3DES (about 4.5 KB on the C3) as well as RC2;
   - an `aes128-cbc` entry whose plaintext is not a DEFLATE stream fails here (the manifest build, or an image's read);
     DeDRM's `ineptepub.py` takes such an entry as it is. How often real books do this is not known.
2. **The `STR_PROTECTED_*` messages in the other languages** (release translation pass; English only so far).
3. **D2, the cache at rest: built** (branch `feat/cache-cipher-at-rest`). A protected book's cache folder is enciphered:
   its extracted chapters, laid-out pages, footnotes, search snippets, pictures, pixel caches and the book's own fonts.
   `lib/CacheCipher` (ChaCha20 through wolfSSL, by file offset) under `HalStorage`: a folder with the marker
   `cipher.bin` enciphers every file opened in it, for whoever opens it, behind a 512-byte header with a random nonce
   that `HalFile` hides (`docs/file-formats.md`, "Enciphered cache folders"). The key comes from the device secret and
   the marker's salt (`lib/BookProtection/CacheAtRest.cpp`); without it nothing in the folder opens. A folder made
   before is emptied on the book's next open or the next look at it, keeping the reading state and `protect.bin`. The
   web download and the serial transfers hand the files out enciphered (`HalStorage::openRaw`).

   Measured 2026-11-23 on the X3 at 160 MHz (`pio run -e bench_crypto`, 64 passes a row):

   | Cipher | 2 KB | 4 KB | 64 KB |
   |---|---|---|---|
   | `CacheCipher` (device path) | 0.56 ms | 1.11 ms | 17.4 ms (3.7 MB/s) |
   | wolfSSL ChaCha20, raw | 0.45 ms | 0.89 ms | 14.2 ms (4.6 MB/s) |
   | wolfSSL AES-128-CBC decrypt | 1.34 ms | 2.67 ms | 42.6 ms (1.5 MB/s) |
   | wolfSSL AES-256-CTR | 1.72 ms | 3.42 ms | 54.6 ms (1.2 MB/s) |

   In the reader, on the X3 (2026-11-23, Moby-Dick's 62-page chapter 2, 14 pt, cold build, three runs each): the
   protected copy builds in 5,935 ms against 5,809 ms for the plain one, +126 ms (+2.2%); on master the two build
   alike (5,827 against 5,824 ms), so all of it is the cache cipher. Of the first version's +330 ms, the 16-byte
   header (content off the sector grid, so SdFat moved every file through its one-sector cache) cost 86 ms and
   writing through a 512-byte chunk (one sector per card write) 118 ms; the header is a sector now and a large write
   from RAM is enciphered where it is. Pure ChaCha is about 30 ms of a build's reads; the open file's last keystream
   block saves 34 ms. Page turns (a page record read from the section file and drawn) are unchanged, 40 to 55 ms
   either way. A protected book's cache made by master was emptied and enciphered on the first open, keeping its
   progress and manifest; one with files of an older layout had them removed and made again.

   The wrapper takes about a quarter longer than raw wolfSSL (3.7 against 4.6 MB/s), and the chapter's remaining cost
   is mostly card time. The C3's AES peripheral (D1 uses it) is untried here.

   Cost on the X3/X4 build against master (same worktree): flash +1,180 B, static RAM +288 B (the table of the last
   four cache folders, in the HalStorage singleton), IRAM unchanged; an open enciphered file takes about 120 B more
   heap (its cipher and keystream block). The new code is about 5.5 KB; linking `lib/CacheCipher` shrank other code
   by about 4.6 KB in the same link, as it did with the library alone (2026-11-23), so the net figure depends on layout.

   Left open: covers and thumbnails a protected book's cache shares with Home stay in the folder, enciphered, so a
   card moved to another reader shows the placeholder cover; Quick Resume's `sleep_frame.bin` and screenshots stay
   plain (owner, 2026-11-23); a book font copied into a card-backed flash-area image (`flash-area.img`, S3 boards
   without a usable partition) is not enciphered there.
4. **Books downloaded with Adobe Digital Editions.** Their licence is inside the EPUB (`META-INF/rights.xml`). The plugin
   derives a key only from a `<book>.epub.rights` beside the book, so such a book needs that file beside it first;
   fetching the `.acsm` again from the shop is the simple route.

The plugin keeps the Adobe account's license private key in plain base64 in `content.key` in its folder and reads it
back with `/download`, so while File Transfer runs any client on the network can read it too, as on upstream.

## LCP

Upstream fulfils LCP loans through Free-Ink's service (`lcp.freeink.org`: `/enroll`, `/fulfill`, `/unlock`), and
enrolment needs a release key that only CrossPoint's official builds carry (`src/network/LcpDevice.cpp`). The SDK keeps
the production LCP profile off the device (`LcpLicense.h`). Asked of Free-Ink on 2026-11-17: may another firmware's
release builds use the service. No answer recorded yet. The reader already opens LCP-shaped books given the key (D1).

## What is kept elsewhere

The SDK fork branch `feat/content-protection-seams` (36cdcc8, 4e418ce: an injectable pull inflater and entry index for
`ProtectedBook`, two GCC 15 test fixes) is pushed but unused here; it is an offer to Free-Ink. The September branch's
SDK pin (`d66c7a9e`) was never pushed and is lost.
