#include "LangCardStrings.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace langcard {

namespace {

constexpr uint32_t WRITE_BYTES = 512;

void put16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, const uint32_t v) {
  put16(p, static_cast<uint16_t>(v));
  put16(p + 2, static_cast<uint16_t>(v >> 16));
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t get32(const uint8_t* p) { return get16(p) | static_cast<uint32_t>(get16(p + 2)) << 16; }

// Where the strings producer writes: the bytes go to the file in WRITE_BYTES pieces, and each key's place and length
// into the table, which is written over its placeholder once every string is in.
class FileStringsWriter final : public FlashLangPartition::StringsWriter {
 public:
  FileStringsWriter(StringsFile& file, const uint32_t limit, uint8_t* buffer, uint8_t* table, const uint16_t keyCount,
                    uint8_t* scratch)
      : file_(file), limit_(limit), buffer_(buffer), table_(table), keyCount_(keyCount), scratch_(scratch) {}

  bool beginKey(const uint16_t id) override {
    const uint32_t at = written_ + fill_;
    // Bit 15 of an offset is FALLBACK, so a string starts below 32 KB, as in the slot.
    if (failed_ || id >= keyCount_ || at >= FlashLangPartition::FALLBACK) {
      LOG_ERR("LCS", "key %u refused at %u B", id, static_cast<unsigned>(at));
      failed_ = true;
      return false;
    }
    closeKey(at);
    put16(table_ + id * ENTRY_BYTES, static_cast<uint16_t>(at));
    key_ = id;
    keyStart_ = at;
    return true;
  }

  bool write(const void* data, size_t len) override {
    if (failed_ || written_ + fill_ + len > limit_) {
      failed_ = true;
      return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    while (len > 0) {
      const size_t take = std::min<size_t>(len, WRITE_BYTES - fill_);
      memcpy(buffer_ + fill_, bytes, take);
      fill_ += static_cast<uint32_t>(take);
      bytes += take;
      len -= take;
      if (fill_ == WRITE_BYTES && !flush()) return false;
    }
    return true;
  }

  uint8_t* scratch() override { return scratch_; }
  size_t scratchBytes() const override { return FlashLangPartition::SCRATCH_BYTES; }

  // Writes what is still buffered and the last key's length. The data's length, once this succeeds.
  bool finish() {
    if (!flush()) return false;
    closeKey(written_);
    return !failed_;
  }
  uint32_t written() const { return written_; }

 private:
  // The key begun last runs up to `end`, its NUL included. A string longer than a chunk cannot be read into the
  // arena; it keeps its place but reads as English.
  void closeKey(const uint32_t end) {
    if (key_ == NONE) return;
    const uint32_t bytes = end - keyStart_;
    if (bytes == 0 || bytes > CHUNK_BYTES) {
      LOG_ERR("LCS", "key %u: a string of %u B, which reads as English", key_, static_cast<unsigned>(bytes));
      put16(table_ + key_ * ENTRY_BYTES, FlashLangPartition::FALLBACK);
      put16(table_ + key_ * ENTRY_BYTES + 2, 0);
    } else {
      put16(table_ + key_ * ENTRY_BYTES + 2, static_cast<uint16_t>(bytes));
    }
    key_ = NONE;
  }

  bool flush() {
    if (failed_) return false;
    if (fill_ == 0) return true;
    if (!file_.append(buffer_, fill_)) {
      LOG_ERR("LCS", "write failed after %u B of strings", static_cast<unsigned>(written_));
      failed_ = true;
      return false;
    }
    written_ += fill_;
    fill_ = 0;
    return true;
  }

  static constexpr uint16_t NONE = 0xFFFF;

  StringsFile& file_;
  uint32_t limit_;
  uint8_t* buffer_;
  uint8_t* table_;
  uint16_t keyCount_;
  uint8_t* scratch_;
  uint32_t fill_ = 0;
  uint32_t written_ = 0;
  uint16_t key_ = NONE;
  uint32_t keyStart_ = 0;
  bool failed_ = false;
};

}  // namespace

CardStrings::~CardStrings() {
  for (uint32_t i = 0; i < chunkCount_; ++i) delete[] chunks_[i];
  file_.release();
}

bool CardStrings::holdsPack(const uint8_t langIndex, const uint32_t stamp, const uint16_t keyCount) {
  uint8_t h[HEADER_BYTES];
  if (!file_.read(0, h, sizeof h)) return false;
  const uint32_t dataLen = get32(h + 12);
  const uint32_t expected = HEADER_BYTES + static_cast<uint32_t>(keyCount) * ENTRY_BYTES + dataLen;
  if (memcmp(h, MAGIC, sizeof MAGIC) != 0 || h[4] != FORMAT_VERSION || h[5] != langIndex || get16(h + 6) != keyCount ||
      get32(h + 8) != stamp || get32(h + 16) != 0 || dataLen > FlashLangPartition::FALLBACK ||
      file_.size() != expected) {
    return false;
  }
  dataLen_ = dataLen;
  return true;
}

bool CardStrings::build(const uint8_t langIndex, const uint32_t stamp, const uint16_t keyCount,
                        const FlashLangPartition::Strings& strings) {
  const uint32_t tableBytes = static_cast<uint32_t>(keyCount) * ENTRY_BYTES;
  // Transient: freed before this returns.
  auto scratch = makeUniqueNoThrow<uint8_t[]>(FlashLangPartition::SCRATCH_BYTES);
  auto buffer = makeUniqueNoThrow<uint8_t[]>(WRITE_BYTES);
  auto table = makeUniqueNoThrow<uint8_t[]>(tableBytes);
  if (!scratch || !buffer || !table) {
    LOG_ERR("LCS", "no memory to unpack language %u", langIndex);
    return false;
  }
  // Every key English until the producer writes its string.
  for (uint32_t id = 0; id < keyCount; ++id) {
    put16(table.get() + id * ENTRY_BYTES, FlashLangPartition::FALLBACK);
    put16(table.get() + id * ENTRY_BYTES + 2, 0);
  }

  // The file in place may be open for reading; it is about to be replaced.
  file_.release();
  if (!file_.create()) {
    LOG_ERR("LCS", "cannot write the strings file");
    return false;
  }
  // The header and the table as placeholders, written over once the strings are in: zeros, which read as no file.
  memset(buffer.get(), 0, WRITE_BYTES);
  bool ok = true;
  for (uint32_t done = 0; ok && done < HEADER_BYTES + tableBytes;) {
    const uint32_t take = std::min(WRITE_BYTES, HEADER_BYTES + tableBytes - done);
    ok = file_.append(buffer.get(), take);
    done += take;
  }
  const uint32_t limit = std::min<uint32_t>(strings.maxBytes, FlashLangPartition::FALLBACK);
  FileStringsWriter writer(file_, limit, buffer.get(), table.get(), keyCount, scratch.get());
  ok = ok && strings.produce(strings.ctx, writer) && writer.finish();
  if (ok) ok = file_.patch(HEADER_BYTES, table.get(), tableBytes);
  if (ok) {
    uint8_t h[HEADER_BYTES] = {};
    memcpy(h, MAGIC, sizeof MAGIC);
    h[4] = FORMAT_VERSION;
    h[5] = langIndex;
    put16(h + 6, keyCount);
    put32(h + 8, stamp);
    put32(h + 12, writer.written());
    ok = file_.patch(0, h, sizeof h);
  }
  if (!ok || !file_.commit()) {
    LOG_ERR("LCS", "language %u: the strings file could not be written", langIndex);
    file_.discard();
    return false;
  }
  LOG_INF("LCS", "language %u unpacked to the card: %u B of strings for %u keys", langIndex,
          static_cast<unsigned>(writer.written()), keyCount);
  return true;
}

bool CardStrings::open(const uint8_t langIndex, const uint32_t stamp, const uint16_t keyCount,
                       const FlashLangPartition::Strings& strings) {
  if (keyCount == 0 || handles_ || !strings.produce) return false;
  std::lock_guard<std::mutex> hold(lock_);
  built_ = false;
  if (!holdsPack(langIndex, stamp, keyCount)) {
    if (!build(langIndex, stamp, keyCount, strings)) return false;
    built_ = true;
    if (!holdsPack(langIndex, stamp, keyCount)) {
      LOG_ERR("LCS", "language %u: the strings file just written does not read back", langIndex);
      return false;
    }
  }
  handles_ = makeUniqueNoThrow<uint16_t[]>(keyCount);
  if (!handles_) {
    LOG_ERR("LCS", "no memory for %u handles", keyCount);
    return false;
  }
  std::fill(handles_.get(), handles_.get() + keyCount, UNREAD);
  keyCount_ = keyCount;
  return true;
}

const char* CardStrings::get(const uint16_t id) {
  if (id >= keyCount_) return nullptr;
  // Acquire: the string's bytes and its chunk's pointer were stored before the handle was (load()).
  const uint16_t h = __atomic_load_n(&handles_[id], __ATOMIC_ACQUIRE);
  if (h < ENGLISH) return chunks_[h >> 10] + (h & 0x3FF);
  if (h == ENGLISH) return nullptr;
  return load(id);
}

void CardStrings::deferRetry() {
  retryAt_ = static_cast<uint32_t>(millis()) + RETRY_AFTER_MS;
  retryPending_ = true;
}

bool CardStrings::reserve(const uint32_t bytes, char** at, uint16_t* handle) {
  if (tail_ + bytes > CHUNK_BYTES) {
    if (chunkCount_ == MAX_CHUNKS || (mayGrow_ && !mayGrow_(CHUNK_BYTES))) return false;
    auto* const chunk = new (std::nothrow) char[CHUNK_BYTES];
    if (!chunk) return false;
    chunks_[chunkCount_++] = chunk;
    tail_ = 0;
    LOG_INF("LCS", "arena chunk %u taken: %u B of strings read so far", static_cast<unsigned>(chunkCount_),
            static_cast<unsigned>(interned_));
  }
  const uint32_t chunk = chunkCount_ - 1;
  *at = chunks_[chunk] + tail_;
  *handle = static_cast<uint16_t>(chunk << 10 | tail_);
  tail_ += bytes;
  return true;
}

const char* CardStrings::load(const uint16_t id) {
  std::lock_guard<std::mutex> hold(lock_);
  // Another task may have read it while this one waited for the lock.
  const uint16_t h = handles_[id];
  if (h < ENGLISH) return chunks_[h >> 10] + (h & 0x3FF);
  if (h == ENGLISH) return nullptr;
  if (retryPending_ && static_cast<int32_t>(static_cast<uint32_t>(millis()) - retryAt_) < 0) return nullptr;
  retryPending_ = false;

  ++cardReads_;
  uint8_t entry[ENTRY_BYTES];
  if (!file_.read(HEADER_BYTES + static_cast<uint32_t>(id) * ENTRY_BYTES, entry, sizeof entry)) {
    LOG_ERR("LCS", "key %u: the strings file does not read - English for now", id);
    file_.release();
    deferRetry();
    return nullptr;
  }
  const uint16_t offset = get16(entry);
  const uint16_t bytes = get16(entry + 2);
  if (offset & FlashLangPartition::FALLBACK) {
    __atomic_store_n(&handles_[id], ENGLISH, __ATOMIC_RELEASE);
    return nullptr;
  }
  if (bytes == 0 || bytes > CHUNK_BYTES || static_cast<uint32_t>(offset) + bytes > dataLen_) {
    LOG_ERR("LCS", "key %u: a table entry outside the strings (%u + %u of %u B)", id, offset, bytes,
            static_cast<unsigned>(dataLen_));
    __atomic_store_n(&handles_[id], ENGLISH, __ATOMIC_RELEASE);
    return nullptr;
  }

  char* at = nullptr;
  uint16_t handle = 0;
  if (!reserve(bytes, &at, &handle)) {
    LOG_DBG("LCS", "key %u: no chunk for %u B - English for now", id, bytes);
    deferRetry();
    return nullptr;
  }
  const uint32_t dataOff = HEADER_BYTES + static_cast<uint32_t>(keyCount_) * ENTRY_BYTES;
  const bool read = file_.read(dataOff + offset, at, bytes);
  if (!read || at[bytes - 1] != '\0') {
    tail_ -= bytes;  // the arena's tail, and nothing points into it
    if (!read) {
      LOG_ERR("LCS", "key %u: its string does not read - English for now", id);
      file_.release();
      deferRetry();
    } else {
      LOG_ERR("LCS", "key %u: its string is not terminated where the table says", id);
      __atomic_store_n(&handles_[id], ENGLISH, __ATOMIC_RELEASE);
    }
    return nullptr;
  }
  interned_ += bytes;
  // Release: the bytes above before the handle that leads to them.
  __atomic_store_n(&handles_[id], handle, __ATOMIC_RELEASE);
  return at;
}

}  // namespace langcard
