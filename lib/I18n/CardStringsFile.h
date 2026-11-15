#pragma once

#include <HalStorage.h>

#include "LangCardStrings.h"

/// The card file of langcard::CardStrings, through HalStorage.
///
/// read() keeps the file open between calls: every open walks the folders of its path, and /.crosspoint holds a cache
/// folder per book, so one per string would cost a directory scan each on a large library. The handle is opened again
/// whenever HalStorage::contentGeneration() moved since (a USB Drive session goes up it at its start, when the card is
/// handed to the host), and none is opened while the card is not mounted. A new file is written beside the old one
/// and put in its place by FileReplace::replace().
class CardStringsFile final : public langcard::StringsFile {
 public:
  explicit CardStringsFile(const char* path);

  uint32_t size() override;
  bool read(uint32_t offset, void* dst, uint32_t len) override;
  void release() override;
  bool create() override;
  bool append(const void* src, uint32_t len) override;
  bool patch(uint32_t offset, const void* src, uint32_t len) override;
  bool commit() override;
  void discard() override;

 private:
  bool ensureOpen();

  const char* path_;
  char tmp_[48];
  HalFile in_;
  uint32_t generation_ = 0;
  HalFile out_;
};
