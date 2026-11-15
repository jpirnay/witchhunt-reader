#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct SdCardFontFileInfo {
  std::string path;   // v4 on-disk naming: "/.crosspoint/fonts/<Family>/<Family>_<size>.cpfont"
                      // e.g. "/.crosspoint/fonts/NotoSansCJK/NotoSansCJK_14.cpfont"
  uint8_t pointSize;  // parsed from filename: 14; 0 for a TTF file, which serves every size
  uint8_t style;      // always 0 in v4 (all 4 styles bundled in one file);
                      // a TTF file's head.macStyle: EpdFontFamily::Style
  // Byte count, read from the directory entry during discovery. Free here and
  // it saves an open() everywhere a caller only needs to tell whether the file
  // changed -- the font preview cache keys on it.
  uint32_t fileBytes = 0;
};

struct SdCardFontFamilyInfo {
  std::string name;  // directory name, e.g. "NotoSansCJK"
  std::vector<SdCardFontFileInfo> files;
  // A TrueType or OpenType CFF family: one .ttf or .otf file per style, regular among them, every size drawn from
  // them on the device (TtfFont). A directory with any .cpfont file is a .cpfont family, whatever else it holds.
  // Derived, not stored: a TTF file is listed with pointSize 0, which no .cpfont name can give, and a field
  // here would cost the registry's 128 reserved records heap for good.
  bool ttf() const { return !files.empty() && files.front().pointSize == 0; }
  // Which family this is: FNV-1a over its name and its files' paths. A book's family is named as the book declares
  // it, so two books' families can share a name; their files, in each book's cache, tell them apart.
  uint32_t filesKey() const;

  const SdCardFontFileInfo* findFile(uint8_t size, uint8_t style = 0) const;
  bool hasSize(uint8_t size) const;
  std::vector<uint8_t> availableSizes() const;

  // Pick the file whose pointSize is closest to targetPtSize. On ties (equal
  // distance) prefers the smaller pointSize so behaviour is deterministic
  // across SD card layouts. Returns nullptr when files is empty, and for a TTF
  // family, which has no per-size files (callers that need one skip it).
  const SdCardFontFileInfo* pickClosestSize(uint8_t targetPtSize) const;
};

// The registry reserves MAX_SD_FAMILIES records at every discovery and keeps them: a field here is that many times
// permanent heap, so whether a family is TTF is derived from its files instead.
static_assert(sizeof(SdCardFontFamilyInfo) == sizeof(std::string) + sizeof(std::vector<SdCardFontFileInfo>),
              "a family record costs only its name and its files");

class SdCardFontRegistry {
 public:
  static constexpr int MAX_SD_FAMILIES = 128;
  // Primary (writable) location. All installs/downloads/deletes target this root.
  static constexpr const char* FONTS_DIR = "/.crosspoint/fonts";
  // Read-only fallback roots for upstream crosspoint-reader compatibility.
  // Upstream uses "/.fonts" (preferred, hidden) and "/fonts" (visible). Both are
  // scanned after the primary; primary wins on family-name conflicts.
  static constexpr const char* FONTS_DIR_UPSTREAM_HIDDEN = "/.fonts";
  static constexpr const char* FONTS_DIR_UPSTREAM_VISIBLE = "/fonts";
  static constexpr const char* const kRoots[] = {FONTS_DIR, FONTS_DIR_UPSTREAM_HIDDEN, FONTS_DIR_UPSTREAM_VISIBLE};

  // Reads its families from `roots`, in that order: the card's three by default, a folder of their own in host tests.
  explicit SdCardFontRegistry(const char* const* roots = kRoots, uint8_t rootCount = 3)
      : roots_(roots), rootCount_(rootCount) {}
  SdCardFontRegistry(const SdCardFontRegistry&) = delete;
  SdCardFontRegistry& operator=(const SdCardFontRegistry&) = delete;

  // Reads the list from the card again. True if it holds any family.
  bool discover();
  // The list is read when first asked for (getFamilies(), findFamily(), getFamilyIndex(), getFamilyCount()) and kept
  // until release(). It cost the X3 ~12 KB of heap and 0.75 s of its boot for 23 families (2026-11-15), and a book
  // needs only the families it draws: SdCardFontSystem reads those one at a time (findOnCard) and drops the list at
  // each activity change. Under the render lock with no other task in the list, as at an activity change.
  void release();
  // The list is in memory: asking for it reads nothing.
  bool listed() const { return listed_.load(std::memory_order_acquire); }
  // Rises at each discover(), the read an install or a removal asks for: a family read from the card before a rise may
  // be out of date. The read on first use leaves it.
  uint32_t generation() const { return generation_.load(std::memory_order_acquire); }
  // The family `name` as the list would hold it, read from its folder alone: the first root with a family of that
  // name, by the rules the list is read by. False, `family` emptied, when the list would have none of that name.
  bool findOnCard(const char* name, SdCardFontFamilyInfo& family) const;
  // Calls `visit(ctx, name)` for each folder the list is read from, until it returns false, and reads no file: a folder
  // that holds no font the reader draws is visited too. A name more than one root holds comes once per root.
  void forEachFamilyFolder(bool (*visit)(void* ctx, const char* name), void* ctx) const;

  // Asked at discovery whether a TTF family with these styles can be installed, from their file sizes and their
  // SD-mode blocks' (ttf::FileProbe::sdBytes); one that cannot is not listed, since picking it would only fall back to
  // a built-in font. Unset (host tests): every family is listed.
  using TtfFits = bool (*)(const uint32_t* fileBytes, const uint32_t* sdBytes, uint8_t count);
  void setTtfFits(const TtfFits fits) { ttfFits_ = fits; }

  // One family directory outside the fonts roots (a book's embedded fonts, see BookFonts), read as discover()
  // reads one; `family.name` is left to the caller. False when it holds no family the reader can draw.
  bool scanFamily(const char* dirPath, SdCardFontFamilyInfo& family) const {
    family.files.clear();
    scanDirectory(dirPath, family);
    return !family.files.empty();
  }

  // What the family folder `dirPath` holds as discovery reads it: a .cpfont font (`cpfont`), a .ttf or .otf file
  // (`outline`). The web upload asks before a file joins it (FontDownloadRules::joinsFolder()).
  struct FolderFonts {
    bool cpfont = false;
    bool outline = false;
  };
  static FolderFonts folderFonts(const char* dirPath);

  // Whether the .ttf or .otf file at `path` is one discovery lists: ttf::probeFile() reads it as a font the engine
  // draws. The web upload asks before an uploaded file replaces one.
  static bool drawsOutlineFile(const char* path);

  const std::vector<SdCardFontFamilyInfo>& getFamilies() const {
    ensureListed();
    return families_;
  }
  const SdCardFontFamilyInfo* findFamily(const std::string& name) const;
  int getFamilyIndex(const std::string& name) const;
  int getFamilyCount() const { return static_cast<int>(getFamilies().size()); }

  // The families' names in the list's order, kept past release(): the settings rows, the reader menu and the reader
  // bars count and label their font options by them alone (~0.4 KB for 23 families, against ~12 KB for the list), so a
  // menu opened in a book does not read the list again. Read with the list, and again with each read of it; asked for
  // first, the list is read for them and not kept.
  uint8_t nameCount() const {
    ensureNamed();
    return static_cast<uint8_t>(nameOffsets_.size());
  }
  // Family `i`'s name, "" past the end. Valid until discover(): a read of the list on first use keeps the names it
  // finds, which are the same while the card is.
  const char* name(uint8_t i) const {
    ensureNamed();
    return i < nameOffsets_.size() ? names_.c_str() + nameOffsets_[i] : "";
  }
  // The index of the family called `name`, -1 when there is none.
  int nameIndex(const char* name) const;
  // Takes the names' room now, at boot: read later into blocks of their own, they split the largest free block for
  // good (X3: Home's 61,428 B one fell to 49,140 B after a visit to the font picker, 2026-11-16). More names than it
  // holds grow it then.
  void reserveNames(size_t bytes, size_t count) const {
    names_.reserve(bytes);
    nameOffsets_.reserve(count);
  }

 private:
  const char* const* roots_;
  uint8_t rootCount_;
  // Read on first use, so mutable: the list is what the card holds, whoever asks first.
  mutable std::vector<SdCardFontFamilyInfo> families_;  // sorted alphabetically
  mutable std::atomic<bool> listed_{false};
  mutable std::atomic<uint32_t> generation_{0};
  mutable std::mutex mutex_;  // the first use, from the loop or the render task
  // name(): the names one after another, each with its NUL, and where each starts.
  mutable std::string names_;
  mutable std::vector<uint16_t> nameOffsets_;
  mutable std::atomic<bool> named_{false};

  void ensureListed() const;
  void ensureNamed() const {
    if (!named_.load(std::memory_order_acquire)) readNames();
  }
  void readNames() const;
  // With mutex_ held: families_ read from the card, and the names when there are none yet or `rename` (discover()).
  // `keep`: the list is in memory from here (listed()), else families_ is emptied again before anyone else may look.
  void readList(bool keep, bool rename) const;
  static bool parseFilename(const char* filename, uint8_t& size, uint8_t& style);
  void scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family) const;
  void addTtfFiles(const char* dirPath, const std::vector<std::string>& names, const std::vector<uint32_t>& bytes,
                   SdCardFontFamilyInfo& family) const;
  TtfFits ttfFits_ = nullptr;
  void scanRoot(const char* rootPath) const;
};
