#pragma once

#include "LiangHyphenation.h"

// Generic Liang-backed hyphenator that stores pattern metadata plus language-specific helpers.
//
// The patterns are held by value: a language whose trie comes from a pack (LanguageRegistry.h) is given them once its
// pack is mapped, in one of the registry's pack slots. Without patterns (data null, size 0) breakIndexes() finds no
// break, so a hyphenator whose pack was taken away is harmless rather than dangling.
class LanguageHyphenator {
 public:
  // A pack slot's, before the registry gives it a language.
  LanguageHyphenator() : patterns_{0, nullptr, 0}, config_(nullptr, nullptr) {}
  LanguageHyphenator(const SerializedHyphenationPatterns& patterns, bool (*isLetterFn)(uint32_t),
                     uint32_t (*toLowerFn)(uint32_t), size_t minPrefix = LiangWordConfig::kDefaultMinPrefix,
                     size_t minSuffix = LiangWordConfig::kDefaultMinSuffix, bool insertsHyphen = true)
      : patterns_(patterns), config_(isLetterFn, toLowerFn, minPrefix, minSuffix), insertsHyphen_(insertsHyphen) {}

  std::vector<size_t> breakIndexes(const std::vector<CodepointInfo>& cps) const {
    if (!patterns_.data) return {};
    return liangBreakIndexes(cps, patterns_, config_);
  }
  // The same into `out` (cleared first), whose capacity a caller keeps from word to word.
  void breakIndexes(const std::vector<CodepointInfo>& cps, std::vector<size_t>& out) const {
    out.clear();
    if (patterns_.data) liangBreakIndexes(cps, patterns_, config_, out);
  }

  size_t minPrefix() const { return config_.minPrefix; }
  size_t minSuffix() const { return config_.minSuffix; }
  // Whether a break inside a word shows a hyphen at the line's end. False for the Indic scripts, whose lines break
  // between syllables without one, as Typst's hypher (where the tries come from) does.
  bool insertsHyphen() const { return insertsHyphen_; }
  // One of the language's letters, as its patterns are matched against.
  bool isLetter(const uint32_t cp) const { return config_.isLetter && config_.isLetter(cp); }

  bool hasPatterns() const { return patterns_.data != nullptr; }
  const SerializedHyphenationPatterns& patterns() const { return patterns_; }
  void setPatterns(const SerializedHyphenationPatterns& patterns) { patterns_ = patterns; }

 protected:
  SerializedHyphenationPatterns patterns_;
  LiangWordConfig config_;
  bool insertsHyphen_ = true;
};
