#pragma once

// In-memory FAT card for WebDAVHandlerTest. Shadows test/shims/HalStorage.h, which has no folders and no files.
//
// It opens names the way SdFat does where the handler's protection depends on it: without regard to case, after
// dropping leading spaces and trailing dots and spaces, and through an entry's 8.3 alias (alias()), as FatFile::open
// compares a name with the short entry when no long name matches. getName() gives the long name. A file's reads can
// be made to fail from an offset on (failReadsFrom()), as a card read error does.

#include <WString.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

class HalFile {
 public:
  HalFile() = default;
  HalFile(HalFile&& other) noexcept { *this = std::move(other); }
  HalFile& operator=(HalFile&& other) noexcept {
    key_ = std::move(other.key_);
    open_ = other.open_;
    position_ = other.position_;
    nextChild_ = other.nextChild_;
    other.open_ = false;
    return *this;
  }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  operator bool() const { return open_; }
  bool isDirectory() const;
  size_t size() const;
  int available() const;
  int read(void* buffer, size_t bytes);
  size_t write(const uint8_t* buffer, size_t bytes);
  size_t getName(char* name, size_t len) const;
  HalFile openNextFile();
  // Device parity: the HAL asserts on closing a handle nothing opened.
  bool close() {
    if (key_.empty()) {
      std::fprintf(stderr, "HalFile::close() on a handle that was never opened: this aborts on device\n");
      std::abort();
    }
    open_ = false;
    return true;
  }

 private:
  friend class HalStorage;
  std::string key_;  // the entry, by HalStorage::key; empty when nothing was opened
  bool open_ = false;
  size_t position_ = 0;
  size_t nextChild_ = 0;
};

using FsFile = HalFile;

class HalStorage {
 public:
  struct Entry {
    std::string name;  // the long name, as made
    std::string content;
    bool folder = false;
    long failReadsFrom = -1;  // reads at or past this offset fail; -1: none do
    uint64_t made = 0;        // creation order, which a folder lists in
  };

  // A name as SdFat opens it, lower-cased: how names are compared here.
  static std::string fold(const std::string& name) {
    size_t first = 0;
    while (first < name.size() && name[first] == ' ') first++;
    size_t last = name.size();
    while (last > first && (name[last - 1] == '.' || name[last - 1] == ' ')) last--;
    std::string folded = name.substr(first, last - first);
    for (char& c : folded) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return folded;
  }
  static std::string join(const std::string& parent, const std::string& name) {
    return parent == "/" ? "/" + name : parent + "/" + name;
  }
  static std::vector<std::string> segments(const std::string& path) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= path.size()) {
      size_t end = path.find('/', pos);
      if (end == std::string::npos) end = path.size();
      const std::string name = fold(path.substr(pos, end - pos));
      if (!name.empty()) out.push_back(name);
      pos = end + 1;
    }
    return out;
  }

  void reset() {
    entries.clear();
    aliases.clear();
    made_ = 0;
  }
  // Makes the folders on the way as needed.
  void putFolder(const std::string& path) { make(path, true, ""); }
  void put(const std::string& path, const std::string& content) { make(path, false, content); }
  // `aliasPath` (a parent's real path plus the alias) opens the entry at `realPath`.
  void alias(const std::string& aliasPath, const std::string& realPath) {
    const auto real = find(realPath);
    if (!real) std::abort();
    aliases[key(aliasPath)] = *real;
  }
  void failReadsFrom(const std::string& path, const long offset) { entry(path).failReadsFrom = offset; }
  // Content of the file at `path`; "<none>" when there is none.
  std::string content(const std::string& path) const {
    const auto found = find(path);
    return found ? entries.at(*found).content : "<none>";
  }

  // The key of the entry `path` opens, if any: each name matched as SdFat matches it, long name or alias.
  std::optional<std::string> find(const std::string& path) const {
    std::string at = "/";
    for (const std::string& name : segments(path)) {
      const std::string next = join(at, name);
      if (entries.count(next) != 0) {
        at = next;
        continue;
      }
      const auto alias = aliases.find(next);
      if (alias == aliases.end()) return std::nullopt;
      at = alias->second;
    }
    return at;
  }

  bool exists(const char* path) { return find(path).has_value(); }
  HalFile open(const char* path, int = 0) {
    HalFile file;
    file.key_ = "?";  // tried, like the HAL's handle on a failed open: close() is legal on it
    const auto found = find(path);
    if (found) {
      file.key_ = *found;
      file.open_ = true;
    }
    return file;
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    file = HalFile();
    file.key_ = "?";
    const auto parent = find(parentOf(path));
    const std::vector<std::string> names = segments(path);
    if (!parent || names.empty() || !isFolderKey(*parent)) return false;
    const std::string k = join(*parent, names.back());
    if (entries.count(k) != 0 && entries[k].folder) return false;
    Entry& e = entries[k];
    e = Entry{lastName(path), "", false, -1, made_++};
    file.key_ = k;
    file.open_ = true;
    return true;
  }
  bool openFileForWrite(const char* module, const std::string& path, HalFile& file) {
    return openFileForWrite(module, path.c_str(), file);
  }
  bool openFileForWrite(const char* module, const String& path, HalFile& file) {
    return openFileForWrite(module, path.c_str(), file);
  }
  bool mkdir(const char* path, bool = true) {
    if (exists(path)) return false;
    make(path, true, "");
    return true;
  }
  // A rename onto a name that is taken fails, as SdFat's does.
  bool rename(const char* from, const char* to) {
    const auto source = find(from);
    const auto parent = find(parentOf(to));
    const std::vector<std::string> names = segments(to);
    if (!source || !parent || names.empty() || exists(to)) return false;
    const std::string target = join(*parent, names.back());
    std::map<std::string, Entry> moved;
    const std::string prefix = *source + "/";
    for (auto it = entries.begin(); it != entries.end();) {
      if (it->first == *source || it->first.rfind(prefix, 0) == 0) {
        moved[target + it->first.substr(source->size())] = it->second;
        it = entries.erase(it);
      } else {
        ++it;
      }
    }
    moved[target].name = lastName(to);
    entries.insert(moved.begin(), moved.end());
    return true;
  }
  bool remove(const char* path) {
    const auto found = find(path);
    if (!found || entries[*found].folder) return false;
    entries.erase(*found);
    return true;
  }
  bool rmdir(const char* path) {
    const auto found = find(path);
    if (!found || !entries[*found].folder || !childrenOf(*found).empty()) return false;
    entries.erase(*found);
    return true;
  }
  bool removeDir(const char* path) {
    const auto found = find(path);
    if (!found || !entries[*found].folder) return false;
    const std::string prefix = *found + "/";
    for (auto it = entries.begin(); it != entries.end();) {
      it = (it->first == *found || it->first.rfind(prefix, 0) == 0) ? entries.erase(it) : std::next(it);
    }
    return true;
  }
  bool areSameFile(const char* first, const char* second, bool& same) {
    const auto a = find(first);
    const auto b = find(second);
    same = a && b && *a == *b;
    return a && b;
  }

  // The children of the folder at `k`, in the order they were made.
  std::vector<std::string> childrenOf(const std::string& k) const {
    std::vector<std::string> children;
    for (const auto& [childKey, e] : entries) {
      if (childKey != k && parentOf(childKey) == k) children.push_back(childKey);
    }
    std::sort(children.begin(), children.end(),
              [this](const std::string& a, const std::string& b) { return entries.at(a).made < entries.at(b).made; });
    return children;
  }
  bool isFolderKey(const std::string& k) const { return k == "/" || (entries.count(k) != 0 && entries.at(k).folder); }

  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  std::map<std::string, Entry> entries;        // by key: the real path, folded
  std::map<std::string, std::string> aliases;  // folded alias path -> key it opens

 private:
  static std::string key(const std::string& path) {
    std::string k = "/";
    for (const std::string& name : segments(path)) k = join(k, name);
    return k;
  }
  static std::string parentOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
  }
  static std::string lastName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }
  Entry& entry(const std::string& path) {
    const auto found = find(path);
    if (!found) std::abort();
    return entries[*found];
  }
  // `path` as a test spells it: "/Folder/name", no empty or trimmed segments.
  void make(const std::string& path, const bool folder, const std::string& content) {
    std::string at = "/";
    size_t pos = 1;
    while (pos <= path.size()) {
      size_t end = path.find('/', pos);
      if (end == std::string::npos) end = path.size();
      const std::string name = path.substr(pos, end - pos);
      pos = end + 1;
      at = join(at, fold(name));
      const bool last = pos > path.size();
      if (entries.count(at) == 0 || last) entries[at] = Entry{name, last ? content : "", !last || folder, -1, made_++};
    }
  }

  uint64_t made_ = 0;
};

#define Storage HalStorage::getInstance()

inline bool HalFile::isDirectory() const { return open_ && Storage.isFolderKey(key_); }
inline size_t HalFile::size() const { return open_ && !isDirectory() ? Storage.entries.at(key_).content.size() : 0; }
inline int HalFile::available() const { return static_cast<int>(size() - std::min(size(), position_)); }
inline int HalFile::read(void* buffer, const size_t bytes) {
  if (!open_ || isDirectory()) return -1;
  const HalStorage::Entry& e = Storage.entries.at(key_);
  if (e.failReadsFrom >= 0 && position_ >= static_cast<size_t>(e.failReadsFrom)) return -1;
  size_t got = std::min(bytes, e.content.size() - std::min(e.content.size(), position_));
  if (e.failReadsFrom >= 0) got = std::min(got, static_cast<size_t>(e.failReadsFrom) - position_);
  std::memcpy(buffer, e.content.data() + position_, got);
  position_ += got;
  return static_cast<int>(got);
}
inline size_t HalFile::write(const uint8_t* buffer, const size_t bytes) {
  if (!open_ || isDirectory()) return 0;
  Storage.entries.at(key_).content.append(reinterpret_cast<const char*>(buffer), bytes);
  return bytes;
}
inline size_t HalFile::getName(char* name, const size_t len) const {
  const std::string real = key_ == "/" ? "/" : Storage.entries.at(key_).name;
  if (!open_ || real.size() + 1 > len) {
    if (len > 0) name[0] = '\0';
    return 0;
  }
  std::memcpy(name, real.c_str(), real.size() + 1);
  return real.size();
}
inline HalFile HalFile::openNextFile() {
  HalFile next;
  next.key_ = "?";
  if (!open_ || !isDirectory()) return next;
  const std::vector<std::string> children = Storage.childrenOf(key_);
  if (nextChild_ >= children.size()) return next;
  next.key_ = children[nextChild_++];
  next.open_ = true;
  return next;
}
