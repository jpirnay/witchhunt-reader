#include "CardStringsFile.h"

#include <FileReplace.h>
#include <Logging.h>

#include <cstdio>

CardStringsFile::CardStringsFile(const char* path) : path_(path) { snprintf(tmp_, sizeof(tmp_), "%s.tmp", path); }

bool CardStringsFile::ensureOpen() {
  const uint32_t generation = Storage.contentGeneration();
  if (in_.isOpen() && generation == generation_) return true;
  in_ = HalFile();  // closed, if it was open
  // Not while the card is not the firmware's (USB Drive) or not mounted: the open would fail anyway, and log it.
  if (!Storage.ready()) return false;
  if (!Storage.openFileForRead("LCS", path_, in_)) {
    in_ = HalFile();
    return false;
  }
  generation_ = generation;
  return true;
}

uint32_t CardStringsFile::size() { return ensureOpen() ? static_cast<uint32_t>(in_.fileSize()) : 0; }

bool CardStringsFile::read(const uint32_t offset, void* dst, const uint32_t len) {
  return ensureOpen() && in_.seekSet(offset) && in_.read(dst, len) == static_cast<int>(len);
}

void CardStringsFile::release() { in_ = HalFile(); }

bool CardStringsFile::create() {
  out_ = HalFile();
  return Storage.openFileForWrite("LCS", tmp_, out_);
}

bool CardStringsFile::append(const void* src, const uint32_t len) {
  return out_.isOpen() && out_.write(src, len) == len;
}

bool CardStringsFile::patch(const uint32_t offset, const void* src, const uint32_t len) {
  if (!out_.isOpen()) return false;
  const size_t end = out_.fileSize();
  return out_.seekSet(offset) && out_.write(src, len) == len && out_.seekSet(end);
}

bool CardStringsFile::commit() {
  if (!out_.isOpen()) return false;
  out_.flush();
  out_ = HalFile();
  in_ = HalFile();
  return FileReplace::replace(tmp_, path_);
}

void CardStringsFile::discard() {
  out_ = HalFile();
  Storage.remove(tmp_);
}
