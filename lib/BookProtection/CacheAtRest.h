#pragma once

#include <CacheCipherFile.h>

#include <cstdint>
#include <string>

// A protected book's cache at rest (D2, docs/design/cache-cipher.md). Its cache folder holds the book's
// derived content: the extracted chapters, the laid-out pages, the footnotes, search snippets, the pictures and their
// pixel caches, the book's own fonts. HalStorage enciphers every file of a marked folder (lib/CacheCipher/
// CacheCipherFile.h); this is where a folder gets its key and its mark.
namespace book_protection {

// The key of an enciphered cache folder from its marker's salt: lib/CacheCipher's derivation over a device key,
// sha256("cache-key:" || the device secret). Bound to this device as the book keys are (BookKey.cpp): a card read on
// a computer, or moved to another reader, holds nothing readable. What HalStorage::setCacheKeySource() is given.
bool cacheFolderKey(const uint8_t (&salt)[cache_cipher::kSaltBytes], uint8_t (&key)[cache_cipher::kKeyBytes]);

// Makes the cache folder `cacheDir` of the protected book `epubPath` enciphered. A folder made before (its derived
// files in the clear, or a cache of the book's ciphertext) is emptied first: the reader's own state (BookCache::
// isReadingState()) and the protection manifest are kept and written back enciphered, everything else is made again
// by the next open. Closes the book's protection first: it holds the manifest open. True when the folder is
// enciphered after it; false leaves nothing in the clear that was not before, and the book should not be opened.
bool encipherCache(const std::string& epubPath, const std::string& cacheDir);

}  // namespace book_protection
