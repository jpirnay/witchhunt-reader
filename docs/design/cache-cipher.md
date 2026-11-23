# A protected book's cache at rest (D2) — design

> **Status.** Implemented (#880). Code: `lib/CacheCipher/CacheCipher.*` (the
> cipher), `lib/CacheCipher/CacheKeys.*` (the key derivation), `lib/CacheCipher/CacheCipherFile.*` (which files, the
> header, the read and write rules), `lib/hal/HalStorage` (the folder rule, `openRaw`),
> `lib/BookProtection/CacheAtRest.*` (the key from the device secret, marking a folder, emptying an old one). The
> format on the card: [file-formats.md](../file-formats.md), "Enciphered cache folders". Tests: `test/cache_cipher/`
> (RFC 8439 vectors, the derivation, slices at their own offsets), `test/hal_storage/` (the device's `HalStorage.cpp`
> over SdFat), `test/epub_pipeline/` (every corpus book laid out in an enciphered folder; `ProtectedBookPipelineTest`).
> Device benchmark: `env:bench_crypto` (`bench_crypto/main.cpp`). Open leads:
> [future_work/protected-content.md](../future_work/protected-content.md).

D1, D2 and D3 name the three parts of protected-content support: D1 opens a protected book (#821), D3 runs a plugin's
crypto on the device (#857), D2 is this record.

## Problem

D1 decrypts a protected book's entries as the reader needs them, and the reader keeps what it derives in the book's
cache folder, as upstream's does: the extracted chapters, the laid-out pages, footnotes, search snippets, pictures and
their pixel caches, the book's own fonts. A card read on a computer held the book in the clear beside the encrypted
EPUB.

## Constraints

- **The owner's rule:** the fastest and leanest cipher available, and reading must not feel different.
  `bench_crypto` states the budgets it was measured against: a page turn's decipher under 2 ms for a single-digit-KB
  page record, a section build under 10 % slower, a cached image of about 48 KB under 25 ms.
- **No format changes.** Every cache format stores offsets (the page LUT, the patched section header, the anchor and
  page-break maps) and patches itself in place. The cipher has to keep each byte where it was.
- **Every reader of the folder.** The reader is not the only code that opens files there: Home and the library read
  covers and thumbnails, the sleep screen reads the book's cover.

## The cipher

ChaCha20 (RFC 8439) as a keystream XORed over the file (`CacheCipher`). It is length-preserving, so no offset moves.
It is addressable by file offset, so a page turn deciphers only the record it reads; a CBC mode would read from byte
zero. The same call enciphers and deciphers.

On the device the keystream comes from wolfSSL's ChaCha20, which TLS links already, so the cipher costs no flash of
its own. The portable core in `CacheCipher.cpp` stays for the host tests, which do not build wolfSSL; `bench_crypto`
checks the device path against the same RFC vectors.

Measured on the X3 at 160 MHz (`pio run -e bench_crypto`, 64 passes a row):

| Cipher | 2 KB | 4 KB | 64 KB |
|---|---|---|---|
| `CacheCipher` (device path) | 0.56 ms | 1.11 ms | 17.4 ms (3.7 MB/s) |
| wolfSSL ChaCha20, raw | 0.45 ms | 0.89 ms | 14.2 ms (4.6 MB/s) |
| wolfSSL AES-128-CBC decrypt | 1.34 ms | 2.67 ms | 42.6 ms (1.5 MB/s) |
| wolfSSL AES-256-CTR | 1.72 ms | 3.42 ms | 54.6 ms (1.2 MB/s) |

The portable core measured 1.8 MB/s on the C3 when the cipher was first written: 2.25 ms for a 4 KB record and 26 ms
for a 48 KB image, over both budgets. It XORs a byte at a time; wolfSSL XORs a word at a time. The wrapper takes about
a quarter longer than raw wolfSSL (3.7 against 4.6 MB/s).

AES in software is slower (the table). wolfSSL's Espressif hardware port is gated on `WOLFSSL_ESPIDF`, which
never turns on under Arduino, and would not help a keystream anyway: its single-block entry point takes a mutex and
reloads the key every 16 bytes (`CacheCipher.h`). D1 decrypts the book's own entries on the chip's AES peripheral
through ESP-IDF's `esp_aes` (`lib/BookProtection/AesCbcCipher.h`); that path was not tried for the cache.

## Keys

A folder's key is `cachekeys::deriveBookKey` (`CacheKeys.h`): the first 32 bytes of the ChaCha20 keystream under a
device key and the folder's salt. The device key is `sha256("cache-key:" || the device secret)`
(`book_protection::cacheFolderKey`). The device secret is D1's, in NVS, so the key never lies on the card: another
reader, or a computer, reads nothing. The label keeps this key apart from the other keys made from the secret (the
book keys' wrap, the plugins' device IDs). The salt sits in the folder's marker and is new with every marker, so a
cache made again is enciphered under another key.

Each file has its own random nonce, made whenever the file is written from empty and kept in its header
(`CacheCipherFile.h`). A rewrite never reuses a keystream; a patch in place reuses its own offset's keystream, which
shows only how the patched bytes changed. `cachekeys::deriveFileNonce` (a nonce from the file's name) is older than
the header; only its host test calls it, and `CacheKeys.h`'s notes on a name-derived nonce predate the header.

## The file layer

`HalStorage` enciphers every file opened in a book cache folder (`.../epub_<n>/`) that holds the marker `cipher.bin`,
whoever opens it. The rule sits beneath every caller, so no reader, theme or screen needed a change.
`HalFile` hides the header: size, position and seeks are the content's.

- **The header is one sector (512 B).** The content then starts on a sector as a plain file's does, and SdFat moves it
  in whole aligned sectors. The first version's 16-byte header put the content off the grid, so every file went
  through SdFat's one-sector cache: 86 ms of that version's +330 ms on the X3 chapter below. On the card the larger
  header costs nothing, since a file takes whole clusters.
- **A large write from RAM is enciphered where it is, then restored.** Writing through a 512-byte chunk made every
  card write one sector: 118 ms of the +330 ms.
- **The open file keeps its last keystream block:** 34 ms. Pure ChaCha is about 30 ms of the chapter's reads.
- **Without the key nothing opens.** A marker another reader made, or a damaged one, refuses every open; an open for
  writing is refused before it can empty a file. A file without a valid header (copied in, damaged, an older layout)
  is damaged cache: removed when opened and made again.
- **Files leave the device enciphered.** The web server's download, the serial transfers and the serial test API's
  GET and HASH read raw (`HalStorage::openRaw`).

## An older cache

A protected book's folder made by earlier firmware is emptied on the book's next open, or the next time Home or the
library looks at it (`book_protection::encipherCache`). The reader's state (`BookCache::isReadingState`) and the
manifest `protect.bin`, which takes an Adobe book seconds to build again, are written back enciphered; everything else
is made again by the next open. Freed sectors keep what they held until they are written over.

## What stays plain

- Screenshots and Quick Resume's `sleep_frame.bin` (owner's decision).
- A book font copied into a card-backed flash-area image (`flash-area.img`, the S3 boards without a usable `spiffs`
  partition; [flash-area.md](flash-area.md)) stays unenciphered there (owner's decision).

Covers and thumbnails that Home takes from a protected book's folder are enciphered with it, so a card moved to
another reader shows the placeholder cover. Whether to keep it so is a decide item in
[future_work/protected-content.md](../future_work/protected-content.md).

## Measured in the reader

Moby-Dick's chapter 2, 14 pt, a cold build, three runs each.

| Board | master, plain / protected | #880, plain / protected | The cipher's share |
|---|---|---|---|
| X3 (62 pages) | 5,824 / 5,827 ms | 5,809 / 5,935 ms | +126 ms (+2.2 %) |
| Murphy (S3, SDMMC; 90 pages) | 4,677 / 4,717 ms (+40 ms, D1) | 4,747 / 4,842 ms | about +55 ms (+1.2 %) |

Page turns (a page record read and drawn) are unchanged on both, 40 to 55 ms on the X3. A cache made by master was
emptied and enciphered on the first open, keeping its progress and manifest. On the card every file of the folder
starts with `WCC1` and holds neither "Ishmael" nor "whale".

## Cost

On the X3/X4 build against master: flash +1,180 B, static RAM +288 B (the table of the last four cache folders, in the
`HalStorage` singleton), IRAM unchanged. An open enciphered file takes about 120 B more heap (its cipher and keystream
block). The new code is about 5.5 KB. Linking `lib/CacheCipher` shrank other code by about 4.6 KB in the same link, so
the net figure depends on layout.
