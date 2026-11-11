#include "FlashAreaRegion.h"

#include <Logging.h>

#include "FlashArea.h"
#include "FlashFontPartition.h"

namespace {
// The font area's mappings (begin()), not a region's (beginRegion()): see liveFontAreaMappings().
uint32_t s_liveFontAreaMappings = 0;
}  // namespace

FlashAreaRegion::~FlashAreaRegion() {
  for (const uint8_t* m : maps_) {
    if (m != nullptr) unmap(m);
  }
}

uint32_t FlashAreaRegion::liveFontAreaMappings() { return s_liveFontAreaMappings; }

bool FlashAreaRegion::begin() {
  area_ = FlashArea::get();
  if (!area_) {
    LOG_ERR("ENF", "no flash area");
    return false;
  }
  base_ = 0;
  size_ = static_cast<uint32_t>(FlashFontPartition::fontUsableSize());
  fontArea_ = true;
  return size_ > 0;
}

bool FlashAreaRegion::beginRegion(const uint32_t base, const uint32_t size) {
  area_ = FlashArea::get();
  if (!area_ || base % kMapPageBytes != 0 || base > area_->size() || size > area_->size() - base) {
    LOG_ERR("ENF", "no region of %u B at %u", static_cast<unsigned>(size), static_cast<unsigned>(base));
    area_ = nullptr;
    return false;
  }
  base_ = base;
  size_ = size;
  fontArea_ = false;
  return size_ > 0;
}

bool FlashAreaRegion::read(const uint32_t offset, void* dst, const uint32_t len) const {
  if (!inRange(offset, len)) return false;
  return area_->read(base_ + offset, dst, len);
}

bool FlashAreaRegion::writable(const char* what, const uint32_t offset, const uint32_t len) const {
  if (!inRange(offset, len)) {
    LOG_ERR("ENF", "%s of %u B at %u is outside the font area (%u B)", what, static_cast<unsigned>(len),
            static_cast<unsigned>(offset), static_cast<unsigned>(size_));
    return false;
  }
  if (fontArea_ && (FlashFontPartition::isMapped() || FlashFontPartition::isWriting())) {
    LOG_ERR("ENF", "%s refused: a .cpfont is mapped from, or being written to, this partition", what);
    return false;
  }
  return true;
}

bool FlashAreaRegion::program(const uint32_t offset, const void* src, const uint32_t len) {
  if (!writable("program", offset, len)) return false;
  return area_->program(base_ + offset, src, len);
}

bool FlashAreaRegion::erase(const uint32_t offset, const uint32_t len) {
  if (!writable("erase", offset, len)) return false;
  if (offset % kSectorBytes != 0 || len % kSectorBytes != 0) {
    LOG_ERR("ENF", "erase of %u B at %u is not sector-aligned", static_cast<unsigned>(len),
            static_cast<unsigned>(offset));
    return false;
  }
  return area_->erase(base_ + offset, len);
}

const uint8_t* FlashAreaRegion::map(const uint32_t offset, const uint32_t len) {
  if (len == 0 || !inRange(offset, len)) return nullptr;
  for (int i = 0; i < kMaxMaps; ++i) {
    if (maps_[i] != nullptr) continue;
    const uint8_t* const ptr = area_->map(base_ + offset, len);
    if (ptr == nullptr) return nullptr;
    maps_[i] = ptr;
    if (fontArea_) {
      fontAreaMaps_ |= static_cast<uint8_t>(1u << i);
      ++s_liveFontAreaMappings;
    }
    return ptr;
  }
  LOG_ERR("ENF", "map refused: all %d mappings are in use", kMaxMaps);
  return nullptr;
}

void FlashAreaRegion::unmap(const uint8_t* mapped) {
  if (mapped == nullptr) return;
  for (int i = 0; i < kMaxMaps; ++i) {
    if (maps_[i] != mapped) continue;
    area_->unmap(mapped);
    const auto bit = static_cast<uint8_t>(1u << i);
    if (fontAreaMaps_ & bit) {
      fontAreaMaps_ = static_cast<uint8_t>(fontAreaMaps_ & ~bit);
      --s_liveFontAreaMappings;
    }
    maps_[i] = nullptr;
    return;
  }
}

uint32_t FlashAreaRegion::freeMapPages() const { return area_ ? area_->freeMapPages() : 0; }
