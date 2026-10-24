#include "FontCatalog.h"

#include <Memory.h>

#include <cstring>

// The layout FontCatalog.h describes. Every record is a multiple of 4 bytes and the block comes from
// new[], so each u32 field sits on a 4-byte boundary (the C3 faults on an unaligned load).
struct FontCatalog::Header {
  uint32_t magic;
  uint16_t version;
  uint16_t families;
  uint16_t files;
  uint16_t stringBytes;
  uint32_t blockBytes;
};

struct FontCatalog::FamilyRecord {
  uint32_t totalSize;
  uint16_t name;  // offsets into the strings
  uint16_t description;
  uint16_t firstFile;
  uint8_t flags;
  uint8_t reserved;
};

struct FontCatalog::FileRecord {
  uint32_t size;
  uint32_t crc32;
  uint16_t name;  // offset into the strings
  uint8_t hasCrc32;
  uint8_t reserved;
};

namespace {

constexpr uint32_t MAGIC = 0x43465043;  // "CPFC" in the file
constexpr uint16_t VERSION = 2;         // 2: the outline and Download-all flags
constexpr size_t LIMIT = 0xFFFF;        // the 16-bit counts and offsets

constexpr uint8_t INSTALLED = 1 << 0;
constexpr uint8_t HAS_UPDATE = 1 << 1;
constexpr uint8_t RESUMABLE = 1 << 2;
constexpr uint8_t OUTLINE = 1 << 3;
constexpr uint8_t NOT_IN_DOWNLOAD_ALL = 1 << 4;

constexpr size_t NO_FILE = static_cast<size_t>(-1);

// The string's length up to `len` or an embedded NUL, whichever comes first.
size_t boundedLength(const char* text, const size_t len) {
  if (len == 0) return 0;
  const void* nul = memchr(text, '\0', len);
  return nul ? static_cast<size_t>(static_cast<const char*>(nul) - text) : len;
}

// Adds one family of `from` to `builder`, as the manifest reader would have.
void addFamily(FontCatalog::Builder& builder, const FontCatalog& from, const size_t family) {
  builder.beginFamily();
  const char* description = from.description(family);
  builder.description(description, strlen(description));
  for (size_t file = 0; file < from.fileCount(family); ++file) {
    const char* name = from.fileName(family, file);
    builder.addFile(name, strlen(name), from.fileSize(family, file), from.fileCrc32(family, file),
                    from.fileHasCrc32(family, file));
  }
  const char* name = from.name(family);
  builder.keepFamily(name, strlen(name));
}

}  // namespace

size_t FontCatalog::blockBytesFor(const Size& size) {
  static_assert(sizeof(Header) == 16 && sizeof(FamilyRecord) == 12 && sizeof(FileRecord) == 12,
                "the stash format FontCatalog.h documents");
  static_assert(alignof(Header) <= 4 && alignof(FamilyRecord) <= 4 && alignof(FileRecord) <= 4,
                "records must sit on the block's 4-byte alignment");
  return sizeof(Header) + size.families * sizeof(FamilyRecord) + size.files * sizeof(FileRecord) + size.stringBytes;
}

bool FontCatalog::fits(const Size& size) {
  return size.families <= LIMIT && size.files <= LIMIT && size.stringBytes <= LIMIT;
}

bool FontCatalog::allocate(const Size& size) {
  clear();
  if (!fits(size)) return false;
  if (size.families == 0) return true;

  // The whole list in one block -- 8 KB for the shipped manifest -- so that freeing it hands the heap
  // back in one piece. It outlives the call and its size is known only once the manifest is counted,
  // so neither the stack nor a static buffer can hold it.
  const size_t bytes = blockBytesFor(size);
  auto fresh = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!fresh) return false;
  const Header head{MAGIC,
                    VERSION,
                    static_cast<uint16_t>(size.families),
                    static_cast<uint16_t>(size.files),
                    static_cast<uint16_t>(size.stringBytes),
                    static_cast<uint32_t>(bytes)};
  memcpy(fresh.get(), &head, sizeof(head));
  block = std::move(fresh);
  return true;
}

const FontCatalog::Header* FontCatalog::header() const { return reinterpret_cast<const Header*>(block.get()); }

const FontCatalog::FamilyRecord* FontCatalog::familyRecords() const {
  return reinterpret_cast<const FamilyRecord*>(block.get() + sizeof(Header));
}

FontCatalog::FamilyRecord* FontCatalog::familyRecords() {
  return reinterpret_cast<FamilyRecord*>(block.get() + sizeof(Header));
}

const FontCatalog::FileRecord* FontCatalog::fileRecords() const {
  return reinterpret_cast<const FileRecord*>(familyRecords() + count());
}

FontCatalog::FileRecord* FontCatalog::fileRecords() { return reinterpret_cast<FileRecord*>(familyRecords() + count()); }

const char* FontCatalog::strings() const { return reinterpret_cast<const char*>(fileRecords() + totalFiles()); }

char* FontCatalog::strings() { return reinterpret_cast<char*>(fileRecords() + totalFiles()); }

size_t FontCatalog::count() const { return block ? header()->families : 0; }

size_t FontCatalog::totalFiles() const { return block ? header()->files : 0; }

size_t FontCatalog::blockBytes() const { return block ? header()->blockBytes : 0; }

const char* FontCatalog::name(const size_t family) const {
  return family < count() ? strings() + familyRecords()[family].name : "";
}

const char* FontCatalog::description(const size_t family) const {
  return family < count() ? strings() + familyRecords()[family].description : "";
}

size_t FontCatalog::fileCount(const size_t family) const {
  const size_t families = count();
  if (family >= families) return 0;
  const size_t end = family + 1 < families ? familyRecords()[family + 1].firstFile : totalFiles();
  return end - familyRecords()[family].firstFile;
}

size_t FontCatalog::totalSize(const size_t family) const {
  return family < count() ? familyRecords()[family].totalSize : 0;
}

size_t FontCatalog::fileIndex(const size_t family, const size_t file) const {
  if (file >= fileCount(family)) return NO_FILE;
  return familyRecords()[family].firstFile + file;
}

const char* FontCatalog::fileName(const size_t family, const size_t file) const {
  const size_t index = fileIndex(family, file);
  return index == NO_FILE ? "" : strings() + fileRecords()[index].name;
}

const char* FontCatalog::fileLocalName(const size_t family, const size_t file) const {
  const char* full = fileName(family, file);
  const char* folder = name(family);
  const size_t folderLen = strlen(folder);
  if (strncmp(full, folder, folderLen) == 0 && full[folderLen] == '/') return full + folderLen + 1;
  return full;
}

uint32_t FontCatalog::fileSize(const size_t family, const size_t file) const {
  const size_t index = fileIndex(family, file);
  return index == NO_FILE ? 0 : fileRecords()[index].size;
}

uint32_t FontCatalog::fileCrc32(const size_t family, const size_t file) const {
  const size_t index = fileIndex(family, file);
  return index == NO_FILE ? 0 : fileRecords()[index].crc32;
}

bool FontCatalog::fileHasCrc32(const size_t family, const size_t file) const {
  const size_t index = fileIndex(family, file);
  return index != NO_FILE && fileRecords()[index].hasCrc32 != 0;
}

bool FontCatalog::flag(const size_t family, const uint8_t flag) const {
  return family < count() && (familyRecords()[family].flags & flag) != 0;
}

void FontCatalog::setFlag(const size_t family, const uint8_t flag, const bool value) {
  if (family >= count()) return;
  uint8_t& flags = familyRecords()[family].flags;
  flags = value ? static_cast<uint8_t>(flags | flag) : static_cast<uint8_t>(flags & ~flag);
}

bool FontCatalog::outline(const size_t family) const { return flag(family, OUTLINE); }
bool FontCatalog::inDownloadAll(const size_t family) const {
  return family < count() && !flag(family, NOT_IN_DOWNLOAD_ALL);
}
bool FontCatalog::installed(const size_t family) const { return flag(family, INSTALLED); }
bool FontCatalog::hasUpdate(const size_t family) const { return flag(family, HAS_UPDATE); }
bool FontCatalog::hasResumableDownload(const size_t family) const { return flag(family, RESUMABLE); }
void FontCatalog::setInstalled(const size_t family, const bool value) { setFlag(family, INSTALLED, value); }
void FontCatalog::setHasUpdate(const size_t family, const bool value) { setFlag(family, HAS_UPDATE, value); }
void FontCatalog::setHasResumableDownload(const size_t family, const bool value) { setFlag(family, RESUMABLE, value); }

bool FontCatalog::copyFrom(const FontCatalog& from, const FamilyFilter keep, const void* ctx) {
  clear();
  if (&from == this) return false;

  Builder counter;
  for (size_t family = 0; family < from.count(); ++family) {
    if (!keep || keep(ctx, from, family)) addFamily(counter, from, family);
  }
  if (!allocate(counter.size())) return false;

  Builder filler(*this);
  size_t copied = 0;
  for (size_t family = 0; family < from.count(); ++family) {
    if (keep && !keep(ctx, from, family)) continue;
    addFamily(filler, from, family);
    if (copied < count()) familyRecords()[copied].flags = from.familyRecords()[family].flags;
    ++copied;
  }
  if (filler.overflowed() || !(filler.size() == counter.size())) {
    clear();
    return false;
  }
  return true;
}

bool FontCatalog::copyFamilyFrom(const FontCatalog& from, size_t family) {
  if (family >= from.count()) {
    clear();
    return false;
  }
  return copyFrom(
      from, [](const void* ctx, const FontCatalog&, const size_t i) { return i == *static_cast<const size_t*>(ctx); },
      &family);
}

bool FontCatalog::writeTo(HalFile& file) const {
  if (!block) return false;
  const size_t bytes = blockBytes();
  return file.write(block.get(), bytes) == bytes;
}

bool FontCatalog::readFrom(HalFile& file) {
  clear();
  const size_t bytes = file.fileSize();
  if (bytes < sizeof(Header) || bytes > blockBytesFor({LIMIT, LIMIT, LIMIT})) return false;

  // One block, as allocate() makes it, sized by the stash and read straight into: no second copy.
  auto fresh = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!fresh) return false;
  if (!file.seekSet(0)) return false;
  const int got = file.read(fresh.get(), bytes);
  if (got < 0 || static_cast<size_t>(got) != bytes) return false;
  if (!holdsTogether(fresh.get(), bytes)) return false;
  block = std::move(fresh);
  return true;
}

bool FontCatalog::holdsTogether(const uint8_t* data, const size_t bytes) {
  Header head;
  memcpy(&head, data, sizeof(head));
  if (head.magic != MAGIC || head.version != VERSION || head.families == 0) return false;
  const Size size{head.families, head.files, head.stringBytes};
  if (head.blockBytes != bytes || blockBytesFor(size) != bytes) return false;

  const auto* families = reinterpret_cast<const FamilyRecord*>(data + sizeof(Header));
  const auto* files = reinterpret_cast<const FileRecord*>(families + size.families);
  const char* stringPool = reinterpret_cast<const char*>(files + size.files);

  // Every string ends inside the block: the last byte is a terminator and every offset is before it.
  if (size.stringBytes == 0 || stringPool[size.stringBytes - 1] != '\0') return false;
  for (size_t file = 0; file < size.files; ++file) {
    if (files[file].name >= size.stringBytes) return false;
  }
  // The families share out the files in order, from the first to the last, and each total adds up.
  for (size_t family = 0; family < size.families; ++family) {
    const FamilyRecord& record = families[family];
    if (record.name >= size.stringBytes || record.description >= size.stringBytes) return false;
    const size_t first = record.firstFile;
    const size_t end = family + 1 < size.families ? families[family + 1].firstFile : size.files;
    if ((family == 0 && first != 0) || first > end || end > size.files) return false;
    uint32_t total = 0;
    for (size_t file = first; file < end; ++file) total += files[file].size;
    if (total != record.totalSize) return false;
  }
  return true;
}

// --- Builder ---

bool FontCatalog::Builder::putString(const char* text, const size_t len, uint16_t& offset) {
  const size_t n = boundedLength(text, len);
  if (target) {
    const size_t capacity = target->block ? target->header()->stringBytes : 0;
    if (open.stringBytes + n + 1 > capacity) {
      familyOverflow = true;
      return false;
    }
    char* at = target->strings() + open.stringBytes;
    if (n > 0) memcpy(at, text, n);
    at[n] = '\0';
    offset = static_cast<uint16_t>(open.stringBytes);
  }
  open.stringBytes += n + 1;
  return true;
}

void FontCatalog::Builder::beginFamily() {
  open = kept;
  familyTotal = 0;
  descriptionOffset = 0;
  hasDescription = false;
  familyFlags = 0;
  familyOverflow = false;
}

void FontCatalog::Builder::outline(const bool value) {
  familyFlags = value ? static_cast<uint8_t>(familyFlags | OUTLINE) : static_cast<uint8_t>(familyFlags & ~OUTLINE);
}

void FontCatalog::Builder::inDownloadAll(const bool value) {
  familyFlags = value ? static_cast<uint8_t>(familyFlags & ~NOT_IN_DOWNLOAD_ALL)
                      : static_cast<uint8_t>(familyFlags | NOT_IN_DOWNLOAD_ALL);
}

void FontCatalog::Builder::description(const char* text, const size_t len) {
  uint16_t offset = 0;
  if (!putString(text, len, offset)) return;
  descriptionOffset = offset;
  hasDescription = true;
}

void FontCatalog::Builder::addFile(const char* name, const size_t nameLen, const uint32_t size, const uint32_t crc32,
                                   const bool hasCrc32) {
  uint16_t nameOffset = 0;
  if (!putString(name, nameLen, nameOffset)) return;
  if (target) {
    if (open.files >= target->totalFiles()) {
      familyOverflow = true;
      return;
    }
    FileRecord& record = target->fileRecords()[open.files];
    record.size = size;
    record.crc32 = hasCrc32 ? crc32 : 0;
    record.name = nameOffset;
    record.hasCrc32 = hasCrc32 ? 1 : 0;
    record.reserved = 0;
  }
  ++open.files;
  familyTotal += size;
}

void FontCatalog::Builder::keepFamily(const char* name, const size_t nameLen) {
  if (!hasDescription) description("", 0);
  uint16_t nameOffset = 0;
  putString(name, nameLen, nameOffset);
  if (target && kept.families >= target->count()) familyOverflow = true;
  if (familyOverflow) {
    overflow = true;
    dropFamily();
    return;
  }
  if (target) {
    FamilyRecord& record = target->familyRecords()[kept.families];
    record.totalSize = familyTotal;
    record.name = nameOffset;
    record.description = descriptionOffset;
    record.firstFile = static_cast<uint16_t>(kept.files);
    record.flags = familyFlags;
    record.reserved = 0;
  }
  kept.families += 1;
  kept.files = open.files;
  kept.stringBytes = open.stringBytes;
  open = kept;
}

void FontCatalog::Builder::dropFamily() {
  open = kept;
  familyOverflow = false;
}
