#include "CacheAtRest.h"

#include <BookCache.h>
#include <CacheKeys.h>
#include <DeviceSecret.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <wolfssl/wolfcrypt/hash.h>

#include <cstring>
#include <memory>
#include <string_view>

#include "BookSession.h"

namespace book_protection {

static_assert(cache_cipher::kSaltBytes == cachekeys::kSaltBytes, "one salt for the marker and the derivation");
static_assert(cache_cipher::kKeyBytes == cachekeys::kBookKeyBytes, "one key size for the derivation and the cipher");

bool cacheFolderKey(const uint8_t (&salt)[cache_cipher::kSaltBytes], uint8_t (&key)[cache_cipher::kKeyBytes]) {
  uint8_t secret[32];
  if (!deviceSecret(secret)) return false;
  // Its own label: no other key made from the secret (the book keys' wrap, the plugins' device IDs) is this one.
  static constexpr char LABEL[] = "cache-key:";
  uint8_t input[sizeof(LABEL) - 1 + sizeof(secret)];
  memcpy(input, LABEL, sizeof(LABEL) - 1);
  memcpy(input + sizeof(LABEL) - 1, secret, sizeof(secret));
  uint8_t deviceKey[cachekeys::kDeviceKeyBytes];
  const bool ok = wc_Sha256Hash(input, sizeof(input), deviceKey) == 0;
  memset(secret, 0, sizeof(secret));
  memset(input, 0, sizeof(input));
  if (ok) cachekeys::deriveBookKey(deviceKey, salt, key);
  memset(deviceKey, 0, sizeof(deviceKey));
  return ok;
}

namespace {

// What survives emptying a folder: the reader's state, which nothing else can give back, and the manifest, which takes
// an Adobe book seconds to build again. All small; a file that does not fit in memory is left to be made again.
constexpr size_t kMaxKeptBytes = 64 * 1024;
constexpr char kManifestName[] = "protect.bin";

bool kept(const std::string_view name) { return BookCache::isReadingState(name) || name == kManifestName; }

struct KeptFile {
  std::string name;
  std::unique_ptr<uint8_t[]> bytes;
  size_t length = 0;
};

// The kept files of a folder not yet enciphered: their bytes are as they are on the card.
size_t readKept(const std::string& cacheDir, KeptFile (&out)[8]) {
  size_t count = 0;
  HalFile dir = Storage.open(cacheDir.c_str());
  if (!dir || !dir.isDirectory()) return 0;
  char name[48];
  for (HalFile entry = dir.openNextFile(); entry && count < 8; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    if (entry.isDirectory() || !kept(name)) continue;
    const size_t length = entry.fileSize();
    if (length == 0 || length > kMaxKeptBytes) {
      if (length != 0)
        LOG_ERR("EBC", "%s is too large to keep (%u B): made again", name, static_cast<unsigned>(length));
      continue;
    }
    entry.close();
    // By path: a folder listing's entry is the directory's, not a stream of the file (on the host shim, no stream).
    HalFile file;
    auto bytes = makeUniqueNoThrow<uint8_t[]>(length);
    if (!bytes || !Storage.openFileForRead("EBC", cacheDir + "/" + name, file) ||
        file.read(bytes.get(), length) != static_cast<int>(length)) {
      LOG_ERR("EBC", "Could not keep %s: made again", name);
      continue;
    }
    out[count].name = name;
    out[count].bytes = std::move(bytes);
    out[count].length = length;
    ++count;
  }
  return count;
}

}  // namespace

bool encipherCache(const std::string& epubPath, const std::string& cacheDir) {
  if (Storage.cacheFolderEnciphered(cacheDir.c_str())) return true;
  // The protection holds the manifest open, and the manifest goes with the folder (to come back enciphered).
  closeBook(epubPath);

  KeptFile keptFiles[8];
  const size_t keptCount = readKept(cacheDir, keptFiles);
  // Everything derived goes: the whole folder, which comes back empty and marked.
  if (!BookCache::clear(cacheDir, BookCache::Keep::Nothing)) {
    LOG_ERR("EBC", "Could not empty %s to encipher it", cacheDir.c_str());
    return false;
  }
  if (!Storage.mkdir(cacheDir.c_str()) || !Storage.markCacheFolderEnciphered(cacheDir.c_str())) {
    LOG_ERR("EBC", "Could not mark %s enciphered", cacheDir.c_str());
    return false;
  }
  for (size_t i = 0; i < keptCount; ++i) {
    const std::string path = cacheDir + "/" + keptFiles[i].name;
    HalFile file;
    const bool written = Storage.openFileForWrite("EBC", path, file) &&
                         file.write(keptFiles[i].bytes.get(), keptFiles[i].length) == keptFiles[i].length;
    if (!(file.close() && written)) {
      LOG_ERR("EBC", "Could not keep %s: made again", path.c_str());
      Storage.remove(path.c_str());
    }
    memset(keptFiles[i].bytes.get(), 0, keptFiles[i].length);
  }
  LOG_INF("EBC", "Enciphered the cache of %s (%u files kept)", epubPath.c_str(), static_cast<unsigned>(keptCount));
  return true;
}

}  // namespace book_protection
