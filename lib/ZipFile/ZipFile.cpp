#include "ZipFile.h"

#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

struct ZipInflateCtx {
  InflateReader reader;  // Must be first — callback casts uzlib_uncomp* to ZipInflateCtx*
  FsFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

namespace {
constexpr uint16_t ZIP_METHOD_STORED = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED = 8;

int zipReadCallback(uzlib_uncomp* uncomp) {
  auto* ctx = reinterpret_cast<ZipInflateCtx*>(uncomp);
  if (ctx->fileRemaining == 0) return -1;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const size_t bytesRead = ctx->file->read(ctx->readBuf, toRead);
  ctx->fileRemaining -= bytesRead;

  if (bytesRead == 0) return -1;

  uncomp->source = ctx->readBuf + 1;
  uncomp->source_limit = ctx->readBuf + bytesRead;
  return ctx->readBuf[0];
}
}  // namespace

static std::string normalizeZipPath(const char* filename) {
  std::string normalized;
  normalized.reserve(strlen(filename));
  for (const char* p = filename; *p; ++p) {
    if (*p == '\\') {
      normalized.push_back('/');
    } else {
      normalized.push_back(*p);
    }
  }
  while (!normalized.empty() && normalized.front() == '/') {
    normalized.erase(normalized.begin());
  }
  return normalized;
}

static std::string normalizeZipPathLower(const char* filename) {
  std::string normalized = normalizeZipPath(filename);
  for (char& ch : normalized) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return normalized;
}

bool ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  const std::string normalizedFilename = normalizeZipPath(filename);
  const std::string normalizedFilenameLower = normalizeZipPathLower(filename);

  const ScopedOpenClose zip{*this};
  if (!zip) {
    LOG_ERR("ZIP", "loadFileStatSlim: failed to open zip for %s", filename);
    return false;
  }

  if (!loadZipDetails()) {
    LOG_ERR("ZIP", "loadFileStatSlim: loadZipDetails failed for %s", filename);
    return false;
  }

  // Phase 1: Try scanning from cursor position first
  uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  bool wrapped = false;
  bool found = false;

  file.seek(startPos);

  uint32_t sig;
  char itemName[256];

  while (true) {
    uint32_t entryStart = file.position();

    if (file.read(&sig, 4) != 4 || sig != 0x02014b50) {
      // End of central directory
      if (!wrapped && lastCentralDirPosValid && startPos != zipDetails.centralDirOffset) {
        // Wrap around to beginning
        file.seek(zipDetails.centralDirOffset);
        wrapped = true;
        continue;
      }
      break;
    }

    // If we've wrapped and reached our start position, stop
    if (wrapped && entryStart >= startPos) {
      break;
    }

    file.seekCur(6);
    file.read(&fileStat->method, 2);
    file.seekCur(8);
    file.read(&fileStat->compressedSize, 4);
    file.read(&fileStat->uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat->localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      // Normalize once, then build lowercase via in-place transform — one alloc, not two.
      std::string normalizedItemName = normalizeZipPath(itemName);
      if (normalizedItemName == normalizedFilename) {
        file.seekCur(m + k);
        lastCentralDirPos = file.position();
        lastCentralDirPosValid = true;
        found = true;
        break;
      }

      std::string normalizedItemNameLower = normalizedItemName;
      for (char& ch : normalizedItemNameLower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (normalizedItemNameLower == normalizedFilenameLower) {
        LOG_DBG("ZIP", "loadFileStatSlim: case-insensitive match for %s => %s", filename, itemName);
        file.seekCur(m + k);
        lastCentralDirPos = file.position();
        lastCentralDirPosValid = true;
        found = true;
        break;
      }
    } else {
      // Name too long, skip it
      file.seekCur(nameLen);
    }

    // Skip extra field + comment
    file.seekCur(m + k);
  }

  if (!found) {
    LOG_DBG("ZIP", "loadFileStatSlim: entry not found after scan: %s (normalized=%s)", filename,
            normalizedFilename.c_str());
  }

  return found;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  file.seek(fileOffset);
  const size_t read = file.read(pLocalHeader, localHeaderSize);

  if (read != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header");
    return -1;
  }

  if (pLocalHeader[0] + (pLocalHeader[1] << 8) + (pLocalHeader[2] << 16) + (pLocalHeader[3] << 24) !=
      0x04034b50 /* ZIP local file header signature */) {
    LOG_ERR("ZIP", "Not a valid zip file header");
    return -1;
  }

  const uint16_t filenameLength = pLocalHeader[26] + (pLocalHeader[27] << 8);
  const uint16_t extraOffset = pLocalHeader[28] + (pLocalHeader[29] << 8);
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
}

bool ZipFile::loadZipDetails() {
  if (zipDetails.isSet) {
    return true;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  const size_t fileSize = file.size();
  if (fileSize < 22) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    return false;  // Minimum EOCD size is 22 bytes
  }

  // We scan the last 1KB (or the whole file if smaller) for the EOCD signature
  // 0x06054b50 is stored as 0x50, 0x4b, 0x05, 0x06 in little-endian
  const int scanRange = fileSize > 1024 ? 1024 : fileSize;
  const auto buffer = static_cast<uint8_t*>(malloc(scanRange));
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for EOCD scan buffer");
    return false;
  }

  file.seek(fileSize - scanRange);
  file.read(buffer, scanRange);

  // Scan backwards for the signature
  int foundOffset = -1;
  for (int i = scanRange - 22; i >= 0; i--) {
    constexpr uint32_t signature = 0x06054b50;
    if (*reinterpret_cast<uint32_t*>(&buffer[i]) == signature) {
      foundOffset = i;
      break;
    }
  }

  if (foundOffset == -1) {
    LOG_ERR("ZIP", "EOCD signature not found in zip file");
    free(buffer);
    return false;
  }

  // Now extract the values we need from the EOCD record
  // Relative positions within EOCD:
  // Offset 10: Total number of entries (2 bytes)
  // Offset 16: Offset of start of central directory with respect to the starting disk number (4 bytes)
  zipDetails.totalEntries = *reinterpret_cast<uint16_t*>(&buffer[foundOffset + 10]);
  zipDetails.centralDirOffset = *reinterpret_cast<uint32_t*>(&buffer[foundOffset + 16]);
  zipDetails.isSet = true;

  free(buffer);
  return true;
}

bool ZipFile::open() {
  if (!Storage.openFileForRead("ZIP", filePath, file)) {
    return false;
  }
  return true;
}

bool ZipFile::close() {
  if (file) {
    file.close();
  }
  lastCentralDirPos = 0;
  lastCentralDirPosValid = false;
  return true;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) {
  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.uncompressedSize);
  return true;
}

int ZipFile::fillUncompressedSizes(const std::vector<SizeTarget>& targets, std::vector<uint32_t>& sizes) {
  if (targets.empty()) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!loadZipDetails()) return 0;

  file.seek(zipDetails.centralDirOffset);

  int matched = 0;
  const int targetCount = static_cast<int>(targets.size());
  uint32_t sig;
  char itemName[256];

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;

    file.seekCur(6);
    uint16_t method;
    file.read(&method, 2);
    file.seekCur(8);
    uint32_t compressedSize, uncompressedSize;
    file.read(&compressedSize, 4);
    file.read(&uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    uint32_t localHeaderOffset;
    file.read(&localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      uint64_t hash = fnvHash64(itemName, nameLen);
      SizeTarget key = {hash, nameLen, 0};

      auto it = std::lower_bound(targets.begin(), targets.end(), key, [](const SizeTarget& a, const SizeTarget& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
      });

      while (it != targets.end() && it->hash == hash && it->len == nameLen) {
        if (it->index < sizes.size()) {
          sizes[it->index] = uncompressedSize;
          matched++;
        }
        ++it;
      }

      if (matched >= targetCount) {
        break;
      }
    } else {
      file.seekCur(nameLen);
    }

    file.seekCur(m + k);
  }

  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return nullptr;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return nullptr;

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  const auto dataSize = trailingNullByte ? inflatedDataSize + 1 : inflatedDataSize;
  const auto data = static_cast<uint8_t*>(malloc(dataSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", dataSize);
    return nullptr;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const size_t dataRead = file.read(data, inflatedDataSize);

    if (dataRead != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.method == ZIP_METHOD_DEFLATED) {
    // Read out deflated content from file
    const auto deflatedData = static_cast<uint8_t*>(malloc(deflatedDataSize));
    if (deflatedData == nullptr) {
      LOG_ERR("ZIP", "Failed to allocate memory for decompression buffer");
      free(data);
      return nullptr;
    }

    const size_t dataRead = file.read(deflatedData, deflatedDataSize);

    if (dataRead != deflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data, expected %d got %d", deflatedDataSize, dataRead);
      free(deflatedData);
      free(data);
      return nullptr;
    }

    bool success = false;
    {
      InflateReader r;
      r.init(false);
      r.setSource(deflatedData, deflatedDataSize);
      success = r.read(data, inflatedDataSize);
    }
    free(deflatedData);

    if (!success) {
      LOG_ERR("ZIP", "Failed to inflate file");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    free(data);
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize) {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return false;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return false;

  file.seek(fileOffset);
  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const auto buffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!buffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for buffer");
      return false;
    }

    size_t remaining = inflatedDataSize;
    while (remaining > 0) {
      const size_t dataRead = file.read(buffer, remaining < chunkSize ? remaining : chunkSize);
      if (dataRead == 0) {
        LOG_ERR("ZIP", "Could not read more bytes");
        free(buffer);
        return false;
      }

      if (out.write(buffer, dataRead) != dataRead) {
        LOG_ERR("ZIP", "Failed to write all output bytes to stream");
        free(buffer);
        return false;
      }
      remaining -= dataRead;
    }

    free(buffer);
    return true;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
      return false;
    }

    auto* outputBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!outputBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for output buffer");
      free(fileReadBuffer);
      return false;
    }

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;
    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = chunkSize;

    // Ring sized to the entry (≤32 KB) — frees up to ~29 KB during small-entry streams.
    if (!ctx.reader.init(true, static_cast<size_t>(inflatedDataSize))) {
      LOG_ERR("ZIP", "Failed to init inflate reader");
      free(outputBuffer);
      free(fileReadBuffer);
      return false;
    }
    ctx.reader.setReadCallback(zipReadCallback);

    bool success = false;
    size_t totalProduced = 0;

    while (true) {
      size_t produced;
      const InflateStatus status = ctx.reader.readAtMost(outputBuffer, chunkSize, &produced);

      totalProduced += produced;
      if (totalProduced > static_cast<size_t>(inflatedDataSize)) {
        LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %zu)", totalProduced,
                static_cast<size_t>(inflatedDataSize));
        break;
      }

      if (produced > 0) {
        if (out.write(outputBuffer, produced) != produced) {
          LOG_ERR("ZIP", "Failed to write all output bytes to stream");
          break;
        }
      }

      if (status == InflateStatus::Done) {
        if (totalProduced != static_cast<size_t>(inflatedDataSize)) {
          LOG_ERR("ZIP", "Decompressed size mismatch (expected %zu, got %zu)", static_cast<size_t>(inflatedDataSize),
                  totalProduced);
          break;
        }
        LOG_DBG("ZIP", "Decompressed %d bytes into %d bytes", deflatedDataSize, inflatedDataSize);
        success = true;
        break;
      }

      if (status == InflateStatus::Error) {
        LOG_ERR("ZIP", "Decompression failed");
        break;
      }
      // InflateStatus::Ok: output buffer full, continue
    }

    free(outputBuffer);
    free(fileReadBuffer);
    return success;  // ctx.reader destructor frees the ring buffer
  }

  LOG_ERR("ZIP", "Unsupported compression method");
  return false;
}

size_t ZipFile::readBytesFromEntry(const char* filename, uint8_t* outBuf, const size_t maxBytes) {
  if (!outBuf || maxBytes == 0) return 0;

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return 0;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return 0;

  file.seek(fileOffset);
  const size_t wantBytes = std::min(maxBytes, static_cast<size_t>(fileStat.uncompressedSize));

  if (fileStat.method == ZIP_METHOD_STORED) {
    const int n = file.read(outBuf, wantBytes);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    // Streaming inflate (32KB ring buffer) is the preferred path.  It can fail
    // if a 32KB ring buffer can't be allocated while another inflate is live
    // (e.g. reading image headers mid-XHTML stream).  In that case fall back to
    // one-shot inflate: read a bounded compressed chunk and decompress it all at
    // once without a ring buffer.
    constexpr size_t READ_BUF = 512;
    auto* readBuf = static_cast<uint8_t*>(malloc(READ_BUF));
    if (!readBuf) return 0;

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = fileStat.compressedSize;
    ctx.readBuf = readBuf;
    ctx.readBufSize = READ_BUF;

    if (ctx.reader.init(true)) {
      ctx.reader.setReadCallback(zipReadCallback);

      size_t totalOut = 0;
      while (totalOut < wantBytes) {
        size_t produced;
        const size_t remaining = wantBytes - totalOut;
        const InflateStatus status = ctx.reader.readAtMost(outBuf + totalOut, remaining, &produced);
        totalOut += produced;
        if (status == InflateStatus::Done || status == InflateStatus::Error) break;
      }

      free(readBuf);
      return totalOut;
    }

    // Streaming ring buffer unavailable — fall back to one-shot inflate.
    // Read a bounded compressed chunk (4× the desired output as a rough overhead
    // estimate, capped at the actual compressed size) and decompress in one shot.
    free(readBuf);
    const size_t compChunkSize = std::min(static_cast<size_t>(fileStat.compressedSize), wantBytes * 4);
    auto* compBuf = static_cast<uint8_t*>(malloc(compChunkSize));
    if (!compBuf) return 0;

    const size_t compRead = file.read(compBuf, compChunkSize);
    if (compRead == 0) {
      free(compBuf);
      return 0;
    }

    InflateReader oneShot;
    oneShot.init(false);
    oneShot.setSource(compBuf, compRead);

    // One-shot read: decompress up to wantBytes; partial output is still useful
    // (e.g. a JPEG SOF marker within the first kHeaderBufSize bytes).
    size_t produced = 0;
    const InflateStatus status = oneShot.readAtMost(outBuf, wantBytes, &produced);
    free(compBuf);
    if (status == InflateStatus::Error && produced == 0) return 0;
    return produced;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// ZipFile::EntryReader — resumable per-entry inflate
// ---------------------------------------------------------------------------

struct ZipFile::EntryReader::Impl {
  ZipFile& zf;
  size_t chunkSize;

  uint16_t method = 0;  // ZIP_METHOD_STORED or ZIP_METHOD_DEFLATED
  FsFile file;
  size_t inflatedSize_ = 0;
  size_t bytesProduced_ = 0;
  bool error_ = false;
  bool done_ = false;

  // Stored-entry tracking
  size_t storedRemaining = 0;

  // Deflated-entry state. ZipInflateCtx.reader must remain first (callback cast).
  ZipInflateCtx ctx = {};
  uint8_t* readBuf = nullptr;

  explicit Impl(ZipFile& zf_, size_t chunkSize_) : zf(zf_), chunkSize(chunkSize_) {}
  ~Impl() { reset(); }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  void reset() {
    if (readBuf) {
      ctx.reader.deinit();
      free(readBuf);
      readBuf = nullptr;
    }
    ctx.file = nullptr;
    ctx.fileRemaining = 0;
    ctx.readBuf = nullptr;
    ctx.readBufSize = 0;
    if (file) file.close();
    inflatedSize_ = 0;
    bytesProduced_ = 0;
    storedRemaining = 0;
    method = 0;
    error_ = false;
    done_ = false;
  }
};

ZipFile::EntryReader::EntryReader(ZipFile& zf, const size_t chunkSize) : impl_(std::make_unique<Impl>(zf, chunkSize)) {}

ZipFile::EntryReader::~EntryReader() = default;
ZipFile::EntryReader::EntryReader(EntryReader&&) noexcept = default;
ZipFile::EntryReader& ZipFile::EntryReader::operator=(EntryReader&&) noexcept = default;

bool ZipFile::EntryReader::open(const char* filename) {
  impl_->reset();

  FileStatSlim fileStat = {};
  if (!impl_->zf.loadFileStatSlim(filename, &fileStat)) {
    LOG_ERR("ZIP", "EntryReader::open: entry not found: %s", filename);
    return false;
  }
  const long dataOffset = impl_->zf.getDataOffset(fileStat);
  if (dataOffset < 0) {
    LOG_ERR("ZIP", "EntryReader::open: bad data offset for %s", filename);
    return false;
  }

  impl_->inflatedSize_ = fileStat.uncompressedSize;
  impl_->method = fileStat.method;

  if (!Storage.openFileForRead("ZIP", impl_->zf.filePath, impl_->file)) {
    LOG_ERR("ZIP", "EntryReader::open: failed to open zip file");
    return false;
  }
  impl_->file.seek(static_cast<size_t>(dataOffset));

  if (fileStat.method == ZIP_METHOD_STORED) {
    impl_->storedRemaining = fileStat.uncompressedSize;
    return true;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    impl_->readBuf = static_cast<uint8_t*>(malloc(impl_->chunkSize));
    if (!impl_->readBuf) {
      LOG_ERR("ZIP", "EntryReader::open: OOM allocating read buffer");
      impl_->file.close();
      return false;
    }
    impl_->ctx.file = &impl_->file;
    impl_->ctx.fileRemaining = fileStat.compressedSize;
    impl_->ctx.readBuf = impl_->readBuf;
    impl_->ctx.readBufSize = impl_->chunkSize;
    // Ring sized to the entry (≤32 KB): small chapters cost a small ring, which is what
    // makes holding the reader across background-build slices affordable.
    if (!impl_->ctx.reader.init(true, fileStat.uncompressedSize)) {
      LOG_ERR("ZIP", "EntryReader::open: OOM initialising inflate ring buffer");
      free(impl_->readBuf);
      impl_->readBuf = nullptr;
      impl_->file.close();
      return false;
    }
    impl_->ctx.reader.setReadCallback(zipReadCallback);
    return true;
  }

  LOG_ERR("ZIP", "EntryReader::open: unsupported compression method %u", fileStat.method);
  impl_->file.close();
  return false;
}

bool ZipFile::EntryReader::step(uint8_t* out, const size_t cap, size_t* produced, bool* done) {
  *produced = 0;
  *done = false;

  if (impl_->done_) {
    *done = true;
    return true;
  }
  if (impl_->error_) return false;

  if (impl_->method == ZIP_METHOD_STORED) {
    if (impl_->storedRemaining == 0) {
      impl_->done_ = true;
      *done = true;
      return true;
    }
    const size_t toRead = cap < impl_->storedRemaining ? cap : impl_->storedRemaining;
    const int n = impl_->file.read(out, toRead);
    if (n <= 0) {
      LOG_ERR("ZIP", "EntryReader::step: stored read error");
      impl_->error_ = true;
      return false;
    }
    impl_->storedRemaining -= static_cast<size_t>(n);
    impl_->bytesProduced_ += static_cast<size_t>(n);
    *produced = static_cast<size_t>(n);
    if (impl_->storedRemaining == 0) {
      impl_->done_ = true;
      *done = true;
    }
    return true;
  }

  if (impl_->method == ZIP_METHOD_DEFLATED) {
    size_t p = 0;
    const InflateStatus status = impl_->ctx.reader.readAtMost(out, cap, &p);
    impl_->bytesProduced_ += p;
    *produced = p;
    if (status == InflateStatus::Done) {
      impl_->done_ = true;
      *done = true;
      return true;
    }
    if (status == InflateStatus::Error) {
      LOG_ERR("ZIP", "EntryReader::step: inflate error");
      impl_->error_ = true;
      return false;
    }
    return true;
  }

  impl_->error_ = true;
  return false;
}

void ZipFile::EntryReader::close() { impl_->reset(); }
bool ZipFile::EntryReader::isOpen() const { return impl_ && static_cast<bool>(impl_->file); }
size_t ZipFile::EntryReader::inflatedSize() const { return impl_ ? impl_->inflatedSize_ : 0; }
size_t ZipFile::EntryReader::bytesProduced() const { return impl_ ? impl_->bytesProduced_ : 0; }
