#include "PsramImageFlash.h"

#include <Logging.h>
#include <Memory.h>
#include <esp_rom_crc.h>

#include <cstddef>
#include <cstring>

namespace {

constexpr char kMagic[4] = {'W', 'H', 'F', 'A'};
constexpr uint8_t kVersion = 1;
constexpr uint8_t kClean = 0xC1;
constexpr uint8_t kDirty = 0xD1;

// The file's first bytes; the rest of the header block is 0xFF.
struct ImageHeader {
  char magic[4];
  uint8_t version;
  uint8_t state;  // kClean: the image behind it is whole; anything else is no image
  uint16_t reserved;
  uint32_t areaBytes;
  uint32_t crc;  // of the bytes before it
};
static_assert(sizeof(ImageHeader) == 16, "image header layout changed");

uint32_t headerCrc(const ImageHeader& h) {
  return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&h), offsetof(ImageHeader, crc));
}

constexpr uint32_t bitWords(const uint32_t bits) { return (bits + 31) / 32; }
bool testBit(const uint32_t* bits, const uint32_t i) { return ((bits[i / 32] >> (i % 32)) & 1u) != 0; }
void setBit(uint32_t* bits, const uint32_t i) { bits[i / 32] |= 1u << (i % 32); }

}  // namespace

bool PsramImageFlash::begin() {
  std::lock_guard<std::mutex> hold(lock_);
  if (image_ == nullptr || bytes_ == 0 || bytes_ % kMapPageBytes != 0) {
    LOG_ERR("FLA", "PSRAM image of %u B is not whole pages", static_cast<unsigned>(bytes_));
    return false;
  }
  const uint32_t pageWords = bitWords(bytes_ / kMapPageBytes);
  const uint32_t sectorWords = bitWords(bytes_ / kSectorBytes);
  // Held for the run: a bit per page and per sector, 112 B for the default 3,456 KB.
  loaded_ = makeUniqueNoThrow<uint32_t[]>(pageWords);
  dirty_ = makeUniqueNoThrow<uint32_t[]>(sectorWords);
  if (!loaded_ || !dirty_) {
    LOG_ERR("FLA", "no memory for the PSRAM image's bitmaps (%u B)",
            static_cast<unsigned>((pageWords + sectorWords) * sizeof(uint32_t)));
    loaded_.reset();
    dirty_.reset();
    return false;
  }
  std::memset(loaded_.get(), 0, pageWords * sizeof(uint32_t));
  std::memset(dirty_.get(), 0, sectorWords * sizeof(uint32_t));

  if (file_ == nullptr) {
    // Nothing to load: load() erases each page as it is first touched, so a boot pays for the pages it uses rather
    // than for a memset of the whole image.
    LOG_INF("FLA", "PSRAM image: %u KB, erased, kept in PSRAM only", static_cast<unsigned>(bytes_ / 1024));
    return true;
  }
  ImageHeader h{};
  fileWhole_ = file_->size() == kHeaderBlockBytes + bytes_ && file_->read(0, &h, sizeof h) &&
               std::memcmp(h.magic, kMagic, sizeof kMagic) == 0 && h.version == kVersion && h.state == kClean &&
               h.areaBytes == bytes_ && h.crc == headerCrc(h);
  if (fileWhole_) {
    LOG_INF("FLA", "PSRAM image: %u KB on the card, loaded as it is used", static_cast<unsigned>(bytes_ / 1024));
  } else {
    startErased();
    LOG_INF("FLA", "PSRAM image: %u KB, erased (no clean image on the card)", static_cast<unsigned>(bytes_ / 1024));
  }
  return true;
}

void PsramImageFlash::startErased() {
  std::memset(image_, 0xFF, bytes_);
  for (uint32_t page = 0; page < bytes_ / kMapPageBytes; ++page) setBit(loaded_.get(), page);
  // Every sector, so the next sync writes the whole file.
  for (uint32_t sector = 0; sector < bytes_ / kSectorBytes; ++sector) setBit(dirty_.get(), sector);
  fileWhole_ = false;
}

bool PsramImageFlash::allLoaded() const {
  for (uint32_t page = 0; page < bytes_ / kMapPageBytes; ++page) {
    if (!testBit(loaded_.get(), page)) return false;
  }
  return true;
}

bool PsramImageFlash::load(const uint32_t offset, const uint32_t len, const bool erasing) const {
  if (len == 0) return true;
  const uint32_t last = (offset + len - 1) / kMapPageBytes;
  for (uint32_t page = offset / kMapPageBytes; page <= last; ++page) {
    if (testBit(loaded_.get(), page)) continue;
    const uint32_t start = page * kMapPageBytes;
    const bool coveredWhole = erasing && offset <= start && start + kMapPageBytes <= offset + len;
    if (!coveredWhole) {
      if (file_ == nullptr) {
        std::memset(image_ + start, 0xFF, kMapPageBytes);
      } else if (!file_->read(kHeaderBlockBytes + start, image_ + start, kMapPageBytes)) {
        LOG_ERR("FLA", "PSRAM image: page %u could not be read from the card", static_cast<unsigned>(page));
        return false;
      }
    }
    setBit(loaded_.get(), page);
  }
  return true;
}

void PsramImageFlash::markDirty(const uint32_t offset, const uint32_t len) {
  if (len == 0) return;
  const uint32_t last = (offset + len - 1) / kSectorBytes;
  for (uint32_t sector = offset / kSectorBytes; sector <= last; ++sector) setBit(dirty_.get(), sector);
}

bool PsramImageFlash::read(const uint32_t offset, void* dst, const uint32_t len) const {
  std::lock_guard<std::mutex> hold(lock_);
  if (!inRange(offset, len) || !load(offset, len, false)) return false;
  if (len != 0) std::memcpy(dst, image_ + offset, len);
  return true;
}

bool PsramImageFlash::program(const uint32_t offset, const void* src, const uint32_t len) {
  std::lock_guard<std::mutex> hold(lock_);
  if (!inRange(offset, len) || !load(offset, len, false)) return false;
  const auto* in = static_cast<const uint8_t*>(src);
  for (uint32_t i = 0; i < len; ++i) image_[offset + i] &= in[i];
  markDirty(offset, len);
  return true;
}

bool PsramImageFlash::erase(const uint32_t offset, const uint32_t len) {
  std::lock_guard<std::mutex> hold(lock_);
  if (!inRange(offset, len) || offset % kSectorBytes != 0 || len % kSectorBytes != 0) return false;
  if (!load(offset, len, true)) return false;
  std::memset(image_ + offset, 0xFF, len);
  markDirty(offset, len);
  return true;
}

const uint8_t* PsramImageFlash::map(const uint32_t offset, const uint32_t len) {
  std::lock_guard<std::mutex> hold(lock_);
  if (len == 0 || !inRange(offset, len) || !load(offset, len, false)) return nullptr;
  return image_ + offset;
}

bool PsramImageFlash::writeHeader(const uint8_t state, const bool wholeBlock) {
  ImageHeader h{};
  std::memcpy(h.magic, kMagic, sizeof kMagic);
  h.version = kVersion;
  h.state = state;
  h.areaBytes = bytes_;
  h.crc = headerCrc(h);
  if (!file_->write(0, &h, sizeof h)) return false;
  if (!wholeBlock) return true;
  uint8_t pad[128];
  std::memset(pad, 0xFF, sizeof pad);
  for (uint32_t at = sizeof h; at < kHeaderBlockBytes;) {
    const uint32_t n = kHeaderBlockBytes - at < sizeof pad ? kHeaderBlockBytes - at : static_cast<uint32_t>(sizeof pad);
    if (!file_->write(at, pad, n)) return false;
    at += n;
  }
  return true;
}

bool PsramImageFlash::sync() {
  std::lock_guard<std::mutex> hold(lock_);
  if (!dirty_) return false;
  if (file_ == nullptr) return true;
  const uint32_t sectors = bytes_ / kSectorBytes;
  bool any = false;
  for (uint32_t s = 0; s < sectors && !any; ++s) any = testBit(dirty_.get(), s);
  if (!any) return true;

  // The file may have gone since begin() (a cleared cache): a new one needs every page, which only a fully loaded
  // image has. Otherwise this run's writes stay in PSRAM, and the next boot starts erased.
  if (fileWhole_ && file_->size() != kHeaderBlockBytes + bytes_) fileWhole_ = false;
  if (!fileWhole_) {
    if (!allLoaded()) {
      LOG_ERR("FLA", "PSRAM image: the card's image is gone and part of it was never loaded - not synced");
      return false;
    }
    for (uint32_t s = 0; s < sectors; ++s) setBit(dirty_.get(), s);
    if (!file_->truncate() || !writeHeader(kDirty, true)) return false;
  } else if (!writeHeader(kDirty, false)) {
    return false;
  }
  if (!file_->sync()) return false;

  uint32_t written = 0;
  for (uint32_t s = 0; s < sectors;) {
    if (!testBit(dirty_.get(), s)) {
      ++s;
      continue;
    }
    uint32_t end = s + 1;
    while (end < sectors && testBit(dirty_.get(), end)) ++end;
    const uint32_t at = s * kSectorBytes;
    const uint32_t len = (end - s) * kSectorBytes;
    if (!file_->write(kHeaderBlockBytes + at, image_ + at, len)) {
      LOG_ERR("FLA", "PSRAM image: %u B at %u could not be written", static_cast<unsigned>(len),
              static_cast<unsigned>(at));
      return false;
    }
    written += len;
    s = end;
  }
  if (!file_->sync() || !writeHeader(kClean, false) || !file_->sync()) return false;

  std::memset(dirty_.get(), 0, bitWords(sectors) * sizeof(uint32_t));
  fileWhole_ = true;
  LOG_DBG("FLA", "PSRAM image: %u B synced", static_cast<unsigned>(written));
  return true;
}
