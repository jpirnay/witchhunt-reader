#include <gtest/gtest.h>

#include <string>

#include "OpdsFilename.h"

namespace {

TEST(OpdsFilename, AuthorTitleIsDefaultOrder) {
  EXPECT_EQ(opdsBookFilename("J. Doe", "My Book", OpdsFilenameFormat::AuthorTitle, ".epub"), "J. Doe - My Book.epub");
}

TEST(OpdsFilename, TitleAuthorSwapsOrder) {
  EXPECT_EQ(opdsBookFilename("J. Doe", "My Book", OpdsFilenameFormat::TitleAuthor, ".epub"), "My Book - J. Doe.epub");
}

TEST(OpdsFilename, TitleOnlyIgnoresAuthor) {
  EXPECT_EQ(opdsBookFilename("J. Doe", "My Book", OpdsFilenameFormat::TitleOnly, ".epub"), "My Book.epub");
}

TEST(OpdsFilename, EmptyAuthorCollapsesToTitleForEveryFormat) {
  EXPECT_EQ(opdsBookFilename("", "My Book", OpdsFilenameFormat::AuthorTitle, ".epub"), "My Book.epub");
  EXPECT_EQ(opdsBookFilename("", "My Book", OpdsFilenameFormat::TitleAuthor, ".epub"), "My Book.epub");
  EXPECT_EQ(opdsBookFilename("", "My Book", OpdsFilenameFormat::TitleOnly, ".epub"), "My Book.epub");
}

TEST(OpdsFilename, IllegalCharactersAreSanitized) {
  // '/' ':' '*' '?' etc. are replaced with '_' by sanitizeFilename.
  EXPECT_EQ(opdsBookFilename("A/B", "C:D*E?", OpdsFilenameFormat::AuthorTitle, ".epub"), "A_B - C_D_E_.epub");
}

TEST(OpdsFilename, EmptyAuthorAndTitleFallsBackToBook) {
  // sanitizeFilename returns "book" when nothing usable remains.
  EXPECT_EQ(opdsBookFilename("", "", OpdsFilenameFormat::AuthorTitle, ".epub"), "book.epub");
  EXPECT_EQ(opdsBookFilename("", "", OpdsFilenameFormat::TitleOnly, ".epub"), "book.epub");
}

TEST(OpdsFilename, LongNameIsTruncatedToByteBudgetBeforeExtension) {
  // sanitizeFilename caps the base at 100 bytes; ".epub" is appended after.
  const std::string longTitle(200, 'a');
  const std::string result = opdsBookFilename("", longTitle, OpdsFilenameFormat::TitleOnly, ".epub");
  EXPECT_EQ(result, std::string(100, 'a') + ".epub");
  EXPECT_EQ(result.size(), 105u);
}

TEST(OpdsFilename, UnknownFormatValueFallsBackToAuthorTitle) {
  // Defensive: a persisted value outside the enum still yields a valid name.
  const auto bogus = static_cast<OpdsFilenameFormat>(99);
  EXPECT_EQ(opdsBookFilename("J. Doe", "My Book", bogus, ".epub"), "J. Doe - My Book.epub");
}

}  // namespace
