#pragma once

#include <cstddef>
#include <string>
#include <vector>

class LanguageHyphenator;

class Hyphenator {
 public:
  struct BreakInfo {
    size_t byteOffset;            // Byte position inside the UTF-8 word where a break may occur.
    bool requiresInsertedHyphen;  // true = a visible '-' must be rendered at the break (pattern/fallback breaks).
                                  // false = break occurs at an existing visible separator boundary
                                  //         (explicit '-' or eligible apostrophe contraction boundary).
  };

  // Returns byte offsets where the word may be hyphenated.
  //
  // Break sources (in priority order):
  //   1. Explicit hyphens already present in the word (e.g. '-' or soft-hyphen U+00AD).
  //      When found, language patterns are additionally run on each alphabetic segment
  //      between separators so compound words can break within their parts.
  //      Example: "US-Satellitensystems" yields breaks after "US-" (no inserted hyphen)
  //               plus pattern breaks inside "Satellitensystems" (Sa|tel|li|ten|sys|tems).
  //   2. Apostrophe contractions between letters (e.g. all'improvviso).
  //      Liang patterns are run per alphabetic segment around apostrophes.
  //      A direct break at the apostrophe boundary is allowed only when the left
  //      segment has at least 3 letters and the right segment has at least 3 letters,
  //      avoiding short clitics (e.g. l', d') and contraction tails (e.g. 've, 're, 'll).
  //   3. Language-specific Liang patterns (e.g. German de_patterns).
  //      Example: "Quadratkilometer" -> Qua|drat|ki|lo|me|ter.
  //   4. Fallback every-N-chars splitting (only when includeFallback is true AND no
  //      pattern breaks were found). Used as a last resort to prevent a single oversized
  //      word from overflowing the page width.
  static std::vector<BreakInfo> breakOffsets(const std::string& word, bool includeFallback);

  // Provide a publication-level language hint (e.g. "en", "en-US", "ru") used to select hyphenation rules.
  static void setPreferredLanguage(const std::string& lang);

  // The patterns for a language tag ("en", "en-US", "eng"), null when the language has none. The book's
  // TypographyProfile holds this, and the section build installs it with setLanguageHyphenator().
  static const LanguageHyphenator* forLanguage(const std::string& lang);
  static void setLanguageHyphenator(const LanguageHyphenator* hyphenator) { cachedHyphenator_ = hyphenator; }
  // The patterns breakOffsets() uses now: the last installed, null when none is.
  static const LanguageHyphenator* languageHyphenator() { return cachedHyphenator_; }

 private:
  static const LanguageHyphenator* cachedHyphenator_;
};

// Installs a language's patterns for a layout that is not a section build, and puts back the ones it found. Without
// it such a layout hyphenates by whatever book the last section build was for, or by nothing when no build has run
// since boot.
class ScopedLanguageHyphenator {
 public:
  explicit ScopedLanguageHyphenator(const LanguageHyphenator* hyphenator)
      : previous_(Hyphenator::languageHyphenator()) {
    Hyphenator::setLanguageHyphenator(hyphenator);
  }
  ~ScopedLanguageHyphenator() { Hyphenator::setLanguageHyphenator(previous_); }
  ScopedLanguageHyphenator(const ScopedLanguageHyphenator&) = delete;
  ScopedLanguageHyphenator& operator=(const ScopedLanguageHyphenator&) = delete;

 private:
  const LanguageHyphenator* previous_;
};