#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "PsramImageFlash.h"

/// The card file of a PsramImageFlash, for host tests.
///
/// - What reads see (the file as the filesystem knows it) and what survives a power cut are kept apart: write() and
///   truncate() reach the card only at sync(), in their order, as SdFat's cache leaves them. With writeThrough(true)
///   they reach it at once, as a cache that is flushed early may.
/// - Power cuts: after cutAfterBytes(n), n more bytes reach the card; the write in flight stops there (a prefix of it
///   lands), and every later write, truncate and sync fails until powerCycle(). A truncate counts as one byte.
/// - powerCycle(): a reboot. The file is what reached the card.
class SimImageFile final : public AreaImageFile {
 public:
  uint32_t size() override { return static_cast<uint32_t>(seen_.size()); }

  bool read(const uint32_t offset, void* dst, const uint32_t len) override {
    if (failReads_ || offset > seen_.size() || len > seen_.size() - offset) return false;
    if (len != 0) std::memcpy(dst, seen_.data() + offset, len);
    ++reads_;
    bytesRead_ += len;
    return true;
  }

  bool write(const uint32_t offset, const void* src, const uint32_t len) override {
    if (dead_ || refuseWrites_ || offset > seen_.size()) return false;
    const auto* bytes = static_cast<const uint8_t*>(src);
    Op op{false, offset, std::vector<uint8_t>(bytes, bytes + len)};
    apply(seen_, op);
    bytesWritten_ += len;
    if (writeThrough_) return land(op);
    pending_.push_back(std::move(op));
    return true;
  }

  bool sync() override {
    if (dead_ || refuseWrites_) return false;
    for (const Op& op : pending_) {
      if (!land(op)) {
        pending_.clear();
        return false;
      }
    }
    pending_.clear();
    return true;
  }

  bool truncate() override {
    if (dead_ || refuseWrites_) return false;
    seen_.clear();
    Op op{true, 0, {}};
    if (writeThrough_) return land(op);
    pending_.push_back(std::move(op));
    return true;
  }

  // --- test controls ---
  void cutAfterBytes(const int64_t bytes) { budget_ = bytes; }
  void writeThrough(const bool on) { writeThrough_ = on; }
  void refuseWrites(const bool on) { refuseWrites_ = on; }
  void failReads(const bool on) { failReads_ = on; }
  void powerCycle() {
    pending_.clear();
    seen_ = card_;
    dead_ = false;
    budget_ = -1;
  }
  /// The file is deleted (a cleared cache), on the card at once.
  void remove() {
    card_.clear();
    seen_.clear();
    pending_.clear();
  }

  // --- what happened ---
  bool dead() const { return dead_; }
  uint32_t reads() const { return reads_; }
  uint64_t bytesRead() const { return bytesRead_; }
  uint64_t bytesWritten() const { return bytesWritten_; }
  const std::vector<uint8_t>& card() const { return card_; }

 private:
  struct Op {
    bool truncate;
    uint32_t offset;
    std::vector<uint8_t> bytes;
  };

  static void apply(std::vector<uint8_t>& file, const Op& op, const size_t count = SIZE_MAX) {
    if (op.truncate) {
      file.clear();
      return;
    }
    const size_t n = std::min(count, op.bytes.size());
    if (file.size() < op.offset + n) file.resize(op.offset + n);
    std::copy_n(op.bytes.begin(), n, file.begin() + op.offset);
  }

  // Puts `op` on the card, as far as the power lasts.
  bool land(const Op& op) {
    if (dead_) return false;
    const size_t cost = op.truncate ? 1 : op.bytes.size();
    if (budget_ >= 0 && static_cast<int64_t>(cost) > budget_) {
      if (!op.truncate) apply(card_, op, static_cast<size_t>(budget_));
      budget_ = 0;
      dead_ = true;
      return false;
    }
    if (budget_ >= 0) budget_ -= static_cast<int64_t>(cost);
    apply(card_, op);
    return true;
  }

  std::vector<uint8_t> card_;  // what survives a power cut
  std::vector<uint8_t> seen_;  // what reads see
  std::vector<Op> pending_;
  int64_t budget_ = -1;
  bool dead_ = false;
  bool writeThrough_ = false;
  bool refuseWrites_ = false;
  bool failReads_ = false;
  uint32_t reads_ = 0;
  uint64_t bytesRead_ = 0;
  uint64_t bytesWritten_ = 0;
};
