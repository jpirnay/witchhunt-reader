#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * One highlight in a book. Layout-independent by design: what identifies and locates it is
 * the chapter, the text, and a position within the chapter -- never a page number, which moves
 * with every font or margin change.
 */
struct Highlight {
  static constexpr uint16_t NO_PARAGRAPH = 0xFFFF;
  static constexpr uint16_t PROGRESS_UNKNOWN = 0xFFFF;

  uint32_t timestamp = 0;  // creation time, UTC epoch seconds; unique per book, the sync join key
  uint16_t spineIndex = 0;
  uint16_t paragraphHint = NO_PARAGRAPH;  // <p> ordinal near the start; disambiguates repeats only
  uint16_t progressQ = PROGRESS_UNKNOWN;  // start position within the chapter, 0..10000
  std::string chapter;
  std::string text;
};

/**
 * A book's highlights, stored in its BookOrbitBookState directory ("highlights.bin"), so they
 * survive a moved file and "Clear Cache". Small enough to hold whole: at most MAX_PER_BOOK
 * entries of at most TEXT_MAX bytes of text.
 */
class HighlightStore {
 public:
  static constexpr uint16_t MAX_PER_BOOK = 256;
  static constexpr size_t TEXT_MAX = 2048;  // BookOrbit's annotation text cap
  static constexpr size_t CHAPTER_MAX = 96;

  // Loads `stateDir`/highlights.bin (missing file = empty store). Returns false only when the
  // directory is empty or the file is unreadable.
  bool load(const std::string& stateDir);
  bool save();

  // Adds a highlight stamped with the current time. Returns its timestamp, or 0 when the store
  // is full or no plausible clock exists to mint an identity.
  uint32_t add(uint16_t spineIndex, uint16_t paragraphHint, uint16_t progressQ, std::string chapter, std::string text);
  bool removeByTimestamp(uint32_t timestamp);
  // Replaces a highlight's text (e.g. with the exact source text once sync has resolved it).
  bool setText(uint32_t timestamp, std::string text);
  void removeAt(size_t index);

  const std::vector<Highlight>& getAll() const { return highlights; }
  bool isLoaded() const { return !dir.empty(); }
  const std::string& stateDir() const { return dir; }

 private:
  static constexpr uint8_t FILE_VERSION = 1;
  std::string filePath() const { return dir + "/highlights.bin"; }

  std::vector<Highlight> highlights;
  std::string dir;
  bool dirty = false;
};
