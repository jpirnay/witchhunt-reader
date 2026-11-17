# Protected content (DRM)

Status: plan of 2026-11-17, replacing the September plan on `feat/protected-content-phase0`
(`docs/protected-content-plan.md` and `docs/protected-content-handover-2026-09-18.md` there).

Owner decisions of 2026-11-17:
- Adobe ADEPT is supported. It is what Libby, Onleihe and the other loan services mostly use, so the Plugin Hub's
  `protected-content` plugin is meant to run here.
- LCP: Free-Ink is asked whether other firmwares may use `lcp.freeink.org` (see LCP below).
- No second library: decryption runs on our own `ZipFile` and our own uzlib inflate (`lib/InflateReader`), never on
  the SDK's vendored miniz or its own zip reader. (In the end none of the SDK library is linked.)
- Order: the X3 catalog check of the plugin work, then D1 and D2.

## The model: upstream's, so the plugins work

Upstream crosspoint-reader (crosspoint-reader#3114, #3824 and #3629 by Justin Mitchell, #4016 by Jadehawk, #3879
by Janne Heß) keeps every DRM scheme out of the reader:
- The reader opens EPUBs whose entries are encrypted with OCF `encryption.xml` (AES-128-CBC or AES-256-CBC, a
  16-byte IV prefix, optional `Compression Method="0"` and `OriginalLength`). The SDK's
  `libs/book/ContentProtection` (`ProtectedBook`) does the decryption; it is already in our submodule, unlinked.
- The content key comes from `<book>.key`, beside the book, which a plugin writes through `POST /api/book-key`
  (`{path, key, expires}`). The file holds the key wrapped to the device: `FIBK`, version 2, key length, due date,
  then AES-256-GCM (12-byte IV, the wrapped key, 16-byte tag) with the 14-byte header as additional data. The KEK
  is SHA-256("book-key:" || a 32-byte device secret in NVS, `devid/secret`).
- A due date is checked when the book opens, against a clock that cannot be set back: a floor kept in NVS,
  restored at boot, raised at every sleep and set by SNTP on every Wi-Fi join. A book with a due date and no
  trusted time does not open and asks for one Wi-Fi connection.
- `/api/status?plugin=<name>` answers a `deviceId` (SHA-256 of the secret and the plugin name), which the Adobe
  plugin uses to activate.

How keys are obtained stays in plugins. The Adobe plugin is an ADEPT activation and fulfilment client in the
browser: it unwraps each book's key with the account's license key through `api.crypto` and hands it to
`/api/book-key`.

## What differs here, and why

- **Decrypted content on the card.** Upstream says nothing decrypted is written to the card, but its section build
  writes the decrypted chapter to the cache, and images too. Ours does the same (`spines/<bucket>/html_<n>.bin` and
  the extracted images). D2 enciphers a protected book's cache at rest, which upstream does not.
- **The hook point.** Upstream hooks `Epub::readItemContents*`. Here `Section` builds its own `ZipFile` and
  `EntryReader`, and about 18 files use `ZipFile` directly, so the decryption goes into `ZipFile`, which every read
  passes through.
- **In-place readers.** `ZipFile::getStoredEntryRange()`, `Epub::openStoredCoverInPlace()` and `decodePngInPlace`
  hand out the raw bytes of a STORED entry. For an encrypted entry those are ciphertext (LCP stores its resources
  STORED), so they must refuse it and let the caller take the decrypting path.
- **No per-entry table in RAM.** The SDK's `ProtectedBook` keeps every zip entry (`ZipScan`, 24 to 32 bytes each) and
  every encrypted entry's hash and size; a book with 1,700 entries would need tens of KB on the X3. Here the encrypted
  entries' facts live on the card (`protect.bin`) and are read in ranges.
- **Decryption beneath our inflate.** The section build, the image probes and the footnote previews read through
  `ZipFile::EntryReader`, sliced across loop passes; `ProtectedBook::decryptEntryToSink()` pushes a whole entry in one
  call and cannot serve them. So `ZipFile` decrypts the raw bytes of an encrypted entry before its own uzlib inflate,
  in every read path, and nothing of the SDK library is linked. Its format rules (IV prefix, PKCS#7 for AES-128,
  xmlenc's padding for AES-256, `Compression Method="0"`, `OriginalLength`) are followed.

## D1: the reader opens protected books

1. SDK seams (2026-11-17, fork branch `feat/content-protection-seams`, 36cdcc8 and 4e418ce): an injectable pull
   inflater and entry index in `ProtectedBook`, with `test_seams`, also built with no miniz linked. Not used here after
   all (see above) and not pinned; an offer to Free-Ink, with the two GCC 15 test fixes.
2. Done, host-tested, not yet run on a device:
   - `ZipFile` (7ae6d69e9): a registry of protected archives (`ZipFile::protect()`, two at a time; while none is
     registered no read takes the lock). A registered archive's stats carry `METHOD_ENCRYPTED` in their method, so
     the stats cached in `book.bin` and the image manifest carry it too, and `getStoredEntryRange()` refuses the
     entry (the in-place cover and PNG decoders then extract). Every read path decrypts beneath the inflate. Adobe
     gives no plaintext sizes: `METHOD_SIZE_ESTIMATED`, `uncompressedSize` never under a full inflate window, and
     `hasExactSize()` for callers that compare sizes (the extract check, the image re-extract check).
   - `lib/BookProtection` (247a42d98): `encryption.xml` through our SAX parser into `<cache>/protect.bin`, records
     sorted by local-header offset and read through a page of 42, built in runs when the sorting table does not fit.
     Sizes Adobe does not give are measured at the build (decrypt and inflate once) for all but JPEG, PNG, GIF and
     WebP. AES-CBC on the chip's AES peripheral (`esp_aes`, already linked by the Wi-Fi stack).
   - `Epub`: the fingerprint walk notes `encryption.xml`; `load()` registers the protection before anything reads
     (building `protect.bin` on the first open with a key, and wiping a cache made before the protection was known);
     the cover and metadata loads register a protection an earlier open established; `clearCache()` drops it.
3. Done (34507c5f7 and the web commit): upstream's `<book>.key` (FIBK v2, v1 read) and NVS device secret, ported;
   `POST /api/book-key`; `deviceId` in `/api/status?plugin=`.
4. Done (58a4f8539): `HalClock::loanNow()`, the clock but never earlier than the latest plausible time the device
   has seen, a floor kept in NVS where upstream's `TrustedTime` keeps it (`cptime/floor`), raised at a loan check,
   after an NTP sync and at the sleep entry. A clock set back (an upload's `t`, `HalClock::applyClientTime()`) cannot
   reopen an ended loan; the floor lags while the device is off.
5. Done (7aae79f7b): the key is a sidecar (`SidecarFiles::kKeyExtensions`, `.epub.key`), so moving and removing a
   book carry it; `BookRelocation::bookMoved()` drops the moving book's protection, which holds its manifest open.
6. Done (242ef3eac): the reader says why a protected book does not open (no key, another device's key, an ended
   loan, no clock, unreadable) and goes back. English only so far; the release's translation pass adds the rest.
7. Host: `EpubProtectedPipelineTest` runs protected copies of three books (text, inline images, JPEGs) through the
   whole pipeline and gets the plain books' layout dump byte for byte; the refusals too. Device, X4 Pro, 2026-11-17 (a68dfdf1f): moby-dick
   made by `tools/make_protected_epub.py` (Adobe and LCP shapes), keys posted through `/api/book-key` (bad key length,
   a system path and a non-EPUB refused; `deviceId` stable and per plugin). Both copies open on chapter 1 with the plain
   book's 109 pages; the page dumps match the plain book's byte for byte; the covers decrypt in the file browser; a copy
   with no key shows its message and goes back. Timings: the Adobe first open builds the manifest in 2.3 s (it measures
   the text's sizes), LCP's in 0.26 s (sizes given), a cached reopen registers in about 60 ms; chapter 1's build 5.94 s
   as LCP (decrypting) against 5.88 s plain. Still open: the X3 (the C3's heap and time, software-free AES there too).

Cost on the C3 against master (`default`, 2026-11-17): flash +16.3 KB, static RAM +296 bytes.

## D2: the cache at rest

Owner rule (2026-11-17): the cipher must be the fastest and leanest available and must not change how reading feels.
Measure first, on the C3: a page turn and a chapter build of a protected book against the same book unprotected, plus
flash. If even the fastest cipher is noticeable, fall back to a cheap obfuscation (a keyed XOR stream, say) or leave
the cache plain, and say which in the release notes. The September numbers are the starting point: ChaCha20 through
wolfSSL 4.6 MB/s, a portable ChaCha20 1.8 MB/s, AES-128-CBC 1.5 MB/s; a 4 KB page record 1.1 ms.

Carry over Phase 0 from `feat/protected-content-phase0` by hand (the branch is 3,345 commits behind and conflicts in
12 files): `lib/CacheCipher` (ChaCha20 through wolfSSL, 4.6 MB/s on the C3, length-preserving and addressable by
offset), the storage scope that enciphers a protected book's cache files, and its host tests. Its keys come from
the device secret of D1 instead of a second store, and the `.rights` stand-in becomes the real "this book is
protected" from D1.

## D3: the Adobe plugin

`api.crypto` in the plugin host (random, sha1, sha256, aesenc/aesdec, rsadec, keygen, pubencrypt, sign, pkcs12) and
`POST /api/crypto`. This needs wolfSSL's RSA private-key operations and PKCS#12, which `scripts/patch_wolfssl.py`
removed to save flash (`WOLFSSL_RSA_PUBLIC_ONLY`, `NO_PWDBASED`): measure that cost on the C3 first. Then an end-to-end
check with a real loan, which needs an Adobe ID and a loan from a library.

## LCP

Upstream fulfils LCP loans through Free-Ink's service (`lcp.freeink.org`: `/enroll`, `/fulfill`, `/unlock`), and
enrolment needs a release key that only CrossPoint's official builds carry (`src/network/LcpDevice.cpp`). The SDK
keeps the production LCP profile off the device (`LcpLicense.h`), so the September plan's LCP on the device does not
work for real loans. Asked of Free-Ink on 2026-11-17: may another firmware's release builds use the service.

## What is lost

The SDK commit the September branch pins (`d66c7a9e`, branch `feat/injectable-inflate`) was never pushed; it is not
in any clone here. D1 step 1 rewrote it against today's `ProtectedBook.cpp`, and the firmware then went without it.
