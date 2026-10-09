// How the book index tells books and authors apart, and the order authors file in (spec §3.3, §3.4).

#include <LibraryFormat.h>
#include <LibraryKeys.h>
#include <gtest/gtest.h>

#include <string>

namespace {

TEST(LibraryKeys, FoldLowercasesAndReducesLatinLettersToTheirBase) {
  EXPECT_EQ(LibraryKeys::fold("\xC3\x89mile Zola"), "emile zola");                     // É
  EXPECT_EQ(LibraryKeys::fold("\xC3\x96" "d\xC3\xB6n von Horv\xC3\xA1th"), "odon von horvath");  // Ö ö á
  EXPECT_EQ(LibraryKeys::fold("\xC5\x81ukasz Orbitowski"), "lukasz orbitowski");       // Ł
  EXPECT_EQ(LibraryKeys::fold("Dvo\xC5\x99\xC3\xA1k"), "dvorak");                      // ř á
}

TEST(LibraryKeys, FoldSpellsOutLettersThatAreTwo) {
  EXPECT_EQ(LibraryKeys::fold("Stra\xC3\x9F" "e"), "strasse");          // ß
  EXPECT_EQ(LibraryKeys::fold("\xC3\x86r\xC3\xB8"), "aero");            // Æ ø
  EXPECT_EQ(LibraryKeys::fold("\xC5\x92uvre"), "oeuvre");               // Œ
}

// macOS writes names decomposed: "O" followed by a combining diaeresis must file with "Ö".
TEST(LibraryKeys, FoldDropsCombiningMarksSoDecomposedNamesMatch) {
  EXPECT_EQ(LibraryKeys::fold("O\xCC\x88zil"), LibraryKeys::fold("\xC3\x96zil"));
  EXPECT_EQ(LibraryKeys::fold("O\xCC\x88zil"), "ozil");
}

// A pretty-printed OPF can leave line breaks inside a name (review minor M2 of plan 1).
TEST(LibraryKeys, FoldCollapsesWhitespaceRuns) {
  EXPECT_EQ(LibraryKeys::fold("  Ursula K.\n   Le\tGuin \xC2\xA0"), "ursula k. le guin");
}

TEST(LibraryKeys, FoldKeepsOtherScriptsAsTheyAre) {
  EXPECT_EQ(LibraryKeys::fold("\xD0\x9B\xD0\xB5\xD0\xB2"), "\xD0\x9B\xD0\xB5\xD0\xB2");  // Лев
}

TEST(LibraryKeys, TheSortKeyIsTheFileAsWhenThereIsOne) {
  EXPECT_EQ(LibraryKeys::authorSortKey("Ursula K. Le Guin", "Le Guin, Ursula K."), "le guin ursula k");
}

TEST(LibraryKeys, ANameWithACommaSortsAsWritten) {
  EXPECT_EQ(LibraryKeys::authorSortKey("Le Guin, Ursula K.", ""), "le guin ursula k");
}

TEST(LibraryKeys, OtherwiseTheLastWordComesFirst) {
  EXPECT_EQ(LibraryKeys::authorSortKey("Terry Pratchett", ""), "pratchett terry");
  EXPECT_EQ(LibraryKeys::authorSortKey("Homer", ""), "homer");
  EXPECT_EQ(LibraryKeys::authorSortKey("", ""), "");
}

TEST(LibraryKeys, TheAuthorHashIgnoresCaseAccentsAndSpacing) {
  EXPECT_EQ(LibraryKeys::authorHash("\xC3\x89mile  Zola"), LibraryKeys::authorHash("emile zola"));
  EXPECT_NE(LibraryKeys::authorHash("Emile Zola"), LibraryKeys::authorHash("Emil Zola"));
  EXPECT_EQ(LibraryKeys::authorHash(""), library::AUTHOR_UNKNOWN);
  EXPECT_EQ(LibraryKeys::authorHash(" \n "), library::AUTHOR_UNKNOWN);
  EXPECT_NE(LibraryKeys::authorHash("Terry Pratchett"), library::AUTHOR_PENDING);
}

TEST(LibraryKeys, ABooksIdentityIgnoresCaseButNotSizeOrName) {
  EXPECT_EQ(LibraryKeys::bookIdentity("Mort.epub", 1000), LibraryKeys::bookIdentity("MORT.EPUB", 1000));
  EXPECT_NE(LibraryKeys::bookIdentity("Mort.epub", 1000), LibraryKeys::bookIdentity("Mort.epub", 1001));
  EXPECT_NE(LibraryKeys::bookIdentity("Mort.epub", 1000), LibraryKeys::bookIdentity("Mort2.epub", 1000));
}

}  // namespace
