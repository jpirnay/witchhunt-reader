#include <gtest/gtest.h>

#include <string>

#include "lib/Epub/Epub/hyphenation/Hyphenator.h"

// The patterns a layout hyphenates by are one global, which a section build installs for its book. A layout that is
// not a section build (a styled dictionary definition) installs its own for its scope and puts the old one back.

namespace {

// Pattern breaks only: without the fallback, a word breaks only where the installed language's patterns allow it.
size_t patternBreaks(const std::string& text) { return Hyphenator::breakOffsets(text, false).size(); }

}  // namespace

TEST(ScopedLanguageHyphenator, ALayoutHyphenatesByTheLanguageItInstalls) {
  // As after a build of a book with no patterns (Hebrew): nothing is installed.
  Hyphenator::setLanguageHyphenator(nullptr);
  ASSERT_EQ(patternBreaks("condition"), 0u);
  {
    const ScopedLanguageHyphenator scoped(Hyphenator::forLanguage("en"));
    EXPECT_EQ(Hyphenator::languageHyphenator(), Hyphenator::forLanguage("en"));
    EXPECT_GT(patternBreaks("condition"), 0u);
  }
}

TEST(ScopedLanguageHyphenator, TheLanguageBeforeItComesBackAfterwards) {
  Hyphenator::setLanguageHyphenator(Hyphenator::forLanguage("de"));
  {
    const ScopedLanguageHyphenator scoped(Hyphenator::forLanguage("en"));
    EXPECT_EQ(Hyphenator::languageHyphenator(), Hyphenator::forLanguage("en"));
  }
  EXPECT_EQ(Hyphenator::languageHyphenator(), Hyphenator::forLanguage("de"));
  Hyphenator::setLanguageHyphenator(nullptr);
}

TEST(ScopedLanguageHyphenator, ALanguageWithoutPatternsHyphenatesNothing) {
  Hyphenator::setLanguageHyphenator(Hyphenator::forLanguage("en"));
  {
    const ScopedLanguageHyphenator scoped(Hyphenator::forLanguage("he"));
    EXPECT_EQ(patternBreaks("condition"), 0u);
  }
  EXPECT_GT(patternBreaks("condition"), 0u);
  Hyphenator::setLanguageHyphenator(nullptr);
}
