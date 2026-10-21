#include "FontBytes.h"

namespace ttf {

const uint8_t* SdPageCache::page(const FileSource& file, const uint32_t index) {
  // An outline is read a few bytes at a time, almost always from the page the last read came from.
  if (owner_[last_] == file.ctx && index_[last_] == index) return data_[last_];
  uint8_t slot = 0;
  for (uint8_t i = 0; i < kPages; ++i) {
    if (owner_[i] == file.ctx && index_[i] == index) {
      used_[i] = ++clock_;
      last_ = i;
      return data_[i];
    }
    if (used_[i] < used_[slot]) slot = i;
  }
  const uint32_t start = index * kPageBytes;
  if (start >= file.size || !file.read) return nullptr;
  const uint32_t bytes = file.size - start < kPageBytes ? file.size - start : kPageBytes;
  owner_[slot] = nullptr;  // a failed read leaves the page empty
  if (!file.read(file.ctx, start, data_[slot], bytes)) return nullptr;
  ++misses_;
  owner_[slot] = file.ctx;
  index_[slot] = index;
  used_[slot] = ++clock_;
  last_ = slot;
  return data_[slot];
}

bool SdPageCache::read(const FileSource& file, uint32_t offset, uint32_t len, void* dst) {
  if (file.ctx == nullptr || offset > file.size || len > file.size - offset) return false;
  auto* out = static_cast<uint8_t*>(dst);
  while (len > 0) {
    const uint8_t* page = this->page(file, offset / kPageBytes);
    if (!page) return false;
    const uint32_t at = offset % kPageBytes;
    const uint32_t n = len < kPageBytes - at ? len : kPageBytes - at;
    std::memcpy(out, page + at, n);
    out += n;
    offset += n;
    len -= n;
  }
  return true;
}

const uint8_t* SdPageCache::window(const FileSource& file, const uint32_t offset, const uint32_t len, uint32_t* got) {
  // read()'s checks for one byte at `offset`.
  if (file.ctx == nullptr || offset >= file.size) return nullptr;
  const uint8_t* page = this->page(file, offset / kPageBytes);
  if (!page) return nullptr;
  const uint32_t at = offset % kPageBytes;
  uint32_t n = kPageBytes - at;
  if (n > file.size - offset) n = file.size - offset;
  *got = n < len ? n : len;
  return page + at;
}

}  // namespace ttf

const uint8_t* FontBytes::windowFar(const uint32_t offset, const uint32_t len, uint32_t* got) {
  // readFar()'s checks for one byte at `offset`.
  if (sd_ && offset >= sd_->fontOffset && offset - sd_->fontOffset < sd_->length) {
    const uint32_t rel = offset - sd_->fontOffset;
    const uint32_t room = sd_->length - rel;
    const uint8_t* p =
        sd_->cache ? sd_->cache->window(sd_->file, sd_->fileOffset + rel, len < room ? len : room, got) : nullptr;
    if (p) return p;
    ioError_ = true;
    return nullptr;
  }
  outOfRange_ = true;
  return nullptr;
}

bool FontBytes::readFar(const uint32_t offset, const uint32_t len, void* dst) {
  if (sd_ && offset >= sd_->fontOffset && offset - sd_->fontOffset <= sd_->length &&
      len <= sd_->length - (offset - sd_->fontOffset)) {
    if (sd_->cache && sd_->cache->read(sd_->file, sd_->fileOffset + (offset - sd_->fontOffset), len, dst)) return true;
    ioError_ = true;
    return false;
  }
  outOfRange_ = true;
  return false;
}
