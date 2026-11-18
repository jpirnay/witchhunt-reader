#pragma once

#include <cstdint>
#include <string>

class BuildArena;

// Opening a protected book: its key (<book>.key), its loan, its manifest (<cache>/protect.bin,
// built on the first open with a key), and its ZipFile::Protection, registered so every read of
// the archive decrypts. What Epub calls; the rest of lib/BookProtection stays behind it.
namespace book_protection {

enum class OpenResult : uint8_t {
  Plain,        // encryption.xml lists no AES-CBC entry
  Protected,    // registered: the book reads
  NoKey,        // protected, and no <book>.key
  OtherDevice,  // a key this device did not wrap (a copied card), or a damaged one
  Expired,      // the loan's due date has passed
  NeedsTime,    // a loan, and the clock has never been set: one Wi-Fi connection sets it
  Unreadable,   // the manifest could not be built or read (logged)
};

// Registers the protection of the archive at `epubPath`, whose ZipFile::contentFingerprint() is
// `fingerprint`. `build`: build the manifest in `cacheDir` when it is missing, else only open one
// that is there (Unreadable without it). `scratch`: lent to the build.
OpenResult openBook(const std::string& epubPath, const std::string& cacheDir, uint64_t fingerprint, bool build,
                    BuildArena* scratch);

// Unregisters the archive's protection: before its cache is cleared (the protection holds the
// manifest open) or the book moves.
void closeBook(const std::string& epubPath);

// The manifest's path in a book's cache folder.
std::string manifestPath(const std::string& cacheDir);

}  // namespace book_protection
