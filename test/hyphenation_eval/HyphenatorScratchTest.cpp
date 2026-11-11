#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/Epub/Epub/hyphenation/Hyphenator.h"

// Hyphenator::breakOffsets in a caller's Scratch finds exactly the breaks the allocating form returns, and a scratch
// grown to the longest word so far takes the next words without growing again: a layout splits word after word with
// no allocation per word.

namespace {

bool sameBreaks(const std::vector<Hyphenator::BreakInfo>& a, const std::vector<Hyphenator::BreakInfo>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].byteOffset != b[i].byteOffset || a[i].requiresInsertedHyphen != b[i].requiresInsertedHyphen) return false;
  }
  return true;
}

// Patterns, explicit and soft hyphens, an apostrophe contraction, a word only the fallback breaks, punctuation around
// a word, a single letter and an empty word.
const std::vector<std::string> kWords = {
    "condition",
    "Satellitensystems",
    "US-Satellitensystems",
    "Quadrat­kilometer",
    "all'improvviso",
    "xqzrtvbnmkjhgfd",
    "“definitely,”",
    "a",
    "",
};

}  // namespace

TEST(HyphenatorScratch, BreaksAreTheSameAsTheAllocatingForm) {
  for (const char* lang : {"en", "de", "it"}) {
    const ScopedLanguageHyphenator scoped(Hyphenator::forLanguage(lang));
    Hyphenator::Scratch scratch;
    for (const bool fallback : {false, true}) {
      for (const std::string& word : kWords) {
        SCOPED_TRACE(std::string(lang) + " \"" + word + "\" fallback=" + (fallback ? "1" : "0"));
        Hyphenator::breakOffsets(word, fallback, scratch);
        EXPECT_TRUE(sameBreaks(scratch.breaks, Hyphenator::breakOffsets(word, fallback)));
      }
    }
  }
}

TEST(HyphenatorScratch, AGrownScratchTakesShorterWordsWithoutGrowing) {
  const ScopedLanguageHyphenator scoped(Hyphenator::forLanguage("en"));
  Hyphenator::Scratch scratch;
  Hyphenator::breakOffsets("incomprehensibilities", true, scratch);
  ASSERT_FALSE(scratch.breaks.empty());
  const auto* cps = scratch.cps.data();
  const auto* indexes = scratch.indexes.data();
  const auto* breaks = scratch.breaks.data();
  for (const char* word : {"condition", "hyphenation", "reading"}) {
    Hyphenator::breakOffsets(word, true, scratch);
    EXPECT_EQ(scratch.cps.data(), cps) << word;
    EXPECT_EQ(scratch.indexes.data(), indexes) << word;
    EXPECT_EQ(scratch.breaks.data(), breaks) << word;
  }
  scratch.release();
  EXPECT_EQ(scratch.cps.capacity(), 0u);
  EXPECT_EQ(scratch.indexes.capacity(), 0u);
  EXPECT_EQ(scratch.breaks.capacity(), 0u);
}
