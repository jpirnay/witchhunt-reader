#include "HighlightStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <ctime>

namespace {
bool readString16(FsFile& f, std::string& out, const size_t max) {
  uint16_t length = 0;
  if (!serialization::tryReadPod(f, length) || length > max) return false;
  out.resize(length);
  return length == 0 || f.read(reinterpret_cast<uint8_t*>(&out[0]), length) == length;
}

// Cuts to at most `max` bytes without splitting a UTF-8 sequence.
void truncateUtf8(std::string& s, const size_t max) {
  if (s.size() <= max) return;
  size_t cut = max;
  while (cut > 0 && (static_cast<uint8_t>(s[cut]) & 0xC0) == 0x80) cut--;
  s.resize(cut);
}

bool writeString16(FsFile& f, const std::string& s, const size_t max) {
  const uint16_t length = static_cast<uint16_t>(std::min(s.size(), max));
  return serialization::tryWritePod(f, length) &&
         (length == 0 || f.write(reinterpret_cast<const uint8_t*>(s.data()), length) == length);
}
}  // namespace

bool HighlightStore::load(const std::string& stateDir) {
  dir = stateDir;
  highlights.clear();
  dirty = false;
  if (dir.empty()) return false;
  if (!Storage.exists(filePath().c_str())) return true;

  FsFile f;
  if (!Storage.openFileForRead("HLS", filePath(), f)) return false;
  uint8_t version = 0;
  uint16_t count = 0;
  if (!serialization::tryReadPod(f, version) || version != FILE_VERSION || !serialization::tryReadPod(f, count) ||
      count > MAX_PER_BOOK) {
    LOG_ERR("HLS", "Unreadable highlights file %s", filePath().c_str());
    f.close();
    return false;
  }
  highlights.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    Highlight h;
    if (!serialization::tryReadPod(f, h.timestamp) || !serialization::tryReadPod(f, h.spineIndex) ||
        !serialization::tryReadPod(f, h.paragraphHint) || !serialization::tryReadPod(f, h.progressQ) ||
        !readString16(f, h.chapter, CHAPTER_MAX) || !readString16(f, h.text, TEXT_MAX)) {
      LOG_ERR("HLS", "Truncated highlights file at entry %u", static_cast<unsigned>(i));
      highlights.clear();
      f.close();
      return false;
    }
    highlights.push_back(std::move(h));
  }
  f.close();
  return true;
}

bool HighlightStore::save() {
  if (!dirty || dir.empty()) return true;
  FsFile f;
  if (!Storage.openFileForWrite("HLS", filePath(), f)) {
    LOG_ERR("HLS", "Failed to open %s for writing", filePath().c_str());
    return false;
  }
  const uint16_t count = static_cast<uint16_t>(std::min<size_t>(highlights.size(), MAX_PER_BOOK));
  bool ok = serialization::tryWritePod(f, FILE_VERSION) && serialization::tryWritePod(f, count);
  for (uint16_t i = 0; ok && i < count; i++) {
    const Highlight& h = highlights[i];
    ok = serialization::tryWritePod(f, h.timestamp) && serialization::tryWritePod(f, h.spineIndex) &&
         serialization::tryWritePod(f, h.paragraphHint) && serialization::tryWritePod(f, h.progressQ) &&
         writeString16(f, h.chapter, CHAPTER_MAX) && writeString16(f, h.text, TEXT_MAX);
  }
  ok = f.close() && ok;
  if (!ok) {
    LOG_ERR("HLS", "Failed while writing highlights");
    return false;
  }
  dirty = false;
  return true;
}

uint32_t HighlightStore::add(const uint16_t spineIndex, const uint16_t paragraphHint, const uint16_t progressQ,
                             std::string chapter, std::string text) {
  if (highlights.size() >= MAX_PER_BOOK || text.empty()) return 0;
  const time_t now = time(nullptr);
  if (now < 1577836800) return 0;  // clock never set: no identity to mint
  uint32_t ts = static_cast<uint32_t>(now);
  // Timestamps are sync identities, so two highlights made in the same second must differ.
  while (std::any_of(highlights.begin(), highlights.end(), [ts](const Highlight& h) { return h.timestamp == ts; })) {
    ts++;
  }
  Highlight h;
  h.timestamp = ts;
  h.spineIndex = spineIndex;
  h.paragraphHint = paragraphHint;
  h.progressQ = progressQ;
  truncateUtf8(chapter, CHAPTER_MAX);
  truncateUtf8(text, TEXT_MAX);
  h.chapter = std::move(chapter);
  h.text = std::move(text);
  highlights.push_back(std::move(h));
  dirty = true;
  return ts;
}

bool HighlightStore::removeByTimestamp(const uint32_t timestamp) {
  auto it = std::find_if(highlights.begin(), highlights.end(),
                         [timestamp](const Highlight& h) { return h.timestamp == timestamp; });
  if (it == highlights.end()) return false;
  highlights.erase(it);
  dirty = true;
  return true;
}

void HighlightStore::removeAt(const size_t index) {
  if (index >= highlights.size()) return;
  highlights.erase(highlights.begin() + static_cast<long>(index));
  dirty = true;
}

bool HighlightStore::setText(const uint32_t timestamp, std::string text) {
  auto it = std::find_if(highlights.begin(), highlights.end(),
                         [timestamp](const Highlight& h) { return h.timestamp == timestamp; });
  if (it == highlights.end() || text.empty()) return false;
  truncateUtf8(text, TEXT_MAX);
  if (it->text == text) return true;
  it->text = std::move(text);
  dirty = true;
  return true;
}
