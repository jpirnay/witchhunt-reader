// The Hyphenator's fallback breaks (a word the language patterns could not split) fall only between the syllables of
// an Indic word, never inside one. With no language patterns installed every break is a fallback break.
#include <IndicScripts.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/Epub/Epub/hyphenation/Hyphenator.h"
#include "lib/Epub/Epub/hyphenation/LiangHyphenation.h"

namespace {

std::vector<uint32_t> codepointsOf(const std::string& word) {
  std::vector<uint32_t> cps;
  const auto* p = reinterpret_cast<const unsigned char*>(word.c_str());
  while (const uint32_t cp = utf8NextCodepoint(&p)) cps.push_back(cp);
  return cps;
}

// The code points on either side of the byte offset `offset` of `word`.
std::pair<uint32_t, uint32_t> neighbours(const std::string& word, const size_t offset) {
  return {codepointsOf(word.substr(0, offset)).back(), codepointsOf(word.substr(offset)).front()};
}

}  // namespace

TEST(IndicFallback, ABreakNeverFallsInsideASyllable) {
  Hyphenator::setLanguageHyphenator(nullptr);
  for (const std::string& word : {std::string("प्रार्थनासंस्कृतम्कृष्णज्ञानम्"), std::string("শ্রীঈশ্বরচন্দ্রকর্তৃক")}) {
    const auto breaks = Hyphenator::breakOffsets(word, true);
    EXPECT_GE(breaks.size(), 3u) << word;
    for (const auto& b : breaks) {
      const auto [before, after] = neighbours(word, b.byteOffset);
      EXPECT_TRUE(indic::syllableBreakAllowed(before, after)) << word << " at byte " << b.byteOffset;
    }
  }
}

// Pins that a word outside the Indic blocks keeps a break at every index the prefix and suffix minimums leave.
TEST(IndicFallback, ALatinWordsFallbackBreaksAreUnchanged) {
  Hyphenator::setLanguageHyphenator(nullptr);
  const std::string word = "Donaudampfschiffahrt";
  const auto breaks = Hyphenator::breakOffsets(word, true);
  const size_t first = LiangWordConfig::kDefaultMinPrefix;
  const size_t last = word.size() - LiangWordConfig::kDefaultMinSuffix;
  ASSERT_EQ(last - first + 1, breaks.size());
  for (size_t i = 0; i < breaks.size(); ++i) EXPECT_EQ(first + i, breaks[i].byteOffset);
}
