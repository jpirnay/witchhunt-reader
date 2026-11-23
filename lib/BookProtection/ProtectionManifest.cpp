#include "ProtectionManifest.h"

#include <BuildArena.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <HeapFit.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "AesCbcCipher.h"
#include "EncryptionXmlParser.h"

namespace book_protection {
namespace {

constexpr const char* ENCRYPTION_XML = "META-INF/encryption.xml";
constexpr uint32_t MANIFEST_MAGIC = 0x31525057;  // "WPR1"
constexpr uint16_t MANIFEST_VERSION = 1;
constexpr uint32_t MAX_RUNS = 16;
constexpr uint8_t FLAG_DEFLATED = 1;
constexpr uint8_t FLAG_SIZE_KNOWN = 2;
// The measuring reader's read buffer; its ring is up to a full inflate window.
constexpr size_t READ_CHUNK = 1024;
constexpr size_t READER_BUDGET = READ_CHUNK + 32768 + 2 * alignof(std::max_align_t);
// Fewer than this per run is not worth the passes.
constexpr size_t MIN_RUN_ENTRIES = 16;

// On the card, little-endian as both chips and the host are. The header, then `runs` run lengths
// (uint32), then the records, each run's sorted by local-header offset.
struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t cipherBits;  // 0: a plain book
  uint64_t fingerprint;
  uint32_t records;
  uint32_t runs;
};
static_assert(sizeof(Header) == 24, "protect.bin header layout");

struct Record {
  uint32_t localHeaderOffset;
  uint32_t plainSize;
  uint8_t flags;
  uint8_t reserved[3];
};
static_assert(sizeof(Record) == 12, "protect.bin record layout");

// An entry while a run is built.
struct Pending {
  uint64_t hash;  // ZipFile::StatTarget's, of the entry's name
  uint16_t len;
  uint8_t flags;
  uint8_t raster;
  uint32_t plainSize;
  uint32_t localHeaderOffset;  // 0 until the walk finds the entry (0 is the mimetype's)
  uint32_t compressedSize;
};
static_assert(sizeof(Pending) == 24, "a run's table entry");

bool isRaster(const std::string_view name) {
  for (const char* ext : {".jpg", ".jpeg", ".png", ".gif", ".webp"}) {
    if (FsHelpers::checkFileExtension(name, ext)) return true;
  }
  return false;
}

size_t arenaRoom(const BuildArena* arena) {
  return arena != nullptr && arena->valid() ? arena->capacity() - arena->used() : 0;
}

// Streams one entry through `out`, its reader in `scratch` when the whole read fits there.
bool streamEntry(ZipFile& zip, const ZipFile::FileStatSlim& stat, Print& out, BuildArena* scratch) {
  const size_t need = READ_CHUNK + InflateReader::ringSizeFor(stat.uncompressedSize) + 2 * alignof(std::max_align_t);
  ZipFile::EntryReader reader(zip, READ_CHUNK, arenaRoom(scratch) >= need ? scratch : nullptr);
  if (!reader.open(stat)) return false;
  uint8_t buf[256];
  bool done = false;
  while (!done) {
    size_t produced = 0;
    if (!reader.step(buf, sizeof(buf), &produced, &done)) return false;
    if (produced > 0 && out.write(buf, produced) != produced) return false;
  }
  return true;
}

// Streams encryption.xml, whose stat is `stat`, through `visit`.
bool parseEncryptionXml(ZipFile& zip, const ZipFile::FileStatSlim& stat, EncryptionXmlParser::Visit visit, void* ctx,
                        BuildArena* scratch) {
  EncryptionXmlParser parser(visit, ctx);
  if (!parser.setup()) return false;
  const bool streamed = streamEntry(zip, stat, parser, scratch);
  return parser.finish() && streamed;
}

struct Census {
  uint32_t entries = 0;
  uint16_t cipherBits = 0;
  bool mixed = false;
};

bool countItem(void* ctx, const EncryptionXmlParser::Item& item) {
  auto* census = static_cast<Census*>(ctx);
  if (census->cipherBits != 0 && census->cipherBits != item.cipherBits) census->mixed = true;
  census->cipherBits = item.cipherBits;
  ++census->entries;
  return true;
}

// Takes the manifest's items [first, end) into `table`, in document order.
struct RunCollector {
  Pending* table;
  uint32_t first;
  uint32_t end;
  uint32_t seen = 0;
  uint32_t filled = 0;
};

bool collectItem(void* ctx, const EncryptionXmlParser::Item& item) {
  auto* run = static_cast<RunCollector*>(ctx);
  const uint32_t index = run->seen++;
  if (index < run->first || index >= run->end) return true;
  const std::string_view name(item.uri, item.uriLen);
  const auto target = ZipFile::StatTarget::named(name);
  Pending& p = run->table[run->filled++];
  p.hash = target.hash;
  p.len = target.len;
  p.flags = static_cast<uint8_t>((item.deflated ? FLAG_DEFLATED : 0) | (item.sizeKnown ? FLAG_SIZE_KNOWN : 0));
  p.raster = isRaster(name) ? 1 : 0;
  p.plainSize = item.plainSize;
  p.localHeaderOffset = 0;
  p.compressedSize = 0;
  return true;
}

bool byName(const Pending& a, const Pending& b) { return a.hash < b.hash || (a.hash == b.hash && a.len < b.len); }
bool byOffset(const Pending& a, const Pending& b) { return a.localHeaderOffset < b.localHeaderOffset; }

// The central-directory walk's visitor: finds each entry of the run, sorted byName, by name.
struct RunWalk {
  Pending* begin;
  Pending* end;
};

void noteEntry(void* ctx, const std::string_view name, const ZipFile::FileStatSlim& stat) {
  auto* walk = static_cast<RunWalk*>(ctx);
  Pending key{};
  const auto target = ZipFile::StatTarget::named(name);
  key.hash = target.hash;
  key.len = target.len;
  for (Pending* it = std::lower_bound(walk->begin, walk->end, key, byName);
       it != walk->end && it->hash == key.hash && it->len == key.len; ++it) {
    // A zip entry stored in another way (deflated by the archive) cannot be decrypted: ZipFile
    // refuses it when read (ENCRYPTED_UNREADABLE), so it is kept, never measured.
    it->localHeaderOffset = stat.localHeaderOffset;
    it->compressedSize = stat.method == 0 ? stat.compressedSize : 0;
  }
}

// A protection holding only the key: lets the build decrypt the stats it makes itself.
class KeyOnlyProtection final : public ZipFile::Protection {
 public:
  KeyOnlyProtection(const uint8_t* key, const size_t keyLen) : keyLen_(keyLen) { memcpy(key_, key, keyLen); }
  ~KeyOnlyProtection() override { memset(key_, 0, sizeof(key_)); }
  bool find(uint32_t, ZipFile::EncryptedEntry*) override { return false; }
  size_t key(uint8_t out[32]) const override {
    memcpy(out, key_, keyLen_);
    return keyLen_;
  }
  const ZipFile::CipherOps& cipher() const override { return aesCbcCipher(); }

 private:
  uint8_t key_[32] = {};
  size_t keyLen_;
};

// Unregisters the build's protection on every way out.
struct BuildRegistration {
  const std::string& path;
  ~BuildRegistration() { ZipFile::unprotect(path); }
};

// Counts what a read hands it.
class CountingPrint final : public Print {
 public:
  size_t write(uint8_t) override {
    ++count;
    return 1;
  }
  size_t write(const uint8_t*, const size_t n) override {
    count += n;
    return n;
  }
  size_t count = 0;
};

// The plaintext size of an encrypted entry, by reading it: decrypted, and inflated when deflated.
bool measure(ZipFile& zip, const Pending& p, BuildArena* scratch, uint32_t* size) {
  const ZipFile::FileStatSlim raw{0, p.compressedSize, p.compressedSize, p.localHeaderOffset};
  ZipFile::EncryptedEntry entry;
  entry.deflated = (p.flags & FLAG_DEFLATED) != 0;
  CountingPrint counter;
  if (!streamEntry(zip, ZipFile::encryptedStat(raw, entry), counter, scratch)) return false;
  *size = static_cast<uint32_t>(counter.count);
  return true;
}

bool writeAll(HalFile& file, const void* data, const size_t size) {
  return file.write(static_cast<const uint8_t*>(data), size) == size;
}

// The table a run is built in: from the lent region when it has room besides the measuring reader,
// else the heap. Capacity 0 when neither holds MIN_RUN_ENTRIES.
class RunTable {
 public:
  RunTable(BuildArena* scratch, const uint32_t wanted) {
    const size_t room = arenaRoom(scratch);
    if (room > READER_BUDGET + MIN_RUN_ENTRIES * sizeof(Pending)) {
      const size_t entries = std::min<size_t>(wanted, (room - READER_BUDGET) / sizeof(Pending));
      block_ = scratch->reserveBlock();
      table_ = static_cast<Pending*>(scratch->alloc(entries * sizeof(Pending)));
      if (table_ != nullptr) {
        arena_ = scratch;
        capacity_ = entries;
        return;
      }
      scratch->release(block_);
    }
    size_t entries = wanted;
    while (entries >= MIN_RUN_ENTRIES && !heapHasBlockFor(entries * sizeof(Pending))) entries /= 2;
    if (entries < MIN_RUN_ENTRIES && entries < wanted) return;
    heap_ = makeUniqueNoThrow<Pending[]>(entries);
    table_ = heap_.get();
    capacity_ = table_ != nullptr ? entries : 0;
  }
  ~RunTable() {
    if (arena_ != nullptr) arena_->release(block_);
  }
  RunTable(const RunTable&) = delete;
  RunTable& operator=(const RunTable&) = delete;
  Pending* data() const { return table_; }
  size_t capacity() const { return capacity_; }

 private:
  BuildArena* arena_ = nullptr;
  BuildArena::Block block_;
  std::unique_ptr<Pending[]> heap_;
  Pending* table_ = nullptr;
  size_t capacity_ = 0;
};

// Builds one run: [first, end) of the manifest's items, found in the archive, measured, sorted by
// offset. Returns its length; -1 on failure (logged).
int buildRun(ZipFile& zip, const ZipFile::FileStatSlim& xmlStat, Pending* table, const uint32_t first,
             const uint32_t end, const uint64_t fingerprint, BuildArena* scratch) {
  RunCollector collector{table, first, end};
  if (!parseEncryptionXml(zip, xmlStat, collectItem, &collector, scratch)) return -1;
  Pending* const tableEnd = table + collector.filled;
  std::sort(table, tableEnd, byName);

  RunWalk walk{table, tableEnd};
  uint64_t walked = 0;
  if (!zip.contentFingerprint(&walked, noteEntry, &walk)) {
    LOG_ERR("PRT", "Central directory walk failed");
    return -1;
  }
  if (walked != fingerprint) {
    LOG_ERR("PRT", "Archive changed during the build");
    return -1;
  }

  // Entries the archive does not hold, and a name listed twice, go.
  std::sort(table, tableEnd, byOffset);
  Pending* kept = table;
  for (Pending* p = table; p != tableEnd; ++p) {
    if (p->localHeaderOffset == 0) {
      LOG_DBG("PRT", "encryption.xml lists an entry the archive lacks");
      continue;
    }
    if (kept != table && kept[-1].localHeaderOffset == p->localHeaderOffset) continue;
    *kept++ = *p;
  }

  for (Pending* p = table; p != kept; ++p) {
    if ((p->flags & FLAG_SIZE_KNOWN) != 0 || p->raster != 0 || p->compressedSize == 0) continue;
    uint32_t size = 0;
    if (!measure(zip, *p, scratch, &size)) {
      LOG_ERR("PRT", "Could not decrypt the entry at %lu: a wrong key?",
              static_cast<unsigned long>(p->localHeaderOffset));
      return -1;
    }
    p->plainSize = size;
    p->flags |= FLAG_SIZE_KNOWN;
  }
  return static_cast<int>(kept - table);
}

ManifestStatus writePlain(const std::string& manifestPath, const uint64_t fingerprint) {
  const std::string tmpPath = manifestPath + ".tmp";
  HalFile out;
  if (!Storage.openFileForWrite("PRT", tmpPath, out)) return ManifestStatus::Failed;
  const Header header{MANIFEST_MAGIC, MANIFEST_VERSION, 0, fingerprint, 0, 0};
  const bool ok = writeAll(out, &header, sizeof(header));
  out.close();
  if (ok && (!Storage.exists(manifestPath.c_str()) || Storage.remove(manifestPath.c_str())) &&
      Storage.rename(tmpPath.c_str(), manifestPath.c_str())) {
    return ManifestStatus::Plain;
  }
  Storage.remove(tmpPath.c_str());
  return ManifestStatus::Failed;
}

// ---- reading a manifest ----

class ManifestProtection final : public ZipFile::Protection {
 public:
  ~ManifestProtection() override { memset(key_, 0, sizeof(key_)); }

  ManifestStatus open(const std::string& path, const uint64_t fingerprint, const uint8_t* key, const size_t keyLen) {
    if (!Storage.exists(path.c_str())) return ManifestStatus::Missing;
    if (!Storage.openFileForRead("PRT", path, file_)) {
      // Gone since: an enciphered cache folder removes a file of it that is not its own (HalStorage), damaged as a
      // manifest of other content is. Built again either way.
      return Storage.exists(path.c_str()) ? ManifestStatus::Failed : ManifestStatus::Missing;
    }
    Header header{};
    if (file_.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || header.magic != MANIFEST_MAGIC ||
        header.version != MANIFEST_VERSION || header.fingerprint != fingerprint || header.runs > MAX_RUNS) {
      return ManifestStatus::Missing;
    }
    if (header.cipherBits == 0) return ManifestStatus::Plain;
    if (keyLen * 8 != header.cipherBits) {
      LOG_ERR("PRT", "A %u-bit key for a book encrypted with %u bits", static_cast<unsigned>(keyLen * 8),
              static_cast<unsigned>(header.cipherBits));
      return ManifestStatus::Failed;
    }
    uint32_t lengths[MAX_RUNS];
    const size_t lengthBytes = header.runs * sizeof(uint32_t);
    if (file_.read(lengths, lengthBytes) != static_cast<int>(lengthBytes)) return ManifestStatus::Missing;
    uint32_t start = 0;
    for (uint32_t r = 0; r < header.runs; ++r) {
      runStart_[r] = start;
      runLength_[r] = lengths[r];
      start += lengths[r];
    }
    if (start != header.records || file_.size() != sizeof(Header) + lengthBytes + header.records * sizeof(Record)) {
      return ManifestStatus::Missing;
    }
    runs_ = header.runs;
    recordsAt_ = sizeof(Header) + lengthBytes;
    memcpy(key_, key, keyLen);
    keyLen_ = keyLen;
    return ManifestStatus::Protected;
  }

  bool find(const uint32_t localHeaderOffset, ZipFile::EncryptedEntry* out) override {
    for (uint32_t r = 0; r < runs_; ++r) {
      uint32_t lo = runStart_[r];
      uint32_t hi = lo + runLength_[r];
      while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        Record record{};
        if (!readRecord(mid, &record)) return false;
        if (record.localHeaderOffset < localHeaderOffset) {
          lo = mid + 1;
        } else if (record.localHeaderOffset > localHeaderOffset) {
          hi = mid;
        } else {
          out->plainSize = record.plainSize;
          out->sizeKnown = (record.flags & FLAG_SIZE_KNOWN) != 0;
          out->deflated = (record.flags & FLAG_DEFLATED) != 0;
          return true;
        }
      }
    }
    return false;
  }

  size_t key(uint8_t out[32]) const override {
    memcpy(out, key_, keyLen_);
    return keyLen_;
  }
  const ZipFile::CipherOps& cipher() const override { return aesCbcCipher(); }

 private:
  static constexpr uint32_t PAGE_RECORDS = 42;  // 504 bytes: a card sector's worth

  // Record `index`, through a page of them: lookups come in offset order (a book's spine, its
  // images), so most land in the page the last one read.
  bool readRecord(const uint32_t index, Record* out) {
    const uint32_t page = index / PAGE_RECORDS;
    if (page != cachedPage_) {
      const uint32_t first = page * PAGE_RECORDS;
      if (!file_.seek(recordsAt_ + first * sizeof(Record))) return false;
      const int got = file_.read(page_, sizeof(page_));
      if (got < static_cast<int>(sizeof(Record))) {
        cachedPage_ = UINT32_MAX;
        return false;
      }
      cachedPage_ = page;
      cachedCount_ = static_cast<uint32_t>(got) / sizeof(Record);
    }
    const uint32_t at = index % PAGE_RECORDS;
    if (at >= cachedCount_) return false;
    memcpy(out, page_ + at * sizeof(Record), sizeof(Record));
    return true;
  }

  HalFile file_;
  uint32_t recordsAt_ = 0;
  uint32_t runs_ = 0;
  uint32_t runStart_[MAX_RUNS] = {};
  uint32_t runLength_[MAX_RUNS] = {};
  uint8_t page_[PAGE_RECORDS * sizeof(Record)] = {};
  uint32_t cachedPage_ = UINT32_MAX;
  uint32_t cachedCount_ = 0;
  uint8_t key_[32] = {};
  size_t keyLen_ = 0;
};

}  // namespace

ManifestProbe probeManifest(const std::string& epubPath) {
  ManifestProbe probe;
  ZipFile zip(epubPath);
  ZipFile::FileStatSlim xmlStat{};
  if (!zip.loadFileStatSlim(ENCRYPTION_XML, &xmlStat)) {
    probe.status = ManifestStatus::Plain;
    return probe;
  }
  Census census;
  if (!parseEncryptionXml(zip, xmlStat, countItem, &census, nullptr)) return probe;
  if (census.mixed) {
    LOG_ERR("PRT", "encryption.xml mixes AES-128 and AES-256");
    return probe;
  }
  probe.status = census.entries == 0 ? ManifestStatus::Plain : ManifestStatus::Protected;
  probe.entries = census.entries;
  probe.cipherBits = census.cipherBits;
  return probe;
}

ManifestStatus buildManifest(const std::string& epubPath, const uint64_t fingerprint, const uint8_t* key,
                             const size_t keyLen, const std::string& manifestPath, BuildArena* scratch) {
  ZipFile zip(epubPath);
  ZipFile::FileStatSlim xmlStat{};
  if (!zip.loadFileStatSlim(ENCRYPTION_XML, &xmlStat)) return writePlain(manifestPath, fingerprint);
  Census census;
  if (!parseEncryptionXml(zip, xmlStat, countItem, &census, scratch)) return ManifestStatus::Failed;
  if (census.entries == 0) return writePlain(manifestPath, fingerprint);
  if (census.mixed) {
    LOG_ERR("PRT", "encryption.xml mixes AES-128 and AES-256");
    return ManifestStatus::Failed;
  }
  if (key == nullptr || keyLen * 8 != census.cipherBits) {
    LOG_ERR("PRT", "A %u-bit key for a book encrypted with %u bits", static_cast<unsigned>(keyLen * 8),
            static_cast<unsigned>(census.cipherBits));
    return ManifestStatus::Failed;
  }

  if (!ZipFile::protect(epubPath, makeUniqueNoThrow<KeyOnlyProtection>(key, keyLen))) {
    LOG_ERR("PRT", "Out of memory for the build's key");
    return ManifestStatus::Failed;
  }
  const BuildRegistration registration{epubPath};

  const RunTable table(scratch, census.entries);
  if (table.capacity() == 0) {
    LOG_ERR("PRT", "No room for a table of %lu entries", static_cast<unsigned long>(census.entries));
    return ManifestStatus::Failed;
  }
  const auto runs = static_cast<uint32_t>((census.entries + table.capacity() - 1) / table.capacity());
  if (runs > MAX_RUNS) {
    LOG_ERR("PRT", "%lu entries would take %lu runs", static_cast<unsigned long>(census.entries),
            static_cast<unsigned long>(runs));
    return ManifestStatus::Failed;
  }

  const std::string tmpPath = manifestPath + ".tmp";
  HalFile out;
  if (!Storage.openFileForWrite("PRT", tmpPath, out)) return ManifestStatus::Failed;
  Header header{MANIFEST_MAGIC, MANIFEST_VERSION, census.cipherBits, fingerprint, 0, runs};
  uint32_t lengths[MAX_RUNS] = {};
  bool ok = writeAll(out, &header, sizeof(header)) && writeAll(out, lengths, runs * sizeof(uint32_t));
  for (uint32_t r = 0; ok && r < runs; ++r) {
    const auto first = static_cast<uint32_t>(r * table.capacity());
    const auto end = static_cast<uint32_t>(std::min<size_t>(census.entries, first + table.capacity()));
    const int length = buildRun(zip, xmlStat, table.data(), first, end, fingerprint, scratch);
    ok = length >= 0;
    for (int i = 0; ok && i < length; ++i) {
      const Pending& p = table.data()[i];
      const Record record{p.localHeaderOffset, p.plainSize, p.flags, {}};
      ok = writeAll(out, &record, sizeof(record));
    }
    if (ok) {
      lengths[r] = static_cast<uint32_t>(length);
      header.records += lengths[r];
    }
  }
  // The header and run lengths again, now that they are known.
  ok = ok && out.seek(0) && writeAll(out, &header, sizeof(header)) && writeAll(out, lengths, runs * sizeof(uint32_t));
  if (!out.close()) ok = false;
  if (ok && (!Storage.exists(manifestPath.c_str()) || Storage.remove(manifestPath.c_str())) &&
      Storage.rename(tmpPath.c_str(), manifestPath.c_str())) {
    LOG_INF("PRT", "Protected book: %lu encrypted entries, AES-%u, %lu run(s)",
            static_cast<unsigned long>(header.records), static_cast<unsigned>(census.cipherBits),
            static_cast<unsigned long>(runs));
    return ManifestStatus::Protected;
  }
  Storage.remove(tmpPath.c_str());
  return ManifestStatus::Failed;
}

ManifestStatus openManifest(const std::string& manifestPath, const uint64_t fingerprint, const uint8_t* key,
                            const size_t keyLen, std::unique_ptr<ZipFile::Protection>* out) {
  auto protection = makeUniqueNoThrow<ManifestProtection>();
  if (!protection) return ManifestStatus::Failed;
  const ManifestStatus status = protection->open(manifestPath, fingerprint, key, keyLen);
  if (status == ManifestStatus::Protected) *out = std::move(protection);
  return status;
}

}  // namespace book_protection
