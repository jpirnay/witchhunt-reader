#pragma once

// The file layer of the cache cipher (docs/future_work/protected-content.md, D2): which files are enciphered, the
// header that carries each file's nonce, and the read, write and offset rules. The device's HalFile and the host
// tests' stdio HalFile both follow them through the templates below, so the pipeline tests run the device's rules.
//
// A protected book's cache folder (".../epub_<n>/") holds a marker, cipher.bin, with a random salt. Every other file
// in it is enciphered: a 16-byte header (magic + a random nonce) and then the content, XORed with the ChaCha20
// keystream of (the folder's key, the file's nonce) at the content's own offset. The handle hides the header: its
// size, position and seeks are the content's, so every cache format, its offsets and its in-place patches work as
// they are. A new nonce per file written from empty: a rename keeps the nonce with the file, and rewriting a file
// never reuses a keystream (CacheCipher.h's contract). A patch in place reuses its own offset's keystream, which
// shows only how the patched bytes changed.

#include <CacheCipher.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace cache_cipher {

constexpr size_t kHeaderBytes = 16;
constexpr uint8_t kFileMagic[4] = {'W', 'C', 'C', '1'};
constexpr char kMarkerName[] = "cipher.bin";
constexpr uint8_t kMarkerMagic[4] = {'W', 'C', 'F', '1'};
constexpr size_t kSaltBytes = 12;
constexpr size_t kMarkerBytes = sizeof(kMarkerMagic) + kSaltBytes;
constexpr size_t kKeyBytes = CacheCipher::kKeyBytes;

// The folder's key from its salt. Installed by the firmware (lib/BookProtection: the device secret); without one, an
// enciphered folder cannot be read or written at all.
using KeySource = bool (*)(const uint8_t (&salt)[kSaltBytes], uint8_t (&key)[kKeyBytes]);
using RandomFill = void (*)(uint8_t* out, size_t len);

// The length of the book cache folder ("<anything>/epub_<digits>") that `path` lies strictly inside, else 0.
size_t folderLength(const char* path);
// Whether `path`, inside a folder of length `folderLen`, is that folder's marker.
bool isMarker(const char* path, size_t folderLen);

enum class FolderState : uint8_t {
  Plain,       // no marker
  Enciphered,  // marker and key
  Unreadable,  // a marker, but no key for it (none installed, or a damaged marker): every open in it fails
};

// What is known of the last few folders, so an open does not read the marker each time. Entries are dropped when a
// remove, rename or rmdir touches their folder, or when a folder is marked.
class FolderTable {
 public:
  struct Info {
    FolderState state = FolderState::Plain;
    uint8_t key[kKeyBytes] = {};
  };

  const Info* find(const char* folder, size_t len);
  void put(const char* folder, size_t len, const Info& info);
  // Drops every entry `path` is inside of or contains.
  void forget(const char* path);
  void clear();

 private:
  static constexpr size_t kEntries = 4;
#if defined(ARDUINO)
  // "/.crosspoint/epub_4294967295" is 28: the table lives in the HalStorage singleton, so in the C3's static RAM.
  static constexpr size_t kMaxFolder = 32;
#else
  static constexpr size_t kMaxFolder = 192;  // the host tests' folders sit in a temp folder
#endif
  struct Entry {
    char folder[kMaxFolder] = {};
    uint8_t len = 0;  // 0: empty
    uint8_t age = 0;
    Info info;
  };
  void drop(Entry& entry);
  Entry entries_[kEntries];
};

void makeMarker(const uint8_t (&salt)[kSaltBytes], uint8_t (&out)[kMarkerBytes]);
bool parseMarker(const uint8_t* bytes, size_t len, uint8_t (&salt)[kSaltBytes]);

inline void makeHeader(const uint8_t (&nonce)[CacheCipher::kNonceBytes], uint8_t (&out)[kHeaderBytes]) {
  memcpy(out, kFileMagic, sizeof(kFileMagic));
  memcpy(out + sizeof(kFileMagic), nonce, CacheCipher::kNonceBytes);
}

inline bool parseHeader(const uint8_t (&in)[kHeaderBytes], uint8_t (&nonce)[CacheCipher::kNonceBytes]) {
  if (memcmp(in, kFileMagic, sizeof(kFileMagic)) != 0) return false;
  memcpy(nonce, in + sizeof(kFileMagic), CacheCipher::kNonceBytes);
  return true;
}

// The state of `path`'s folder (`folderLen` from folderLength()), from the table or else from its marker, with the
// folder's key in `key` when it is Enciphered. `readMarker(markerPath, bytes, cap)` returns the marker's length, 0
// when there is none, -1 when one is there and cannot be read.
template <class ReadMarker>
FolderState resolve(FolderTable& table, const char* path, const size_t folderLen, KeySource keySource,
                    ReadMarker&& readMarker, uint8_t (&key)[kKeyBytes]) {
  if (const FolderTable::Info* known = table.find(path, folderLen)) {
    memcpy(key, known->key, sizeof(key));
    return known->state;
  }
  char markerPath[256];
  // A folder path too long to look at is refused rather than guessed at.
  if (folderLen + 1 + sizeof(kMarkerName) > sizeof(markerPath)) return FolderState::Unreadable;
  memcpy(markerPath, path, folderLen);
  markerPath[folderLen] = '/';
  memcpy(markerPath + folderLen + 1, kMarkerName, sizeof(kMarkerName));

  FolderTable::Info info;
  uint8_t bytes[kMarkerBytes];
  const int got = readMarker(static_cast<const char*>(markerPath), bytes, sizeof(bytes));
  if (got != 0) {
    uint8_t salt[kSaltBytes];
    const bool keyed = got > 0 && parseMarker(bytes, static_cast<size_t>(got), salt) && keySource != nullptr &&
                       keySource(salt, info.key);
    info.state = keyed ? FolderState::Enciphered : FolderState::Unreadable;
    memset(salt, 0, sizeof(salt));
  }
  table.put(path, folderLen, info);
  memcpy(key, info.key, sizeof(key));
  memset(info.key, 0, sizeof(info.key));
  return info.state;
}

enum class Attach : uint8_t {
  Plain,       // an empty file opened read-only: it reads as empty, nothing to decipher
  Enciphered,  // `out` holds the file's cipher; the raw position is past the header
  Refused,     // not a file of this folder's (no header, a short one), or no memory: close it, the open fails
};

// Attaches the cipher to a file just opened at raw position 0. `Raw` has size(), seek(uint64_t), read(void*, size_t)
// and write(const void*, size_t) on the file's bytes as they are on the card.
template <class Raw>
Attach attach(Raw& raw, const bool mayWrite, const uint8_t (&key)[kKeyBytes], RandomFill fill,
              std::unique_ptr<CacheCipher>& out) {
  uint8_t header[kHeaderBytes];
  uint8_t nonce[CacheCipher::kNonceBytes];
  const uint64_t size = raw.size();
  if (size == 0) {
    if (!mayWrite) return Attach::Plain;
    fill(nonce, sizeof(nonce));
    makeHeader(nonce, header);
    if (raw.write(header, sizeof(header)) != sizeof(header)) return Attach::Refused;
  } else if (size < kHeaderBytes || raw.read(header, sizeof(header)) != static_cast<int>(sizeof(header)) ||
             !parseHeader(header, nonce)) {
    return Attach::Refused;
  }
  out.reset(new (std::nothrow) CacheCipher(key, nonce));
  memset(nonce, 0, sizeof(nonce));
  return out ? Attach::Enciphered : Attach::Refused;
}

// Reads at the raw position and deciphers in place. -1 when the cipher refuses (the bytes are zeroed, never handed
// out as they are on the card).
template <class Raw>
int read(Raw& raw, const CacheCipher& cipher, void* buf, const size_t count) {
  const uint64_t pos = raw.position();
  const int got = raw.read(buf, count);
  if (got <= 0) return got;
  if (pos < kHeaderBytes || !cipher.apply(static_cast<uint8_t*>(buf), static_cast<size_t>(got), pos - kHeaderBytes)) {
    memset(buf, 0, static_cast<size_t>(got));
    return -1;
  }
  return got;
}

// Enciphers a copy, a chunk at a time (the caller's bytes stay as they are), and writes it at the raw position.
// Stops short, never writing a byte in the clear, when the cipher refuses.
template <class Raw>
size_t write(Raw& raw, const CacheCipher& cipher, const void* buf, const size_t count) {
  uint8_t chunk[512];
  const auto* src = static_cast<const uint8_t*>(buf);
  const uint64_t start = raw.position();
  size_t done = 0;
  if (start < kHeaderBytes) return 0;
  while (done < count) {
    const size_t len = count - done < sizeof(chunk) ? count - done : sizeof(chunk);
    memcpy(chunk, src + done, len);
    if (!cipher.apply(chunk, len, start - kHeaderBytes + done)) break;
    const size_t wrote = raw.write(chunk, len);
    done += wrote;
    if (wrote != len) break;
  }
  memset(chunk, 0, sizeof(chunk));
  return done;
}

}  // namespace cache_cipher
