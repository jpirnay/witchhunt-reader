#pragma once

#include "LiangHyphenation.h"

// Generic Liang-backed hyphenator that stores pattern metadata plus language-specific helpers.
//
// The patterns are held by value: a language whose trie comes from a pack (LanguageRegistry.h) starts without any and
// is given them once its pack is mapped. Without patterns (data null, size 0) breakIndexes() finds no break, so a
// hyphenator whose pack was taken away is harmless rather than dangling.
class LanguageHyphenator {
 public:
  LanguageHyphenator(const SerializedHyphenationPatterns& patterns, bool (*isLetterFn)(uint32_t),
                     uint32_t (*toLowerFn)(uint32_t), size_t minPrefix = LiangWordConfig::kDefaultMinPrefix,
                     size_t minSuffix = LiangWordConfig::kDefaultMinSuffix)
      : patterns_(patterns), config_(isLetterFn, toLowerFn, minPrefix, minSuffix) {}

  std::vector<size_t> breakIndexes(const std::vector<CodepointInfo>& cps) const {
    if (!patterns_.data) return {};
    return liangBreakIndexes(cps, patterns_, config_);
  }

  size_t minPrefix() const { return config_.minPrefix; }
  size_t minSuffix() const { return config_.minSuffix; }

  bool hasPatterns() const { return patterns_.data != nullptr; }
  const SerializedHyphenationPatterns& patterns() const { return patterns_; }
  void setPatterns(const SerializedHyphenationPatterns& patterns) { patterns_ = patterns; }

 protected:
  SerializedHyphenationPatterns patterns_;
  LiangWordConfig config_;
};
