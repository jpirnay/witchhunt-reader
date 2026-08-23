#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "HyphenationCommon.h"

// What should visually happen when the line is broken at a language-generated
// break opportunity?
//
// Western hyphenation normally inserts a visible hyphen.
// East-Asian line breaking normally does not.
enum class BreakDecoration : uint8_t {
  InsertHyphen,
  None,
};

class LanguageHyphenator {
 public:
  virtual ~LanguageHyphenator() = default;

  // Returns codepoint indexes at which a line may be broken.
  // Index N means: break before cps[N].
  virtual std::vector<size_t> breakIndexes(const std::vector<CodepointInfo>& cps) const = 0;

  virtual size_t minPrefix() const = 0;
  virtual size_t minSuffix() const = 0;

  virtual BreakDecoration breakDecoration() const = 0;
};