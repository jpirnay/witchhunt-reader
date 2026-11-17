# Protected content (DRM)

Status: plan of 2026-11-17, replacing the September plan on `feat/protected-content-phase0`
(`docs/protected-content-plan.md` and `docs/protected-content-handover-2026-09-18.md` there).

Owner decisions of 2026-11-17:
- Adobe ADEPT is supported. It is what Libby, Onleihe and the other loan services mostly use, so the Plugin Hub's
  `protected-content` plugin is meant to run here.
- LCP: Free-Ink is asked whether other firmwares may use `lcp.freeink.org` (see LCP below).
- No second library: decryption runs on our own `ZipFile` and our own uzlib inflate (`lib/InflateReader`), never on
  the SDK's vendored miniz or its own zip reader.
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
- **No per-entry table in RAM.** The SDK's `ZipScan` keeps every zip entry (24 to 32 bytes each); a book with
  1,700 entries would need 50 KB on the X3. The SDK gets an injectable entry lookup that our `ZipFile` serves from
  its own walk, as it does for ordinary books.
- **One inflate.** The SDK's two inflate sites (entry decryption and the `encryption.xml` read) call miniz directly
  and push input into it. Our uzlib pulls input through a callback, so the SDK gets an injectable pull inflater
  (begin, read, end). Its miniz binding stays for standalone use and is never linked here. Offered to Free-Ink.

## D1: the reader opens protected books

1. SDK, on a fork branch offered to Free-Ink: the injectable inflater and entry lookup in `ProtectedBook`, with its
   host tests passing on both the miniz binding and a test inflater.
2. `ZipFile` decrypts the entries of a protected book; the in-place readers refuse encrypted entries.
3. `BookKey` (the `.key` format above), the device secret, `POST /api/book-key`, `deviceId` in `/api/status`.
4. The trusted clock: upstream's `TrustedTime` or a floor in `HalClock`, whichever fits our clock code.
5. `BookRelocation` carries `<book>.key` (and `.rights`) when a book moves.
6. Messages for a book with no key, a key for another device, an expired loan, no trusted time (`tr()`, all UI
   languages).
7. Tests: host tests on EPUBs encrypted by a script with a known key (AES-128 and AES-256, deflated and STORED
   entries, encrypted images and cover), the in-place refusals, the `.key` format, the clock floor; a device test
   on the X3 and an S3 board with the same books.

Cost to measure on the C3 against master: flash and the open-path heap.

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
in any clone here. D1 step 1 rewrites it against today's `ProtectedBook.cpp`.
