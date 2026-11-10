#pragma once

#include <cstddef>
#include <cstdint>
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

  // Where a line may end inside the Myanmar text of `word`, in ascending order, none drawing a hyphen: between two
  // syllables of a run, where the fallback's rule allows a cut (myanmar::breakAllowed), whatever the hyphenation
  // setting. Burmese writes no spaces between words, so a phrase is split at a line end as a Hangul word is
  // (the Myanmar design's PR 3). Empty for a word without Myanmar text, and between Myanmar and another script.
  static std::vector<BreakInfo> myanmarLineEndBreaks(const std::string& word);

  // Provide a publication-level language hint (e.g. "en", "en-US", "ru") used to select hyphenation rules.
  static void setPreferredLanguage(const std::string& lang);

  // The patterns for a language tag ("en", "en-US", "eng"), null when the language has none. The book's
  // TypographyProfile holds this, and the section build installs it with setLanguageHyphenator().
  static const LanguageHyphenator* forLanguage(const std::string& lang);
  // The code of the language a tag hyphenates by ("de" for "de-AT" or "ger"), whether or not its patterns are
  // available; null when the firmware does not hyphenate the language. Names its pack (hyph-<code>.cphyph).
  static const char* languageCode(const std::string& lang);
  // The patterns' part of a section's cache key for a book in this language (hyphenationLayoutKey in
  // LanguageRegistry.h): 0 for the patterns the image carried before packs. 0 with hyphenation off, without asking
  // the pack provider (which maps the trie on the X3 and X4): such a layout keeps its key whatever pack is installed.
  static uint32_t layoutKey(const std::string& lang, bool hyphenationEnabled);
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