#pragma once

#include "LanguageHyphenator.h"
#include "LiangHyphenation.h"

class LiangLanguageHyphenator final : public LanguageHyphenator {
 public:
  LiangLanguageHyphenator(const SerializedHyphenationPatterns& patterns, bool (*isLetterFn)(uint32_t),
                          uint32_t (*toLowerFn)(uint32_t), size_t minPrefix = LiangWordConfig::kDefaultMinPrefix,
                          size_t minSuffix = LiangWordConfig::kDefaultMinSuffix)
      : patterns_(patterns), config_(isLetterFn, toLowerFn, minPrefix, minSuffix) {}

  std::vector<size_t> breakIndexes(const std::vector<CodepointInfo>& cps) const override {
    return liangBreakIndexes(cps, patterns_, config_);
  }

  size_t minPrefix() const override { return config_.minPrefix; }

  size_t minSuffix() const override { return config_.minSuffix; }

  BreakDecoration breakDecoration() const override { return BreakDecoration::InsertHyphen; }

 private:
  const SerializedHyphenationPatterns& patterns_;
  LiangWordConfig config_;
};