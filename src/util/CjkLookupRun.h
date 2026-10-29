#pragma once

#include <cstddef>
#include <cstdint>

// The run of CJK text a dictionary lookup starts on. ~80 bytes, a stack local of its callers; no heap.
class CjkLookupRun {
 public:
  enum class Script : uint8_t { Other, Unspaced, Hangul };
  static constexpr uint8_t MAX_CODEPOINTS = 12;

  // The script of the first codepoint of `text` that cleanWord() keeps (leading ASCII
  // non-alphanumerics and edge punctuation skipped): Unspaced (utf8IsUnspacedCjk), Hangul
  // (utf8IsHangul) or Other, also when nothing is kept.
  static Script scriptOf(const char* text, size_t length);

  // Feeds the page's next word, in reading order. The first word fixes the script and may start
  // with edge punctuation, which is skipped. Unspaced: takes codepoints while they are unspaced CJK
  // and not edge punctuation, across words. Hangul: takes the first word's leading Hangul
  // codepoints and closes. Returns true while the run is open (feed the next word), false once a
  // stop codepoint, the 12-codepoint cap or the script closed it; the word that closes it may
  // still have contributed codepoints.
  bool append(const char* text, size_t length);

  Script script() const { return script_; }
  const char* text() const { return text_; }  // NUL-terminated
  uint8_t bytes() const { return bytes_; }
  uint8_t codepoints() const { return count_; }

  // Words (counted from the first appended, 1-based count) the first `runBytes` bytes of the run
  // come from; 0 when runBytes is 0 or not the end of a codepoint.
  uint8_t wordsCovering(uint8_t runBytes) const;

 private:
  Script script_ = Script::Other;
  bool started_ = false;
  bool closed_ = false;
  uint8_t bytes_ = 0;
  uint8_t count_ = 0;
  uint8_t words_ = 0;                  // words fed so far
  uint8_t end_[MAX_CODEPOINTS] = {};   // byte offset just past each codepoint
  uint8_t word_[MAX_CODEPOINTS] = {};  // index of the word each codepoint came from
  char text_[MAX_CODEPOINTS * 4 + 1] = {};
};
