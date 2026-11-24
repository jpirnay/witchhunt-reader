#pragma once
// Real-filesystem shim for host tests that exercise ZipFile or CssParser.
// Implements FsFile on top of stdio so tests can read actual .epub files
// and write real cache files under /tmp.

#include <CacheCipherFile.h>
#include <fcntl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

// Device HalStorage.h transitively provides the Arduino core (millis, ESP);
// mirror that so TUs which only include HalStorage.h still compile.
#include "Arduino.h"
#include "Print.h"
#include "WString.h"

class HalFile : public Print {
 public:
  HalFile() = default;
  ~HalFile() { closeQuiet(); }

  HalFile(HalFile&& o) noexcept
      : fp_(o.fp_),
        hasImpl_(o.hasImpl_),
        isDir_(o.isDir_),
        hasName_(o.hasName_),
        name_(std::move(o.name_)),
        entrySize_(o.entrySize_),
        entryDate_(o.entryDate_),
        entryTime_(o.entryTime_),
        entries_(std::move(o.entries_)),
        next_(o.next_),
        dirPath_(std::move(o.dirPath_)),
        cipher_(std::move(o.cipher_)) {
    o.fp_ = nullptr;
    o.hasImpl_ = false;
    o.isDir_ = false;
    o.hasName_ = false;
    o.next_ = 0;
  }
  HalFile& operator=(HalFile&& o) noexcept {
    if (this != &o) {
      closeQuiet();
      fp_ = o.fp_;
      hasImpl_ = o.hasImpl_;
      isDir_ = o.isDir_;
      hasName_ = o.hasName_;
      name_ = std::move(o.name_);
      entrySize_ = o.entrySize_;
      entryDate_ = o.entryDate_;
      entryTime_ = o.entryTime_;
      entries_ = std::move(o.entries_);
      next_ = o.next_;
      dirPath_ = std::move(o.dirPath_);
      cipher_ = std::move(o.cipher_);
      o.fp_ = nullptr;
      o.hasImpl_ = false;
      o.isDir_ = false;
      o.hasName_ = false;
      o.next_ = 0;
    }
    return *this;
  }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  // WH_HOST_STDIO_UNBUFFERED=1 drops glibc's lazily malloc'd 4 KB buffer (one per open FILE*),
  // which has no device counterpart: SdFat reads through the card driver's shared sector cache.
  // The epub_pipeline heap census sets it so those buffers do not appear as allocation sites
  // (they showed up as 4 KB x 600 under a misleading symbol). Every read is then a syscall, so
  // it stays off by default.
  static void applyStdioPolicy(FILE* fp) {
    static const bool unbuffered = std::getenv("WH_HOST_STDIO_UNBUFFERED") != nullptr;
    if (fp != nullptr && unbuffered) setvbuf(fp, nullptr, _IONBF, 0);
  }

  bool openForRead(const std::string& path) {
    closeQuiet();
    hasImpl_ = true;
    ++opens;
    fp_ = fopen(path.c_str(), "rb");
    applyStdioPolicy(fp_);
    if (fp_ && trackOpenPaths) openPaths[fp_] = path;
    return fp_ != nullptr;
  }

  bool openForWrite(const std::string& path) {
    closeQuiet();
    hasImpl_ = true;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(parent, ec);
      if (ec) return false;
    }
    // Read/write, as on the device (SDCardManager::openFileForWrite: O_RDWR | O_CREAT | O_TRUNC):
    // a section build reads its anchor map back through the write handle.
    ++opens;
    fp_ = fopen(path.c_str(), "w+b");
    applyStdioPolicy(fp_);
    if (fp_ && trackOpenPaths) openPaths[fp_] = path;
    return fp_ != nullptr;
  }

  // Mirrors HalStorage::openFileForUpdate: existing file, read+write, no truncation.
  bool openForUpdate(const std::string& path) {
    closeQuiet();
    hasImpl_ = true;
    ++opens;
    fp_ = fopen(path.c_str(), "r+b");
    applyStdioPolicy(fp_);
    if (fp_ && trackOpenPaths) openPaths[fp_] = path;
    return fp_ != nullptr;
  }

  // Device parity, deliberately fatal. HalStorage gives every handle it opens an Impl — even when
  // the open FAILED — so on device close() is fine on an opened-and-failed handle but asserts on
  // one that was never opened at all (or was moved from). stdio cannot tell those apart, so
  // hasImpl_ models it: without this, closing a never-opened handle is silently fine on the host
  // and aborts in the reader's hands. Not assert(): host tests build with NDEBUG.
  bool close() {
    if (!hasImpl_) {
      std::fprintf(stderr, "HalFile::close() on a handle that was never opened — this aborts on device\n");
      std::abort();
    }
    return closeQuiet();
  }

  // Directory handle, so tests can exercise code that walks the SD card
  // (DictionaryRegistry scans /dictionaries/*/ for index stems). Entries are
  // snapshotted and sorted at open time: std::filesystem iteration order is
  // unspecified, and device SdFat order is not reproducible either, so a test
  // that depends on it would be flaky rather than wrong.
  bool openDir(const std::string& path) {
    closeQuiet();
    hasImpl_ = true;
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec) || ec) return false;
    isDir_ = true;
    dirPath_ = path;
    for (const auto& e : std::filesystem::directory_iterator(path, ec)) {
      DirEntry entry{e.path().filename().string(), e.is_directory()};
      if (!entry.isDir) {
        std::error_code sizeError;
        entry.size = static_cast<uint64_t>(std::filesystem::file_size(e.path(), sizeError));
      }
      fatStamp(e.path(), entry.fatDate, entry.fatTime);  // a folder's entry carries a date too
      entries_.push_back(std::move(entry));
    }
    std::sort(entries_.begin(), entries_.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return true;
  }

  bool isOpen() const { return fp_ != nullptr; }
  explicit operator bool() const { return fp_ != nullptr || isDir_ || hasName_; }

  // Test fault injection: a read that starts at or past this offset fails, as a flaky card read
  // would. -1 (the default) turns it off.
  static inline long failReadsFrom = -1;
  // The same for one read only: the first read starting at or past this offset fails, and the
  // hook turns itself off -- a single bad sector, with everything after it readable again.
  static inline long failOneReadFrom = -1;
  // Every read that starts exactly at this offset fails, reads elsewhere do not: hits one known
  // record (a ZIP entry's data) even when the reader visits later offsets first (the central
  // directory at the end of the archive). -1 turns it off.
  static inline long failReadAt = -1;
  // The same for seeks: every seek to exactly this offset fails, seeks elsewhere do not -- a card
  // error inside the file, where SdFat's seekSet fails when the FAT read of its cluster-chain walk
  // does. -1 turns it off.
  static inline long failSeekAt = -1;
  // Every seek to this offset or past it fails. stdio seeks past the end of a file; SdFat's seekSet
  // fails there. Set to a file's size, it models the card. -1 turns it off.
  static inline long failSeeksFrom = -1;
  // Test instrumentation: when set, every seek within a file appends its target offset, so a test
  // can fail each offset a pass seeks to in turn.
  static inline std::vector<size_t>* seekLog = nullptr;
  // Test instrumentation: bytes delivered by read(), across all files. Lets a test bound how much
  // a pass reads, where wall-clock time on a host says little about SD cost.
  static inline size_t bytesRead = 0;
  // The same for the card's per-call cost: on the device every read, seek, available() and write
  // is one call into SdFat under the storage lock, whatever its size. And file opens, each one a
  // directory walk there.
  static inline size_t calls = 0;
  // Paths removed through HalStorage::remove while a handle still held them open. On SdFat a handle that flushes into a
  // removed file writes through a freed cluster chain (cluster 0 for a tail crossing a cluster boundary), so a
  // remove-then-close order is a card-corruption bug; the host cannot see it (POSIX unlinks an open file; Windows
  // refuses). Recorded, not enforced: a test asserts on it. Keyed by FILE* so a moved handle keeps its entry.
  // Only while a test turns it on (TrackOpenPaths): the map's nodes and paths are heap the firmware does not have, and
  // the heap-peak gate counts every allocation (about 1.4 KB at a build's peak with the section's files open).
  static inline bool trackOpenPaths = false;
  static inline std::vector<std::string> removedWhileOpen;
  static inline std::map<FILE*, std::string> openPaths;
  static bool isOpenPath(const std::string& path) {
    for (const auto& [fp, p] : openPaths) {
      if (p == path) return true;
    }
    return false;
  }

  static inline size_t opens = 0;
  // Folders opened by path: on the device each one searches its parent from the first entry.
  static inline size_t folderOpens = 0;
  // And flushes: on the device a sync writes the file's data and its directory entry out to the card.
  static inline size_t flushes = 0;

  int read(void* buf, size_t n) {
    ++calls;
    if (!fp_) return -1;
    if (failReadsFrom >= 0 && ftell(fp_) >= failReadsFrom) return -1;
    if (failReadAt >= 0 && ftell(fp_) == failReadAt) return -1;
    if (failOneReadFrom >= 0 && ftell(fp_) >= failOneReadFrom) {
      failOneReadFrom = -1;
      return -1;
    }
    if (cipher_) {
      RawStdio raw{fp_};
      const int got = cache_cipher::read(raw, *cipher_, buf, n);
      if (got > 0) bytesRead += static_cast<size_t>(got);
      return got;
    }
    const size_t got = fread(buf, 1, n, fp_);
    bytesRead += got;
    return static_cast<int>(got);
  }
  // Single-byte read, SdFat-style: returns the byte or -1 (used by Bitmap.cpp).
  int read() {
    ++calls;
    if (!fp_) return -1;
    if (cipher_) {
      uint8_t b = 0;
      RawStdio raw{fp_};
      return cache_cipher::read(raw, *cipher_, &b, 1) == 1 ? b : -1;
    }
    return fgetc(fp_);
  }

  // A folder's position is where its listing stands, counted in 32-byte directory entries as on
  // FAT (one per name here; FAT spends more on a long one), so a walk can close a folder and seek
  // back into its listing (FolderWalk).
  static constexpr size_t kDirEntryBytes = 32;
  bool seek(size_t pos) {
    ++calls;
    if (isDir_) {
      if (pos % kDirEntryBytes != 0 || pos / kDirEntryBytes > entries_.size()) return false;
      next_ = pos / kDirEntryBytes;
      return true;
    }
    if (fp_ && seekLog) seekLog->push_back(pos);
    if (failSeekAt >= 0 && static_cast<long>(pos) == failSeekAt) return false;
    if (failSeeksFrom >= 0 && static_cast<long>(pos) >= failSeeksFrom) return false;
    return fp_ && fseek(fp_, static_cast<long>(pos + header()), SEEK_SET) == 0;
  }
  bool seekSet(size_t pos) { return seek(pos); }
  bool seekCur(int64_t offset) {
    ++calls;
    return fp_ && fseek(fp_, static_cast<long>(offset), SEEK_CUR) == 0;
  }
  bool seek64(uint64_t pos) { return fp_ && fseek(fp_, static_cast<long>(pos + header()), SEEK_SET) == 0; }
  bool seekSet64(uint64_t pos) { return seek64(pos); }

  size_t position() const {
    if (isDir_) return next_ * kDirEntryBytes;
    return fp_ ? static_cast<size_t>(ftell(fp_)) - header() : 0;
  }
  uint64_t position64() const { return position(); }
  // Bytes left to read, as on device: EpubImageManifest::load() sizes its corruption guard from
  // this, so a constant here made every persisted manifest look corrupt on the host.
  int available() const {
    ++calls;
    if (!fp_) return 0;
    const long cur = ftell(fp_);
    fseek(fp_, 0, SEEK_END);
    const long end = ftell(fp_);
    fseek(fp_, cur, SEEK_SET);
    return end > cur ? static_cast<int>(end - cur) : 0;
  }

  size_t size() { return fileSize(); }
  size_t fileSize() {
    if (!fp_) return hasName_ ? static_cast<size_t>(entrySize_) : 0;
    const long cur = ftell(fp_);
    fseek(fp_, 0, SEEK_END);
    const long sz = ftell(fp_);
    fseek(fp_, cur, SEEK_SET);
    return static_cast<size_t>(sz) > header() ? static_cast<size_t>(sz) - header() : 0;
  }
  uint64_t size64() { return fileSize(); }
  uint64_t fileSize64() { return fileSize(); }

  size_t write(const uint8_t* data, size_t size) { return write(static_cast<const void*>(data), size); }
  // Device SdFat accepts void*; Page.cpp writes char arrays through this.
  size_t write(const void* data, size_t size) {
    ++calls;
    if (!fp_) return 0;
    if (cipher_) {
      RawStdio raw{fp_};
      // The device enciphers a large write in place when the buffer is in RAM (HalStorage.cpp): so does the host, where
      // every buffer a write is given is.
      return cache_cipher::write(raw, *cipher_, data, size, true);
    }
    return fwrite(data, 1, size, fp_);
  }
  size_t write(uint8_t c) override { return write(static_cast<const void*>(&c), 1); }

  // HalStorage's at-rest cipher, as the device's HalStorage::attachCacheCipher does it: for a file just opened in an
  // enciphered cache folder. Damaged or Failed closes it, and the open fails.
  cache_cipher::Attach attachCacheCipher(const bool mayWrite, const uint8_t (&key)[cache_cipher::kKeyBytes]) {
    if (!fp_ || isDir_) return cache_cipher::Attach::Plain;
    RawStdio raw{fp_};
    const auto attached = cache_cipher::attach(raw, mayWrite, key, fillRandom, cipher_);
    if (attached == cache_cipher::Attach::Damaged || attached == cache_cipher::Attach::Failed) closeQuiet();
    return attached;
  }
  bool enciphered() const { return cipher_ != nullptr; }
  // A failed open, as the device leaves one: closed, and still closeable.
  void failOpen() { closeQuiet(); }
  void flush() {
    ++flushes;
    if (fp_) fflush(fp_);
  }
  size_t getName(char* out, size_t len) {
    if (!out || len == 0) return 0;
    const size_t n = std::min(name_.size(), len - 1);
    std::memcpy(out, name_.data(), n);
    out[n] = '\0';
    return n;
  }
  bool rename(const char*) { return false; }
  bool getModifyDateTime(uint16_t* pdate, uint16_t* ptime) {
    if (!hasName_ || pdate == nullptr || ptime == nullptr) return false;
    *pdate = entryDate_;
    *ptime = entryTime_;
    return true;
  }
  // std::filesystem keeps no creation time: the write time stands in for it.
  bool getCreateDateTime(uint16_t* pdate, uint16_t* ptime) { return getModifyDateTime(pdate, ptime); }
  bool isDirectory() const { return isDir_; }
  void rewindDirectory() { next_ = 0; }
  // One entry per call, exhausted-directory returns a falsy handle. A folder's
  // entry can be listed in turn, as SdFat's can; a file's carries what a scan
  // reads, and no open stream.
  HalFile openNextFile() {
    HalFile f;
    if (next_ >= entries_.size()) return f;
    const DirEntry& e = entries_[next_++];
    if (e.isDir) f.openDir(dirPath_ + "/" + e.name);
    f.hasImpl_ = true;
    f.hasName_ = true;
    f.name_ = e.name;
    f.isDir_ = e.isDir;
    f.entrySize_ = e.size;
    f.entryDate_ = e.fatDate;
    f.entryTime_ = e.fatTime;
    return f;
  }

 private:
  // A file's last write as FAT date and time, in UTC: the stamp SdFat reads from a directory entry.
  static void fatStamp(const std::filesystem::path& path, uint16_t& date, uint16_t& time) {
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(path, ec);
    date = 0;
    time = 0;
    if (ec) return;
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::file_clock::to_sys(written));
    const std::time_t t = std::chrono::system_clock::to_time_t(seconds);
    const std::tm* tm = std::gmtime(&t);
    if (tm == nullptr || tm->tm_year < 80) return;
    date = static_cast<uint16_t>(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
    time = static_cast<uint16_t>((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
  }

  // close() without the device-parity check: for the destructor and the open helpers, which must
  // both tolerate a handle that was never opened.
  bool closeQuiet() {
    cipher_.reset();
    if (fp_) {
      openPaths.erase(fp_);
      fclose(fp_);
      fp_ = nullptr;
    }
    isDir_ = false;
    entries_.clear();
    next_ = 0;
    return true;
  }

  struct DirEntry {
    std::string name;
    bool isDir = false;
    uint64_t size = 0;
    uint16_t fatDate = 0;
    uint16_t fatTime = 0;
  };

  FILE* fp_ = nullptr;
  bool hasImpl_ = false;
  bool isDir_ = false;
  bool hasName_ = false;
  std::string name_;
  uint64_t entrySize_ = 0;  // a directory entry's size and FAT stamp, as SdFat's openNext gives them
  uint16_t entryDate_ = 0;
  uint16_t entryTime_ = 0;
  std::vector<DirEntry> entries_;
  size_t next_ = 0;
  std::string dirPath_;
  std::unique_ptr<cache_cipher::CipherStream> cipher_;

  // An enciphered file's header, which sizes, positions and seeks leave out.
  size_t header() const { return cipher_ ? cache_cipher::kHeaderBytes : 0; }
  // The bytes as they are in the file, for cache_cipher's templates.
  struct RawStdio {
    FILE* fp;
    uint64_t size() const {
      const long cur = ftell(fp);
      fseek(fp, 0, SEEK_END);
      const long end = ftell(fp);
      fseek(fp, cur, SEEK_SET);
      return end > 0 ? static_cast<uint64_t>(end) : 0;
    }
    uint64_t position() const { return static_cast<uint64_t>(ftell(fp)); }
    bool seek(uint64_t pos) { return fseek(fp, static_cast<long>(pos), SEEK_SET) == 0; }
    int read(void* buf, size_t count) { return static_cast<int>(fread(buf, 1, count, fp)); }
    size_t write(const void* buf, size_t count) { return fwrite(buf, 1, count, fp); }
  };
  static void fillRandom(uint8_t* out, size_t len) {
    static std::mt19937 engine{std::random_device{}()};
    for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(engine());
  }
};

using FsFile = HalFile;

class HalStorage {
 public:
  bool openFileForRead(const char*, const std::string& path, HalFile& f) {
    if (refuses(path.c_str())) return refuse(f);
    return f.openForRead(path) && attach(f, path.c_str(), false);
  }
  bool openFileForRead(const char* m, const char* path, HalFile& f) { return openFileForRead(m, std::string(path), f); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& f) {
    if (refuses(path.c_str())) return refuse(f);  // before the open, which empties the file
    return f.openForWrite(path) && attach(f, path.c_str(), true);
  }
  bool openFileForWrite(const char* m, const char* path, HalFile& f) {
    return openFileForWrite(m, std::string(path), f);
  }
  bool openFileForUpdate(const char*, const std::string& path, HalFile& f) {
    if (refuses(path.c_str())) return refuse(f);
    return f.openForUpdate(path) && attach(f, path.c_str(), true);
  }
  bool openFileForUpdate(const char* m, const char* path, HalFile& f) {
    return openFileForUpdate(m, std::string(path), f);
  }

  // The device's at-rest cipher for a book cache folder (lib/hal/HalStorage.h), on the same rules.
  void setCacheKeySource(cache_cipher::KeySource source) {
    keySource_ = source;
    folders_.clear();
  }
  bool markCacheFolderEnciphered(const char* folder) {
    uint8_t salt[cache_cipher::kSaltBytes];
    std::random_device rd;
    for (uint8_t& b : salt) b = static_cast<uint8_t>(rd());
    uint8_t bytes[cache_cipher::kMarkerBytes];
    cache_cipher::makeMarker(salt, bytes);
    const std::string path = std::string(folder) + "/" + cache_cipher::kMarkerName;
    FILE* fp = fopen(path.c_str(), "wb");
    const bool ok = fp != nullptr && fwrite(bytes, 1, sizeof(bytes), fp) == sizeof(bytes);
    if (fp != nullptr && fclose(fp) != 0) return false;
    folders_.forget(folder);
    return ok;
  }
  bool cacheFolderEnciphered(const char* folder) {
    const std::string probe = std::string(folder) + "/" + cache_cipher::kMarkerName;
    const size_t len = cache_cipher::folderLength(probe.c_str());
    uint8_t key[cache_cipher::kKeyBytes];
    return len == std::strlen(folder) && state(probe.c_str(), len, key) == cache_cipher::FolderState::Enciphered;
  }
  // The file's bytes as they are on the card, never deciphered.
  HalFile openRaw(const char* path) {
    HalFile f;
    f.openForRead(path);
    return f;
  }
  bool exists(const char* path) { return std::filesystem::exists(path); }
  // The device counts the firmware's own writes, for caches keyed on the card's contents; a host
  // test changes its tree between runs, not under a cache.
  uint32_t contentGeneration() const { return 0; }
  // Mirrors the device HalStorage: stream a whole file into a Print sink in
  // chunks. Epub::applyMetadataSidecar() feeds ContentOpfParser through this,
  // so the host pipeline exercises the same code path the device runs.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256) {
    HalFile f;
    if (!openFileForRead("SD", path, f)) return false;
    std::vector<uint8_t> buf(chunkSize ? chunkSize : 256);
    for (;;) {
      const int n = f.read(buf.data(), buf.size());
      if (n < 0) return false;
      if (n == 0) break;
      if (out.write(buf.data(), static_cast<size_t>(n)) != static_cast<size_t>(n)) return false;
    }
    return true;
  }
  // Mirrors the device HalStorage: the whole file, its size checked against `cap` first. Fails on a
  // missing, directory, empty, above-`cap` or short-read file.
  bool readFileToString(const char*, const std::string& path, size_t cap, std::string& out) {
    out.clear();
    HalFile f;
    if (!openFileForRead("SD", path, f) || f.isDirectory()) return false;
    const size_t size = f.fileSize();
    if (size == 0 || size > cap) return false;
    out.resize(size);
    return f.read(out.data(), size) == static_cast<int>(size);
  }
  bool mkdir(const char* path, bool recursive = true) {
    std::error_code ec;
    return recursive ? std::filesystem::create_directories(path, ec) : std::filesystem::create_directory(path, ec);
  }
  bool remove(const char* path) {
    folders_.forget(path);
    if (HalFile::isOpenPath(path)) HalFile::removedWhileOpen.emplace_back(path);
    // error_code form: Windows refuses to unlink an open file and the throwing form would terminate the test.
    std::error_code ec;
    return std::filesystem::remove(path, ec) && !ec;
  }
  bool rename(const char* oldPath, const char* newPath) {
    folders_.forget(oldPath);
    folders_.forget(newPath);
    std::error_code ec;
    std::filesystem::rename(oldPath, newPath, ec);
    return !ec;
  }
  bool removeDir(const char* path) {
    folders_.forget(path);
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return !ec;
  }
  using oflag_t = int;
  HalFile open(const char* path, oflag_t = O_RDONLY) {
    HalFile f;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec) && !ec) {
      ++HalFile::folderOpens;
      f.openDir(path);
    } else if (refuses(path)) {
      refuse(f);
    } else if (f.openForRead(path)) {
      attach(f, path, false);
    }
    return f;
  }
  // Sorted for determinism: std::filesystem iteration order is unspecified and
  // the pipeline harness requires byte-identical behavior across runs.
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(path, ec)) {
      if (e.is_regular_file()) names.push_back(e.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    if (names.size() > static_cast<size_t>(maxFiles)) names.resize(maxFiles);
    std::vector<String> out;
    out.reserve(names.size());
    for (auto& n : names) out.push_back(String(std::move(n)));
    return out;
  }
  static HalStorage& getInstance() {
    static HalStorage i;
    return i;
  }

 private:
  cache_cipher::FolderState state(const char* path, size_t len, uint8_t (&key)[cache_cipher::kKeyBytes]) {
    const auto readMarker = [](const char* markerPath, uint8_t* bytes, size_t cap) -> int {
      std::error_code ec;
      if (!std::filesystem::exists(markerPath, ec)) return 0;
      FILE* fp = fopen(markerPath, "rb");
      if (fp == nullptr) return -1;
      const size_t got = fread(bytes, 1, cap, fp);
      fclose(fp);
      return got > 0 ? static_cast<int>(got) : -1;
    };
    return cache_cipher::resolve(folders_, path, len, keySource_, readMarker, key);
  }
  // HalStorage::cacheFolderRefuses.
  bool refuses(const char* path) {
    const size_t len = cache_cipher::folderLength(path);
    if (len == 0 || cache_cipher::isMarker(path, len)) return false;
    uint8_t key[cache_cipher::kKeyBytes];
    return state(path, len, key) == cache_cipher::FolderState::Unreadable;
  }
  static bool refuse(HalFile& f) {
    f.openForRead("");  // a failed open: closed, and closeable
    return false;
  }
  // HalStorage::attachCacheCipher: false when the open fails for it.
  bool attach(HalFile& f, const char* path, bool mayWrite) {
    const size_t len = cache_cipher::folderLength(path);
    if (len == 0 || cache_cipher::isMarker(path, len) || f.isDirectory()) return true;
    uint8_t key[cache_cipher::kKeyBytes];
    switch (state(path, len, key)) {
      case cache_cipher::FolderState::Plain:
        return true;
      case cache_cipher::FolderState::Enciphered:
        switch (f.attachCacheCipher(mayWrite, key)) {
          case cache_cipher::Attach::Plain:
          case cache_cipher::Attach::Enciphered:
            return true;
          case cache_cipher::Attach::Damaged:
            remove(path);  // HalStorage::attachCacheCipher: damaged cache, closed and removed
            return false;
          case cache_cipher::Attach::Failed:
            return false;  // the heap or the card refused, not the file: closed and kept
        }
        return false;
      case cache_cipher::FolderState::Unreadable:
        break;
    }
    f.failOpen();
    return false;
  }

  cache_cipher::FolderTable folders_;
  cache_cipher::KeySource keySource_ = nullptr;
};

#define Storage HalStorage::getInstance()
