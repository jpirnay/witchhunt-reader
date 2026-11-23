#include "CacheCipherFile.h"

namespace cache_cipher {

namespace {

constexpr char kFolderPrefix[] = "/epub_";

}  // namespace

size_t folderLength(const char* path) {
  if (path == nullptr) return 0;
  // The last "/epub_<digits>/" in the path: a book's cache folder is never nested in another's.
  size_t found = 0;
  for (const char* at = strstr(path, kFolderPrefix); at != nullptr; at = strstr(at + 1, kFolderPrefix)) {
    const char* end = at + sizeof(kFolderPrefix) - 1;
    const char* digits = end;
    while (*end >= '0' && *end <= '9') ++end;
    if (end != digits && *end == '/' && end[1] != '\0') found = static_cast<size_t>(end - path);
  }
  return found;
}

bool isMarker(const char* path, const size_t folderLen) {
  return path != nullptr && path[folderLen] == '/' && strcmp(path + folderLen + 1, kMarkerName) == 0;
}

CipherStream::~CipherStream() { memset(keystream_, 0, sizeof(keystream_)); }

bool CipherStream::apply(uint8_t* buf, const size_t len, const uint64_t offset) {
  if (buf == nullptr) return false;
  if (len == 0) return true;
  // CacheCipher's own limit, checked before any byte changes.
  if (offset > CacheCipher::kMaxOffset || len - 1 > CacheCipher::kMaxOffset - offset) return false;
  constexpr size_t kBlock = CacheCipher::kBlockBytes;
  size_t done = 0;
  while (done < len) {
    const uint64_t at = offset + done;
    const uint64_t block = at / kBlock;
    const size_t within = static_cast<size_t>(at % kBlock);
    const size_t left = len - done;
    bool ok = true;
    if (within == 0 && left >= kBlock && block != block_) {
      // Whole blocks not computed yet: one call for all of them.
      const size_t run = left - left % kBlock;
      ok = cipher_.apply(buf + done, run, at);
      done += run;
    } else {
      if (block != block_) {
        memset(keystream_, 0, sizeof(keystream_));
        ok = cipher_.apply(keystream_, kBlock, block * kBlock);
        block_ = ok ? block : kNoBlock;
      }
      const size_t take = left < kBlock - within ? left : kBlock - within;
      if (ok) {
        for (size_t i = 0; i < take; ++i) buf[done + i] ^= keystream_[within + i];
      }
      done += take;
    }
    if (!ok) {
      // Only wolfSSL itself can fail here. Part of the buffer may be done already: none of it is handed on.
      memset(buf, 0, len);
      return false;
    }
  }
  return true;
}

void makeMarker(const uint8_t (&salt)[kSaltBytes], uint8_t (&out)[kMarkerBytes]) {
  memcpy(out, kMarkerMagic, sizeof(kMarkerMagic));
  memcpy(out + sizeof(kMarkerMagic), salt, kSaltBytes);
}

bool parseMarker(const uint8_t* bytes, const size_t len, uint8_t (&salt)[kSaltBytes]) {
  if (len != kMarkerBytes || memcmp(bytes, kMarkerMagic, sizeof(kMarkerMagic)) != 0) return false;
  memcpy(salt, bytes + sizeof(kMarkerMagic), kSaltBytes);
  return true;
}

const FolderTable::Info* FolderTable::find(const char* folder, const size_t len) {
  for (Entry& entry : entries_) {
    if (entry.len == len && memcmp(entry.folder, folder, len) == 0) {
      entry.age = 0;
      return &entry.info;
    }
  }
  return nullptr;
}

void FolderTable::put(const char* folder, const size_t len, const Info& info) {
  if (len == 0 || len >= kMaxFolder) return;  // looked up again next time
  Entry* slot = &entries_[0];
  for (Entry& entry : entries_) {
    if (entry.age < 255) ++entry.age;
    if (entry.len == 0) {
      slot = &entry;
      break;
    }
    if (entry.age > slot->age) slot = &entry;
  }
  drop(*slot);
  memcpy(slot->folder, folder, len);
  slot->folder[len] = '\0';
  slot->len = static_cast<uint8_t>(len);
  slot->age = 0;
  slot->info = info;
}

void FolderTable::forget(const char* path) {
  if (path == nullptr) return;
  const size_t pathLen = strlen(path);
  for (Entry& entry : entries_) {
    if (entry.len == 0) continue;
    const size_t common = pathLen < entry.len ? pathLen : entry.len;
    // One is the other or lies inside it: "/.crosspoint" or ".../epub_1" itself, or a file inside it.
    if (memcmp(entry.folder, path, common) == 0 &&
        (pathLen == entry.len || (pathLen < entry.len ? entry.folder[pathLen] == '/' : path[entry.len] == '/'))) {
      drop(entry);
    }
  }
}

void FolderTable::clear() {
  for (Entry& entry : entries_) drop(entry);
}

void FolderTable::drop(Entry& entry) {
  memset(entry.info.key, 0, sizeof(entry.info.key));
  entry.info.state = FolderState::Plain;
  entry.len = 0;
  entry.age = 0;
  entry.folder[0] = '\0';
}

}  // namespace cache_cipher
