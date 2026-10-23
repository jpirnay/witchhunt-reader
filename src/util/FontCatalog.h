#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <memory>

// The font manifest's families, their files and every string they hold, in ONE heap block of exactly
// the size they need. The Font Manager keeps the list while the user browses it and frees it before a
// download. As a vector of families -- two std::strings and a vector of files each, a std::string per
// file -- it was about 300 small allocations (19 KB on an X3), interleaved with whatever else allocated
// meanwhile. Freeing them gave the bytes back but not a block big enough for wolfSSL's 16.7 KB TLS
// record, and the download failed. One block comes back in one piece.
//
// Layout, in the host's byte order (a stash never leaves the device that wrote it):
//
//   header   16 bytes   u32 magic 'CPFC', u16 version, u16 families, u16 files, u16 stringBytes,
//                       u32 blockBytes (the whole block, header included)
//   families 12 bytes   u32 totalSize, u16 name, u16 description, u16 firstFile, u8 flags, u8 0
//                       flags: installed, update, resumable (set after the build); outline, not in "Download all"
//   files    12 bytes   u32 size, u32 crc32, u16 name, u8 hasCrc32, u8 0
//   strings             every string NUL-terminated; name and description fields are offsets here
//
// A family's files run from its firstFile to the next family's (or to the end). The 16-bit counts
// and offsets bound a catalog at 65535 families, files and string bytes: a block that size would
// never be allocated on the C3 in the first place.
//
// Built by a Builder in two passes over the same events: one counts, then one fills a block allocated
// for exactly that count. Read by index; the strings are const char* into the block, valid until the
// catalog is cleared, replaced or destroyed. Only the three per-family flags change after the build.
// An index out of range reads as "" / 0 / false, and setting a flag on one does nothing.
class FontCatalog {
 public:
  struct Size {
    size_t families = 0;
    size_t files = 0;
    size_t stringBytes = 0;  // every string with its terminator
    bool operator==(const Size&) const = default;
  };

  class Builder;

  // Which families copyFrom() keeps. ctx is the caller's, passed through.
  using FamilyFilter = bool (*)(const void* ctx, const FontCatalog& from, size_t family);

  // The block a catalog of `size` takes, header included.
  static size_t blockBytesFor(const Size& size);
  // Whether `size` fits the block's 16-bit counts and offsets.
  static bool fits(const Size& size);

  FontCatalog() = default;
  FontCatalog(const FontCatalog&) = delete;
  FontCatalog& operator=(const FontCatalog&) = delete;
  FontCatalog(FontCatalog&&) noexcept = default;
  FontCatalog& operator=(FontCatalog&&) noexcept = default;
  void swap(FontCatalog& other) noexcept { block.swap(other.block); }

  // Drops what the catalog held, then allocates the block for `size` -- none for zero families -- for
  // a Builder to fill. False, the catalog left empty, when `size` does not fit the format or the heap.
  bool allocate(const Size& size);
  // Frees the block.
  void clear() { block.reset(); }

  size_t count() const;
  bool empty() const { return count() == 0; }
  // Files across all families.
  size_t totalFiles() const;
  // The block's size, header included; 0 when empty.
  size_t blockBytes() const;

  const char* name(size_t family) const;
  const char* description(size_t family) const;  // "" when the manifest gives none
  size_t fileCount(size_t family) const;
  size_t totalSize(size_t family) const;  // the sum of its files' sizes

  // As the manifest gives it, e.g. "Alegreya/Alegreya_10.cpfont": what the download URL ends in.
  const char* fileName(size_t family, size_t file) const;
  // fileName() without a leading "<family name>/": the file's name inside the family's folder.
  const char* fileLocalName(size_t family, size_t file) const;
  uint32_t fileSize(size_t family, size_t file) const;
  uint32_t fileCrc32(size_t family, size_t file) const;  // 0 when !fileHasCrc32()
  bool fileHasCrc32(size_t family, size_t file) const;   // false for a v1 manifest: size check only

  // A TrueType or CFF family (the manifest's format "ttf" or "otf"): one file per style, every size drawn from it.
  bool outline(size_t family) const;
  // "Download all" takes it (the manifest's inDownloadAll; the CJK families say no). False out of range.
  bool inDownloadAll(size_t family) const;

  bool installed(size_t family) const;
  bool hasUpdate(size_t family) const;
  // A __staging folder from an interrupted download exists: the next download resumes it.
  bool hasResumableDownload(size_t family) const;
  void setInstalled(size_t family, bool value);
  void setHasUpdate(size_t family, bool value);
  void setHasResumableDownload(size_t family, bool value);

  // Replaces this catalog with the families of `from` that `keep` selects (all of them when keep is
  // null), flags included, in a block of their own. Zero selected leaves it empty, and is not a
  // failure. False, the catalog left empty, when the block cannot be allocated or `from` is this.
  bool copyFrom(const FontCatalog& from, FamilyFilter keep, const void* ctx);
  // copyFrom() for the one family at `family`; false also when there is no such family.
  bool copyFamilyFrom(const FontCatalog& from, size_t family);

  // The whole block, in one write. False when empty or the write falls short.
  bool writeTo(HalFile& file) const;
  // Replaces this catalog with what writeTo() wrote to `file`, read from its start in one read into
  // one new block. False, the catalog left empty, when the file is short, long, not a catalog, or
  // does not hold together (an offset past the strings, files out of order, a total that does not
  // add up), or the block cannot be allocated.
  bool readFrom(HalFile& file);

 private:
  struct Header;
  struct FamilyRecord;
  struct FileRecord;

  const Header* header() const;
  const FamilyRecord* familyRecords() const;
  FamilyRecord* familyRecords();
  const FileRecord* fileRecords() const;
  FileRecord* fileRecords();
  const char* strings() const;
  char* strings();
  // The index into fileRecords() of `file` in `family`, or SIZE_MAX when there is none.
  size_t fileIndex(size_t family, size_t file) const;
  void setFlag(size_t family, uint8_t flag, bool value);
  bool flag(size_t family, uint8_t flag) const;
  // Whether `bytes` read back from a stash are a catalog every accessor can read safely.
  static bool holdsTogether(const uint8_t* data, size_t bytes);

  std::unique_ptr<uint8_t[]> block;
};

// Builds a FontCatalog family by family. Run it twice over the same events: first with no catalog,
// when it only counts (size() is then what to allocate), then with the catalog allocated for that
// size, when it writes. Both passes do the same arithmetic, so the second fills the block exactly.
//
// A family is open from beginFamily() until keepFamily() or dropFamily(). Its description and files
// go into the block as they come, because the manifest may give them before the name or a file that
// rules the family out; dropFamily() takes them back. Strings stop at an embedded NUL, in both passes.
//
// The writing pass never writes past the block. Anything that would not fit -- the events changed
// between the passes -- marks the family, and keeping a marked family drops it and sets overflowed().
class FontCatalog::Builder {
 public:
  Builder() = default;
  explicit Builder(FontCatalog& target) : target(&target) {}

  void beginFamily();
  // The last one given wins.
  void description(const char* text, size_t len);
  // The open family's kind and its place in "Download all"; .cpfont and in it until said otherwise.
  void outline(bool value);
  void inDownloadAll(bool value);
  void addFile(const char* name, size_t nameLen, uint32_t size, uint32_t crc32, bool hasCrc32);
  void keepFamily(const char* name, size_t nameLen);
  void dropFamily();

  // What the families kept so far take.
  const Size& size() const { return kept; }
  bool overflowed() const { return overflow; }

 private:
  // Adds `text` to the strings, at `offset` when writing. False when it does not fit the block.
  bool putString(const char* text, size_t len, uint16_t& offset);

  FontCatalog* target = nullptr;
  Size kept;
  // `kept` plus what the open family has added.
  Size open;
  uint32_t familyTotal = 0;
  uint16_t descriptionOffset = 0;
  bool hasDescription = false;
  uint8_t familyFlags = 0;
  bool familyOverflow = false;
  bool overflow = false;
};
