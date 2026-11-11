#pragma once

#include "PsramImageFlash.h"

/// The PSRAM image's file on the card (FlashArea, backend b), through HalStorage. Each call opens the file and closes
/// it again, so nothing holds it between calls (a cleared cache may delete it, USB mass storage may take the card),
/// and every write is on the card when it returns: sync() has nothing left to do.
class CardImageFile final : public AreaImageFile {
 public:
  explicit CardImageFile(const char* path) : path_(path) {}

  uint32_t size() override;
  bool read(uint32_t offset, void* dst, uint32_t len) override;
  bool write(uint32_t offset, const void* src, uint32_t len) override;
  bool sync() override { return true; }
  bool truncate() override;

 private:
  const char* path_;
};
