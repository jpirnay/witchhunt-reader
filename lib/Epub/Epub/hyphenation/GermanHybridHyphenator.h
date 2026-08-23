#pragma once

#include "LanguageHyphenator.h"

class GermanHybridHyphenator final : public LanguageHyphenator {
 public:
  static constexpr size_t kMinPrefix = 2;
  static constexpr size_t kMinSuffix = 2;

  std::vector<size_t> breakIndexes(const std::vector<CodepointInfo>& cps) const override;

  size_t minPrefix() const override { return kMinPrefix; }

  size_t minSuffix() const override { return kMinSuffix; }

  BreakDecoration breakDecoration() const override { return BreakDecoration::InsertHyphen; }
};