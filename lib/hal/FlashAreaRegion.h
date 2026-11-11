#pragma once

#include "NorFlash.h"

/// A window of the flash area (FlashArea::get()) for one store: the font area, everything below the hyphenation region
/// and the language slot (FlashFontPartition::fontUsableSize()), for the TTF glyph store; or the hyphenation region.
/// The two stores reach the area only through this class.
///
/// The font area is shared with FlashFontPartition (.cpfont families), so the two exclude each other:
/// program() and erase() are refused while a .cpfont is mapped or being written, and FlashFontPartition
/// refuses to write or map while this class holds a mapping of the font area (liveFontAreaMappings()). Not
/// thread-safe; callers serialise (RenderLock). Destroying it releases any mapping still held.
class FlashAreaRegion final : public NorFlash {
 public:
  FlashAreaRegion() = default;
  ~FlashAreaRegion() override;
  FlashAreaRegion(const FlashAreaRegion&) = delete;
  FlashAreaRegion& operator=(const FlashAreaRegion&) = delete;

  /// Over the font area. False when the board has no area, or none larger than the reservations above the font area.
  bool begin();
  /// Over `size` bytes of the area from `base` instead of the font area: the hyphenation region. No .cpfont is
  /// ever written there, so its writes are not refused while one is mapped. `base` must be 64 KB aligned.
  bool beginRegion(uint32_t base, uint32_t size);

  uint32_t size() const override { return size_; }
  bool read(uint32_t offset, void* dst, uint32_t len) const override;
  bool program(uint32_t offset, const void* src, uint32_t len) override;
  bool erase(uint32_t offset, uint32_t len) override;
  const uint8_t* map(uint32_t offset, uint32_t len) override;
  void unmap(const uint8_t* mapped) override;
  uint32_t freeMapPages() const override;

  /// The font area's mappings held by every FlashAreaRegion (begin(): the TTF glyph store's): FlashFontPartition stays
  /// off the area while this is not 0. A region's (beginRegion(): the hyphenation store's tries, mapped while a
  /// book is open) are not counted: no .cpfont reaches the region, nor does the store reach the font area.
  static uint32_t liveFontAreaMappings();

 private:
  static constexpr int kMaxMaps = 4;  // the glyph regions, a style (or the copied tables), and spares

  bool inRange(const uint32_t offset, const uint32_t len) const {
    return area_ != nullptr && offset <= size_ && len <= size_ - offset;
  }
  bool writable(const char* what, uint32_t offset, uint32_t len) const;

  NorFlash* area_ = nullptr;
  uint32_t base_ = 0;  // the window's start in the area: 0 for the font area
  uint32_t size_ = 0;
  bool fontArea_ = true;
  // Bit i: maps_[i] was made over the font area and is counted in liveFontAreaMappings(), so it is released as it was
  // counted whichever area the instance is begun over by then. In fontArea_'s padding.
  uint8_t fontAreaMaps_ = 0;
  const uint8_t* maps_[kMaxMaps] = {};
};
