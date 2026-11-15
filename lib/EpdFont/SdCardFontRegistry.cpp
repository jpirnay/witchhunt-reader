#include "SdCardFontRegistry.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ttf/TtfFamily.h>

#include <algorithm>
#include <cstring>
#include <numeric>

#include "CjkUiPacks.h"

// --- SdCardFontFamilyInfo helpers ---

const SdCardFontFileInfo* SdCardFontFamilyInfo::findFile(uint8_t size, uint8_t style) const {
  for (const auto& f : files) {
    if (f.pointSize == size && f.style == style) return &f;
  }
  return nullptr;
}

uint32_t SdCardFontFamilyInfo::filesKey() const {
  uint32_t key = glyphstore::fnv1a(name.c_str(), name.size() + 1);
  for (const auto& f : files) key = glyphstore::fnv1a(f.path.c_str(), f.path.size() + 1, key);
  return key;
}

bool SdCardFontFamilyInfo::hasSize(uint8_t size) const {
  for (const auto& f : files) {
    if (f.pointSize == size) return true;
  }
  return false;
}

std::vector<uint8_t> SdCardFontFamilyInfo::availableSizes() const {
  std::vector<uint8_t> sizes;
  if (ttf()) return sizes;  // drawn at any size, from one file per style
  for (const auto& f : files) {
    bool found = false;
    for (uint8_t s : sizes) {
      if (s == f.pointSize) {
        found = true;
        break;
      }
    }
    if (!found) sizes.push_back(f.pointSize);
  }
  std::sort(sizes.begin(), sizes.end());
  return sizes;
}

const SdCardFontFileInfo* SdCardFontFamilyInfo::pickClosestSize(uint8_t targetPtSize) const {
  if (ttf()) return nullptr;
  const SdCardFontFileInfo* selected = nullptr;
  int bestDiff = INT32_MAX;
  for (const auto& f : files) {
    int diff = std::abs(static_cast<int>(f.pointSize) - static_cast<int>(targetPtSize));
    // Strict < ensures the first scan wins on ties; then tie-break by smaller
    // pointSize to make the choice independent of filesystem enumeration order.
    if (diff < bestDiff || (diff == bestDiff && selected && f.pointSize < selected->pointSize)) {
      bestDiff = diff;
      selected = &f;
    }
  }
  return selected;
}

// --- SdCardFontRegistry ---

bool SdCardFontRegistry::parseFilename(const char* filename, uint8_t& size, uint8_t& style) {
  // V4 naming: <name>_<size>.cpfont (e.g. Bookerly-SD_14.cpfont)
  // Use an ends-with check rather than strstr() so that in-progress downloads
  // like "Foo_14.cpfont.tmp" or backups like "Foo_14.cpfont~" aren't accepted.
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  const size_t nameLen = strlen(filename);
  if (nameLen <= kExtLen) return false;
  if (strcmp(filename + nameLen - kExtLen, kExt) != 0) return false;
  const char* ext = filename + nameLen - kExtLen;

  size_t baseLen = ext - filename;
  if (baseLen == 0 || baseLen > 127) return false;

  char base[128];
  memcpy(base, filename, baseLen);
  base[baseLen] = '\0';

  char* lastUnderscore = strrchr(base, '_');
  if (!lastUnderscore || lastUnderscore == base) return false;

  const char* sizeStr = lastUnderscore + 1;
  char* endPtr;
  long sizeVal = strtol(sizeStr, &endPtr, 10);
  if (endPtr == sizeStr || *endPtr != '\0' || sizeVal < 1 || sizeVal > 255) return false;
  size = static_cast<uint8_t>(sizeVal);
  style = 0;
  return true;
}

// A directory of .ttf and .otf files: each file's style and weight come from its head and OS/2 tables, a style bit
// they leave out from its name (ttf::fileNameStyle), and the file of each style nearest the regular or bold weight
// serves it (ttf::assignStyles; without a regular file, its nearest style serves as the regular). Only a directory
// with a file the engine draws becomes a family.
void SdCardFontRegistry::addTtfFiles(const char* dirPath, const std::vector<std::string>& names,
                                     const std::vector<uint32_t>& bytes, SdCardFontFamilyInfo& family) const {
  std::vector<size_t> order(names.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::sort(order.begin(), order.end(), [&](const size_t a, const size_t b) { return names[a] < names[b]; });
  std::vector<ttf::FileProbe> probes(order.size());
  for (size_t i = 0; i < order.size(); ++i) {
    const std::string path = std::string(dirPath) + "/" + names[order[i]];
    HalFile file;
    if (!Storage.openFileForRead("SDREG", path.c_str(), file)) continue;
    probes[i] = ttf::probeFile(ttf::fileSource(file, bytes[order[i]]));
    if (probes[i].style < 0) {
      LOG_DBG("SDREG", "Not an outline font the reader draws: %s", path.c_str());
      continue;
    }
    // A style bit the file's tables leave out, its name may give: an Italic that declares itself regular would
    // otherwise tie with the Regular and, sorting first, serve as the body text's face.
    const int named = probes[i].style | ttf::fileNameStyle(names[order[i]].c_str());
    if (named != probes[i].style) {
      LOG_DBG("SDREG", "Style %d of %s from its name (its tables say %d)", named, path.c_str(), probes[i].style);
      probes[i].style = named;
    }
  }
  int chosen[4];
  if (!ttf::assignStyles(probes.data(), probes.size(), chosen)) {
    LOG_DBG("SDREG", "No outline-font style the engine draws in %s", dirPath);
    return;
  }
  uint32_t fileBytes[4];
  uint32_t sdBytes[4];
  uint8_t styleCount = 0;
  for (const int file : chosen) {
    if (file < 0) continue;
    // Flash mode copies the file and writes a shaping font's CPac behind it: the block, not the file, is what must fit.
    fileBytes[styleCount] = probes[static_cast<size_t>(file)].flashBytes;
    sdBytes[styleCount++] = probes[static_cast<size_t>(file)].sdBytes;
  }
  if (ttfFits_ && !ttfFits_(fileBytes, sdBytes, styleCount)) {
    LOG_INF("SDREG", "TTF family in %s does not fit the flash area, or there is none; not listed", dirPath);
    return;
  }
  for (uint8_t style = 0; style < 4; ++style) {
    if (chosen[style] < 0) continue;
    const size_t file = order[static_cast<size_t>(chosen[style])];
    SdCardFontFileInfo info;
    info.path = std::string(dirPath) + "/" + names[file];
    info.pointSize = 0;
    info.style = style;
    info.fileBytes = bytes[file];
    family.files.push_back(std::move(info));
  }
}

namespace {

// Calls `visit(ctx, name, bytes)` for each file of the folder `dirPath` that discovery reads: no folder, no hidden file
// ('.') or macOS resource fork ('_'). A function, not a template: one walk in flash for every caller.
void forEachFontFile(const char* dirPath, void (*visit)(void* ctx, const char* name, uint32_t bytes), void* ctx) {
  FsFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return;
  char nameBuffer[128];
  while (true) {
    FsFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    entry.getName(nameBuffer, sizeof(nameBuffer));
    const auto entryBytes = static_cast<uint32_t>(entry.fileSize());
    entry.close();
    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;
    visit(ctx, nameBuffer, entryBytes);
  }
  dir.close();
}

}  // namespace

void SdCardFontRegistry::scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family) const {
  struct Scan {
    const char* dirPath;
    SdCardFontFamilyInfo& family;
    std::vector<std::string> ttfNames;
    std::vector<uint32_t> ttfBytes;
  } scan{dirPath, family, {}, {}};
  forEachFontFile(
      dirPath,
      [](void* ctx, const char* name, const uint32_t entryBytes) {
        Scan& scan = *static_cast<Scan*>(ctx);
        uint8_t size, style;
        if (!parseFilename(name, size, style)) {
          if (ttf::isOutlineFontFileName(name)) {
            scan.ttfNames.emplace_back(name);
            scan.ttfBytes.push_back(entryBytes);
          }
          return;
        }

        SdCardFontFileInfo info;
        info.path = std::string(scan.dirPath) + "/" + name;
        info.pointSize = size;
        info.style = style;
        info.fileBytes = entryBytes;
        scan.family.files.push_back(std::move(info));
      },
      &scan);
  if (family.files.empty() && !scan.ttfNames.empty()) addTtfFiles(dirPath, scan.ttfNames, scan.ttfBytes, family);
}

SdCardFontRegistry::FolderFonts SdCardFontRegistry::folderFonts(const char* dirPath) {
  FolderFonts held;
  forEachFontFile(
      dirPath,
      [](void* ctx, const char* name, uint32_t) {
        FolderFonts& held = *static_cast<FolderFonts*>(ctx);
        uint8_t size, style;
        if (parseFilename(name, size, style)) {
          held.cpfont = true;
        } else if (ttf::isOutlineFontFileName(name)) {
          held.outline = true;
        }
      },
      &held);
  return held;
}

bool SdCardFontRegistry::drawsOutlineFile(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("SDREG", path, file)) return false;
  const ttf::FileProbe probe = ttf::probeFile(ttf::fileSource(file, static_cast<uint32_t>(file.fileSize())));
  file.close();
  return probe.style >= 0;
}

namespace {

// Whether the folder `name` in a fonts root is read as a family.
bool readsAsFamilyFolder(const char* name) {
  // Hidden and system folders (macOS ._*, .Trashes, ...).
  if (name[0] == '\0' || name[0] == '.' || name[0] == '_') return false;
  // The CJK UI packs draw the interface's CJK (src/CjkUiPack.cpp): no reading fonts.
  if (cjk_ui_pack::isFamily(name)) return false;
  // In-flight install and rollback folders (Bookerly__staging, Bookerly__backup, ...) may hold valid-looking .cpfont
  // files during a resumable download, but must not be offered as families.
  const size_t nameLen = strlen(name);
  for (const char* suffix : {"__staging", "__backup"}) {
    const size_t suffixLen = strlen(suffix);
    if (nameLen > suffixLen && strcmp(name + nameLen - suffixLen, suffix) == 0) return false;
  }
  return true;
}

// Calls `visit(ctx, name)` for each folder of the root `rootPath` read as a family, until it returns false; false then.
// A missing root is logged by the read of the list only: a book's lookups would log it at every open.
bool forEachFolderIn(const char* rootPath, bool (*visit)(void* ctx, const char* name), void* ctx,
                     const bool logMissing = false) {
  FsFile root = Storage.open(rootPath);
  if (!root) {
    if (logMissing) {
      LOG_DBG("SDREG", "Fonts directory not found: %s", rootPath);
    }
    return true;
  }
  if (!root.isDirectory()) {
    LOG_ERR("SDREG", "Fonts path is not a directory: %s", rootPath);
    root.close();
    return true;
  }
  char nameBuffer[128];
  bool more = true;
  while (more) {
    FsFile entry = root.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (isDirectory) entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();
    if (isDirectory && readsAsFamilyFolder(nameBuffer)) more = visit(ctx, nameBuffer);
  }
  root.close();
  return more;
}

}  // namespace

void SdCardFontRegistry::scanRoot(const char* rootPath) const {
  struct Scan {
    const SdCardFontRegistry& registry;
    const char* rootPath;
  } scan{*this, rootPath};
  forEachFolderIn(
      rootPath,
      [](void* ctx, const char* name) {
        const Scan& scan = *static_cast<const Scan*>(ctx);
        auto& families = scan.registry.families_;
        // Primary wins: a family an earlier root holds is not read again.
        for (const auto& f : families) {
          if (f.name == name) {
            LOG_DBG("SDREG", "Skipping duplicate family %s in %s (primary wins)", name, scan.rootPath);
            return true;
          }
        }
        SdCardFontFamilyInfo family;
        family.name = name;
        const std::string subDirPath = std::string(scan.rootPath) + "/" + name;
        scan.registry.scanDirectory(subDirPath.c_str(), family);
        if (!family.files.empty()) {
          families.push_back(std::move(family));
          LOG_DBG("SDREG", "Found %s family: %s (%d files) in %s", families.back().ttf() ? "TTF" : ".cpfont",
                  families.back().name.c_str(), static_cast<int>(families.back().files.size()), scan.rootPath);
        }
        return true;
      },
      &scan, /*logMissing=*/true);
}

void SdCardFontRegistry::readList(const bool keep, const bool rename) const {
  families_.clear();
  families_.reserve(MAX_SD_FAMILIES);

  // Primary first so it wins on family-name conflicts; the upstream roots are
  // read-only fallbacks — installers/deleters always target only the primary.
  for (uint8_t r = 0; r < rootCount_; ++r) scanRoot(roots_[r]);

  // Sort families alphabetically
  std::sort(families_.begin(), families_.end(),
            [](const SdCardFontFamilyInfo& a, const SdCardFontFamilyInfo& b) { return a.name < b.name; });

  // Cap at MAX_SD_FAMILIES
  if (static_cast<int>(families_.size()) > MAX_SD_FAMILIES) {
    families_.resize(MAX_SD_FAMILIES);
  }

  // Kept otherwise: a name() handed out stays valid across a read on first use. Written in place, into the room
  // reserveNames() took at boot.
  if (rename || !named_.load(std::memory_order_relaxed)) {
    names_.clear();
    nameOffsets_.clear();
    for (const auto& family : families_) {
      nameOffsets_.push_back(static_cast<uint16_t>(names_.size()));
      names_.append(family.name.c_str(), family.name.size() + 1);
    }
    named_.store(true, std::memory_order_release);
  }

  LOG_DBG("SDREG", "Discovery complete: %d families%s", static_cast<int>(families_.size()), keep ? "" : ", names kept");
  if (keep) {
    listed_.store(true, std::memory_order_release);
  } else {
    std::vector<SdCardFontFamilyInfo>().swap(families_);
  }
}

bool SdCardFontRegistry::discover() {
  const std::lock_guard<std::mutex> guard(mutex_);
  readList(/*keep=*/true, /*rename=*/true);
  generation_.fetch_add(1, std::memory_order_acq_rel);
  return !families_.empty();
}

void SdCardFontRegistry::ensureListed() const {
  if (listed()) return;
  const std::lock_guard<std::mutex> guard(mutex_);
  if (!listed()) readList(/*keep=*/true, /*rename=*/false);
}

void SdCardFontRegistry::readNames() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (!named_.load(std::memory_order_relaxed)) readList(/*keep=*/listed(), /*rename=*/true);
}

void SdCardFontRegistry::release() {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (!listed()) return;
  listed_.store(false, std::memory_order_release);
  std::vector<SdCardFontFamilyInfo>().swap(families_);  // shrink_to_fit() frees nothing under -fno-exceptions
  LOG_DBG("SDREG", "Family list released");
}

bool SdCardFontRegistry::findOnCard(const char* name, SdCardFontFamilyInfo& family) const {
  family.name.clear();
  std::vector<SdCardFontFileInfo>().swap(family.files);
  if (!name || !readsAsFamilyFolder(name)) return false;
  for (uint8_t r = 0; r < rootCount_; ++r) {
    // The root's own entry for the name, as the list reads it: an open by path would also take a folder whose name
    // differs in case, which the list, comparing names exactly, never offers.
    struct Find {
      const char* name;
      bool found;
    } find{name, false};
    forEachFolderIn(
        roots_[r],
        [](void* ctx, const char* folder) {
          Find& find = *static_cast<Find*>(ctx);
          find.found = strcmp(folder, find.name) == 0;
          return !find.found;
        },
        &find);
    if (!find.found) continue;
    const std::string dir = std::string(roots_[r]) + "/" + name;
    scanDirectory(dir.c_str(), family);
    if (!family.files.empty()) {
      family.name = name;
      return true;
    }
  }
  return false;
}

void SdCardFontRegistry::forEachFamilyFolder(bool (*visit)(void* ctx, const char* name), void* ctx) const {
  for (uint8_t r = 0; r < rootCount_; ++r) {
    if (!forEachFolderIn(roots_[r], visit, ctx)) return;
  }
}

const SdCardFontFamilyInfo* SdCardFontRegistry::findFamily(const std::string& name) const {
  for (const auto& f : getFamilies()) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

int SdCardFontRegistry::nameIndex(const char* const name) const {
  if (!name) return -1;
  const uint8_t count = nameCount();
  for (uint8_t i = 0; i < count; ++i) {
    if (strcmp(names_.c_str() + nameOffsets_[i], name) == 0) return i;
  }
  return -1;
}

int SdCardFontRegistry::getFamilyIndex(const std::string& name) const {
  const auto& families = getFamilies();
  for (int i = 0; i < static_cast<int>(families.size()); i++) {
    if (families[i].name == name) return i;
  }
  return -1;
}
