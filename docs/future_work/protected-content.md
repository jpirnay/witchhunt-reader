# Protected content (DRM)

Status: D1 (the reader opens protected books, #821), the listing the Adobe plugin needs (#828), D3 (`/api/crypto` for
the Adobe plugin, #857) and D2 (a protected book's cache enciphered at rest, #880;
[design/cache-cipher.md](../design/cache-cipher.md)) are merged and have run on devices. Open: a real Adobe account end
to end, the messages' translations, books from Adobe Digital Editions, LCP (asked of Free-Ink), and three decisions.

Owner decisions:
- Adobe ADEPT is supported. It is what Libby, Onleihe and the other loan services mostly use, and what shops such as
  Thalia sell as "Adobe DRM", so the Plugin Hub's `protected-content` plugin is meant to run here.
- No second library: decryption runs on our own `ZipFile` and our own uzlib inflate (`lib/InflateReader`), never on
  the SDK's vendored miniz or its own zip reader.
- `/api/files` lists folders as upstream does.
- `/api/crypto` runs the Adobe plugin's crypto on the device, on all boards, with the RSA private-key code the flow
  does not use cut away. The alternative, a JavaScript crypto library in the browser's plugin host (no firmware
  flash), was declined: it would be a second crypto implementation beside our wolfSSL.

## What is in master

Background for the items below. The endpoints are in [webserver-endpoints.md](../webserver-endpoints.md)
(`POST /api/book-key`, `POST /api/crypto`), the plugin side in [sd-plugins.md](../sd-plugins.md).

- D1 (#821): `ZipFile` decrypts beneath our uzlib (`METHOD_ENCRYPTED` stats, a registry of two protected archives),
  `lib/BookProtection` (`encryption.xml` into `<cache>/protect.bin`, AES on the chip's peripheral, Adobe's
  `aes128-cbc-uncompressed`), upstream's book key and device secret (`<book>.epub.key`, NVS `devid/secret`),
  `HalClock::loanNow()` (NVS `cptime/floor`), the reader's messages, `POST /api/book-key`, `deviceId`. Cost on the C3:
  +18,114 B flash, +312 B static RAM. The key and the plugin's licence (`<book>.epub.rights`, which it derives the key
  from again after a flash erase) move and go with their book (`SidecarFiles`, #858), as upstream's do.
  The loan floor rises only from a clock that came from NTP or the hardware RTC (or was saved from one), never from a
  hotspot client's browser time: NVS `halclock/clienttime` marks such a time until the next NTP sync, since it also
  reaches the RTC and the epochs saved at sleep. An NTP sync sets the floor to its own time, lowering one a wrong clock
  left in the future (`clock_authority::raisedLoanFloor()`, `loanFloorAfterNtp()`, test/clock_authority).
- #828: `/api/files` lists a dot folder named by its path whatever **Show Hidden Files** says.
- D3 (#857): `POST /api/crypto` (upstream's handler, #3114, on the SDK's `WolfsslCrypto`, only that file built:
  `lib/PluginCrypto`), `api.crypto` in the plugin host, wolfSSL with its RSA private-key paths, password-based
  encryption, 3DES and RC2 back in, and a source patch (`CROSSPOINT_SP_RSA_PUBLIC_ONLY` in `scripts/patch_wolfssl.py`,
  gated on wolfSSL 5.7.2) that keeps SP math's 2048 to 4096-bit RSA code for public-key operations only. Cost against
  master b1a0fe019: C3 +38,212 B flash, +8 B static RAM (map unchanged at 50 code and 44 rodata pages, 19.3 KB left in
  the last code page); X4 Pro +35,084 B. The Plugins page marks `protected-content` "partly runs here" and offers it.
- D2 (#880): [design/cache-cipher.md](../design/cache-cipher.md).

Measured on devices (records in `~/.local/state/witchhunt-device-backups/`, scripts in
`~/.local/state/witchhunt-work/protected-content/`):
- Reading (Moby-Dick chapter 1, the owner's settings): Adobe-shaped and LCP-shaped copies give the plain book's page
  dumps byte for byte on the X4 Pro and the X3, covers decrypt, a copy with no key shows its message. First open (the
  manifest) 2.3 s Adobe, 0.26 s LCP on the X4 Pro; 2.95 s and 0.37 s on the X3; a cached reopen about 60 ms. Chapter
  build as LCP 5.94 s against 5.88 s plain (X4 Pro); 4,631 ms Adobe, 4,499 ms LCP, 4,493 ms plain (X3), the X3's
  lowest free heap no lower than the plain book's.
- `/api/crypto` (`crypto_check.py`, every op against OpenSSL): 18 of 18 on the Murphy and on the X3. X3: RSA-1024
  `keygen` 0.54 to 0.88 s, `sign` 467 ms, `rsadec` 466 ms, `pubencrypt` to RSA-4096 153 ms, `pkcs12` (RC2 and 3DES
  bags) 225 ms, no watchdog reset, about 110 KB of heap free during the ops. TLS after the trim, through the relay, on
  both boards: `adeactivate.adobe.com` (ECDHE-RSA) and `raw.githubusercontent.com` verify (`insecure=0`).
- The plugin end to end against a mock ADEPT service (Murphy, `adept-mock/adept_mock.mjs`: the plugin's page in jsdom,
  its relay calls to Adobe's and the distributor's hosts answered in process, its download turned into an upload): 22
  checks pass. The device encrypts the sign-in to the authentication certificate, makes both key pairs and wraps their
  private halves, opens the RC2/3DES PKCS#12, signs Activate, InitLicenseService and Fulfill (each signature verified
  over the request's canonical SHA-1), unwraps the content key with `rsadec`, and the plugin stores it with the loan
  end. The loan opens on the reader, its pages byte-identical to the plain copy's.

Tools: `tools/make_protected_epub.py in.epub out.epub --key <hex> [--scheme lcp]` makes Adobe-shaped and LCP-shaped
books; the test books and their keys are in `~/.local/state/witchhunt-work/protected-content/`. Host tests:
`test/book_protection`, `test/cache_cipher`, `EpubProtectedPipelineTest`, `ZipEncryptedEntryTest`; web:
`plugin_host_test` (`api.crypto`), `plugins_catalog_test`. A device run leaves the device secret and probably the loan
floor in NVS; firmware that does not use them ignores both.

## Open

1. **A real account end to end.** Needs the owner's Adobe ID; Adobe's free sample eBook library still hands out `.acsm`
   loan tokens (fulfilled by DTS ByteBooks' content server), so no library card is needed. Only with the owner's go:
   an activation may take one of the account's device slots. What a real run settles:
   - the distributors' fulfilment hosts must chain to a root in our bundle (`lib/SecureNet/include/CrossPointRoots.pem`).
     A shop's or library's `.acsm` names its host (`operatorURL`), so this can be checked from the file alone, before
     any activation (Thalia, Onleihe, OverDrive: unknown yet);
   - whether Adobe's signing bundle needs 3DES (about 4.5 KB on the C3) as well as RC2;
   - an `aes128-cbc` entry whose plaintext is not a DEFLATE stream fails here (the manifest build, or an image's read);
     DeDRM's `ineptepub.py` takes such an entry as it is. How often real books do this is not known.
2. **The `STR_PROTECTED_*` messages in the other languages** (release translation pass; English only so far,
   `lib/I18n/translations/english.yaml`).
3. **Books downloaded with Adobe Digital Editions: support them (decided 2026-11-23).** Their licence is inside the
   EPUB (`META-INF/rights.xml`). The plugin derives a key only from a `<book>.epub.rights` beside the book, so such a
   book does not open until that file is there; fetching the `.acsm` again from the shop works today. Plan: the
   firmware writes `<book>.epub.rights` from the book's `rights.xml` when the book has none (a `ZipFile` read;
   `SidecarFiles` already moves the file with its book). The other route, the plugin doing it through `/download` and
   `writeFile`, would change Plugin Hub's code, not ours. Next step: where the firmware notices such a book (the
   protection's first open, which already reads `encryption.xml`, is the candidate) and a host test with an
   ADE-shaped book from `tools/make_protected_epub.py`.
4. **The plugin's licence key at rest: encipher it (decided 2026-11-23).** The plugin keeps the Adobe account's
   licence private key in plain base64 in `content.key` in its folder and reads it back with `/download`, so while
   File Transfer runs any client on the network can read it too, as on upstream. Decided: the key is enciphered at
   rest with ChaCha20 under a key bound to the device, as the cache is ([design/cache-cipher.md](../design/cache-cipher.md)),
   so `/download` hands any client only ciphertext that is useless off this device. To design: how the plugin then
   uses the key. The key only feeds `rsadec` in `POST /api/crypto`, so the device can decipher it there and the
   plaintext never leaves it; that changes upstream's plugin flow, and how it reaches the plugin is open.
5. **Covers of a protected book stay plain (decided 2026-11-23).** Today Home's covers and thumbnails of a protected
   book lie in its enciphered cache folder like everything else there, so a card moved to another reader shows the
   placeholder cover ([design/cache-cipher.md](../design/cache-cipher.md), "What stays plain"). Decided: the cover and
   thumbnails stay outside the cipher, as screenshots and `sleep_frame.bin` do. Next step: exempt the files
   `BookCache::isCoverImage()` names (the cover and thumbnail BMPs, the cached raw cover) from the folder rule in
   `HalStorage`, and decide whether covers an older build enciphered are made again.
6. **D2 leads, device work.** On the Murphy the plain copy built about 65 ms slower with #880 than without it,
   while the cipher never runs for it and the X3 shows no such difference (5,812 against 5,824 ms); code placement in
   the S3's flash cache is the guess, not proven. The C3's AES peripheral (D1 uses it, through `esp_aes`) was not
   tried as the cache cipher.

Renewing a loan needs the plugin host's job queue, which is not here: [plugins.md](plugins.md), the survey's
leftovers. Fetching the book again works.

## LCP

Upstream fulfils LCP loans through Free-Ink's service (`lcp.freeink.org`: `/enroll`, `/fulfill`, `/unlock`), and
enrolment needs a release key that only CrossPoint's official builds carry (upstream's `src/network/LcpDevice.cpp`;
not in this tree). The SDK keeps the production LCP profile off the device (`LcpLicense.h`). Asked of Free-Ink on
2026-11-17: may another firmware's release builds use the service. No answer recorded yet. The reader already opens
LCP-shaped books given the key (D1).

## What is kept elsewhere

The SDK fork branch `feat/content-protection-seams` (4e418ce, an injectable pull inflater and entry index for
`ProtectedBook`, on 36cdcc8, a GCC 15 fix for the ContentProtection host tests) is pushed but unused here; it is an
offer to Free-Ink.
