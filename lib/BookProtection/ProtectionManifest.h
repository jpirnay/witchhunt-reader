#pragma once

#include <ZipFile.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class BuildArena;

// The encrypted entries of one protected book, on the card beside its other caches: built once
// from META-INF/encryption.xml, then read in ranges, so nothing per entry is held in RAM while the
// book is read (books of thousands of entries exist; a C3 has no room for a table of them).
//
// protect.bin: a header pinning the archive's content fingerprint and the cipher (AES-128 or
// AES-256; one per book, as Free-Ink's ProtectedBook has it), then runs of 12-byte records, each
// sorted by the entry's local-header offset: the plaintext's size and whether it is deflated. One
// run unless the transient table that sorts them does not fit the memory at hand.
//
// Adobe's manifest gives no plaintext sizes. The build measures them (decrypts and inflates each
// entry once, with the key), for every entry but the JPEG, PNG, GIF and WebP images: the readers
// only ever stream those, the measuring would read a picture book whole, and an estimate serves
// (ZipFile::METHOD_SIZE_ESTIMATED).
namespace book_protection {

enum class ManifestStatus : uint8_t {
  Plain,      // encryption.xml lists no AES-CBC entry (font obfuscation, say): read as any book
  Protected,  // built, or opened
  Missing,    // no manifest, or one for other content: build it
  Failed,     // logged
};

// The AES-CBC entries encryption.xml lists, and their cipher (128 or 256 bits), without the key.
// Failed when the document is malformed or mixes ciphers.
struct ManifestProbe {
  ManifestStatus status = ManifestStatus::Failed;
  uint32_t entries = 0;
  uint16_t cipherBits = 0;
};
ManifestProbe probeManifest(const std::string& epubPath);

// Builds `manifestPath` for the archive at `epubPath`, whose ZipFile::contentFingerprint() is
// `fingerprint`, measuring with the content key. `scratch`: a lent region for the transient table
// and the measuring reads, or null for the heap. Plain writes a manifest that says so, so the next
// open does not parse encryption.xml again. While it runs, a protection holding only the key is
// registered for `epubPath`; none is left registered afterwards.
ManifestStatus buildManifest(const std::string& epubPath, uint64_t fingerprint, const uint8_t* key, size_t keyLen,
                             const std::string& manifestPath, BuildArena* scratch);

// The protection over `manifestPath`, built for this content and a key of its cipher, in `out`.
ManifestStatus openManifest(const std::string& manifestPath, uint64_t fingerprint, const uint8_t* key, size_t keyLen,
                            std::unique_ptr<ZipFile::Protection>* out);

}  // namespace book_protection
