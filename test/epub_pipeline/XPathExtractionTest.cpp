// The string side of KOReader XPaths: pulling the spine and the body-child paragraph out of a
// path without touching the book, and the normalisation the reverse mapper matches on.
//
// These run before any chapter is inflated, so they are what decides the spine even when the
// resolver cannot run; a wrong answer here sends a sync to the wrong chapter.
#include <gtest/gtest.h>

#include <string>

#include "KOReaderSync/ChapterXPathIndexer.h"
#include "KOReaderSync/ChapterXPathIndexerInternal.h"
#include "KOReaderSync/ProgressMapper.h"

namespace {

int spineOf(const std::string& xpath) {
  int spine = -99;
  return ChapterXPathIndexer::tryExtractSpineIndexFromXPath(xpath, spine) ? spine : -1;
}

int paragraphOf(const std::string& xpath) {
  uint16_t p = 0;
  return ChapterXPathIndexer::tryExtractParagraphIndexFromXPath(xpath, p) ? p : -1;
}

// --- DocFragment -> spine ----------------------------------------------------------------------

TEST(XPathSpine, DocFragmentIsOneBased) {
  EXPECT_EQ(spineOf("/body/DocFragment[1]/body"), 0);
  EXPECT_EQ(spineOf("/body/DocFragment[13]/body/section/header/hgroup/h1/span[1]/a/span/text().0"), 12);
}

TEST(XPathSpine, ZeroIsNotASpine) { EXPECT_EQ(spineOf("/body/DocFragment[0]/body/p[1]"), -1); }

TEST(XPathSpine, CaseAndSpacingDoNotMatter) {
  EXPECT_EQ(spineOf("/body/docfragment[3]/body"), 2);
  EXPECT_EQ(spineOf("/body/DocFragment[ 7 ]/body"), 6);
}

TEST(XPathSpine, DocFragmentMayEndThePath) { EXPECT_EQ(spineOf("/body/DocFragment[12]"), 11); }

TEST(XPathSpine, MissingOrMalformedPredicateIsRejected) {
  EXPECT_EQ(spineOf(""), -1);
  EXPECT_EQ(spineOf("/body/p[1]"), -1);
  EXPECT_EQ(spineOf("/body/DocFragment[]/body"), -1);
  EXPECT_EQ(spineOf("/body/DocFragment[abc]/body"), -1);
  EXPECT_EQ(spineOf("/body/DocFragment[3/body"), -1);
}

// --- p[N] -> paragraph LUT index ---------------------------------------------------------------

TEST(XPathParagraph, BodyChildParagraphIsExtracted) {
  EXPECT_EQ(paragraphOf("/body/DocFragment[7]/body/p[685]/text().96"), 685);
  EXPECT_EQ(paragraphOf("/body/DocFragment[7]/body/p[3]/span[2]/text()[1].5"), 3);
}

TEST(XPathParagraph, ExplicitBodyIndexAndCaseAreTolerated) {
  EXPECT_EQ(paragraphOf("/body/DocFragment[7]/body[1]/p[3]"), 3);
  EXPECT_EQ(paragraphOf("/body/DocFragment[7]/body/P[3]"), 3);
}

TEST(XPathParagraph, BareParagraphMeansTheFirst) { EXPECT_EQ(paragraphOf("/body/DocFragment[7]/body/p"), 1); }

TEST(XPathParagraph, NestedParagraphIsNotABodyChild) {
  // The LUT counts direct children of <body>; p[4] inside a div is the 4th of the div's.
  EXPECT_EQ(paragraphOf("/body/DocFragment[7]/body/div[2]/p[4]"), -1);
  EXPECT_EQ(paragraphOf("/body/DocFragment[7]/body/section/p[30]/text().0"), -1);
}

TEST(XPathParagraph, NoParagraphNoIndex) {
  EXPECT_EQ(paragraphOf("/body/DocFragment[7]/body"), -1);
  EXPECT_EQ(paragraphOf("/body/DocFragment[7]/body/li[3]"), -1);
  EXPECT_EQ(paragraphOf("/body/DocFragment[7]/body/h1/text().0"), -1);
  EXPECT_EQ(paragraphOf(""), -1);
}

TEST(XPathParagraph, IndexStaysWithinTheLut) {
  EXPECT_EQ(paragraphOf("/body/DocFragment[7]/body/p[0]"), -1);
  EXPECT_EQ(paragraphOf("/body/DocFragment[7]/body/p[65535]"), 65535);
  EXPECT_EQ(paragraphOf("/body/DocFragment[7]/body/p[65536]"), -1);
}

// --- Normalisation the reverse mapper matches on ------------------------------------------------

using ChapterXPathIndexerInternal::isAncestorPath;
using ChapterXPathIndexerInternal::normalizeXPath;
using ChapterXPathIndexerInternal::removeIndices;

TEST(XPathNormalize, LowercasesAndAddsImplicitFirstIndices) {
  EXPECT_EQ(normalizeXPath("/body/DocFragment[2]/body/section/p[30]"),
            "/body[1]/docfragment[2]/body[1]/section[1]/p[30]");
}

TEST(XPathNormalize, StripsTheTextPointSuffix) {
  const std::string expected = "/body[1]/docfragment[2]/body[1]/p[3]";
  EXPECT_EQ(normalizeXPath("/body/DocFragment[2]/body/p[3]/text().0"), expected);
  EXPECT_EQ(normalizeXPath("/body/DocFragment[2]/body/p[3]/text()[2].17"), expected);
  EXPECT_EQ(normalizeXPath("/body/DocFragment[2]/body/p[3]/text()"), expected);
  EXPECT_EQ(normalizeXPath("/body/DocFragment[2]/body/p[3].5"), expected);
}

TEST(XPathNormalize, DropsWhitespaceAndTrailingSlashes) {
  EXPECT_EQ(normalizeXPath(" /body/DocFragment[2]/body/p[3] / "), "/body[1]/docfragment[2]/body[1]/p[3]");
}

TEST(XPathNormalize, EmptyStaysEmpty) { EXPECT_EQ(normalizeXPath(""), ""); }

TEST(XPathNormalize, RemoveIndicesKeepsOnlyTheTags) {
  EXPECT_EQ(removeIndices("/body[1]/docfragment[2]/body[1]/div[3]/p[30]"), "/body/docfragment/body/div/p");
}

TEST(XPathNormalize, AncestorMeansAProperPrefixOnASegmentBoundary) {
  EXPECT_TRUE(isAncestorPath("/body[1]/div[1]", "/body[1]/div[1]/p[2]"));
  EXPECT_FALSE(isAncestorPath("/body[1]/div[1]", "/body[1]/div[1]"));
  EXPECT_FALSE(isAncestorPath("/body[1]/div[1]", "/body[1]/div[12]/p[2]"));
}

// --- What a record says without opening the book --------------------------------------------------
//
// The reader compares a fetched record against where it is before deciding whether to inflate a
// chapter for it. That comparison needs the spine and the body-child paragraph, both of which
// are in the XPath string.

TEST(PeekRemote, ReadsSpineAndBodyChildParagraph) {
  const auto pos = ProgressMapper::peekRemote({"/body/DocFragment[1]/body/p[25]/text().0", ProgressPpb(300000)}, 3);
  EXPECT_TRUE(pos.hasResolvedSpineIndex);
  EXPECT_EQ(pos.spineIndex, 0);
  EXPECT_TRUE(pos.hasParagraphIndex);
  EXPECT_EQ(pos.paragraphIndex, 25);
}

TEST(PeekRemote, NestedParagraphGivesOnlyTheSpine) {
  const auto pos =
      ProgressMapper::peekRemote({"/body/DocFragment[2]/body/section/p[30]/text().0", ProgressPpb(300000)}, 3);
  EXPECT_TRUE(pos.hasResolvedSpineIndex);
  EXPECT_EQ(pos.spineIndex, 1);
  EXPECT_FALSE(pos.hasParagraphIndex);
}

TEST(PeekRemote, SpineOutOfRangeOrMissingIsNotTrusted) {
  EXPECT_FALSE(
      ProgressMapper::peekRemote({"/body/DocFragment[9]/body/p[1]", ProgressPpb(300000)}, 3).hasResolvedSpineIndex);
  EXPECT_FALSE(
      ProgressMapper::peekRemote({"/body/DocFragment[0]/body/p[1]", ProgressPpb(300000)}, 3).hasResolvedSpineIndex);
  EXPECT_FALSE(ProgressMapper::peekRemote({"", ProgressPpb(300000)}, 3).hasResolvedSpineIndex);
}

TEST(PeekRemote, UnknownSpineCountSkipsTheRangeCheck) {
  EXPECT_TRUE(
      ProgressMapper::peekRemote({"/body/DocFragment[9]/body/p[1]", ProgressPpb(300000)}, 0).hasResolvedSpineIndex);
}

}  // namespace
