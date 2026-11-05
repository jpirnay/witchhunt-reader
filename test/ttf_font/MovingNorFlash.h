#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "NorFlash.h"
#include "SimNorFlash.h"

/// A NorFlash over a SimNorFlash whose mappings move, as the ESP-IDF MMU's do (the glyph store maps one flash-mode
/// style block at a time): map() gives a fresh heap copy of the range at a new address every time, program() and
/// erase() reach the chip and every live copy that overlaps the range, and unmap() fills the copy with 0xA5 and frees
/// it. The store keeps working through it; whatever still points into an unmapped copy reads poison.
class MovingNorFlash final : public NorFlash {
 public:
  explicit MovingNorFlash(SimNorFlash& chip) : chip_(chip) {}

  uint32_t size() const override { return chip_.size(); }
  bool read(const uint32_t offset, void* dst, const uint32_t len) const override {
    return chip_.read(offset, dst, len);
  }

  bool program(const uint32_t offset, const void* src, const uint32_t len) override {
    const bool done = chip_.program(offset, src, len);
    refresh(offset, len);
    return done;
  }

  bool erase(const uint32_t offset, const uint32_t len) override {
    const bool done = chip_.erase(offset, len);
    refresh(offset, len);
    return done;
  }

  const uint8_t* map(const uint32_t offset, const uint32_t len) override {
    const uint8_t* const chipView = chip_.map(offset, len);
    if (!chipView) return nullptr;
    // The allocator may hand a freed copy's address to the next, so the view is shifted inside its block until it is
    // one never given before: a mapping always lands somewhere new.
    auto copy = std::make_unique<Copy>();
    copy->offset = offset;
    copy->chipView = chipView;
    // At most given_.size() shifts collide with a view given before, so the slack holds a free one.
    copy->block.assign(len + 16 * (2 * given_.size() + 2), 0);
    size_t shift = 16 * given_.size();
    while (std::find(given_.begin(), given_.end(), copy->block.data() + shift) != given_.end()) shift += 16;
    copy->view = copy->block.data() + shift;
    copy->length = len;
    std::memcpy(copy->view, chipView, len);
    given_.push_back(copy->view);
    const uint8_t* const mapped = copy->view;
    copies_.push_back(std::move(copy));
    ++maps_;
    return mapped;
  }

  void unmap(const uint8_t* mapped) override {
    for (auto it = copies_.begin(); it != copies_.end(); ++it) {
      if ((*it)->view != mapped) continue;
      std::fill((*it)->view, (*it)->view + (*it)->length, uint8_t{0xA5});
      chip_.unmap((*it)->chipView);
      copies_.erase(it);
      return;
    }
  }

  uint32_t freeMapPages() const override { return chip_.freeMapPages(); }

  /// Maps made so far, and the copies live now.
  uint32_t maps() const { return maps_; }
  size_t liveCopies() const { return copies_.size(); }

 private:
  struct Copy {
    uint32_t offset = 0;
    const uint8_t* chipView = nullptr;
    std::vector<uint8_t> block;  // the view lies inside it
    uint8_t* view = nullptr;
    uint32_t length = 0;
  };

  // The chip's bytes of [offset, offset + len) into every live copy that overlaps them.
  void refresh(const uint32_t offset, const uint32_t len) {
    for (const auto& copy : copies_) {
      const uint64_t from = std::max<uint64_t>(offset, copy->offset);
      const uint64_t to = std::min<uint64_t>(uint64_t{offset} + len, uint64_t{copy->offset} + copy->length);
      if (from >= to) continue;
      chip_.read(static_cast<uint32_t>(from), copy->view + (from - copy->offset), static_cast<uint32_t>(to - from));
    }
  }

  SimNorFlash& chip_;
  std::vector<std::unique_ptr<Copy>> copies_;
  std::vector<const uint8_t*> given_;  // every view given so far
  uint32_t maps_ = 0;
};
