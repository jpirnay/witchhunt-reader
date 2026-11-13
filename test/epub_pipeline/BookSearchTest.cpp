// Book search over a chapter's XHTML: the query folded as the text is, matches across inline markup and white space,
// whole words, entities, the text the layout never counts, the range a search keeps, the cap, and each hit's offset
// as the layout parser counts it (VisibleText.h: non-whitespace bytes from <body> on, outside head, script, style).
// The hits go out one by one in the order found, and the card's file gives them back by index.
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub/BookSearch.h"
#include "Epub/BookSearchHitFile.h"

namespace {

using BookSearch::ChapterSearch;
using BookSearch::Hit;
using BookSearch::Pattern;

struct Collected final : BookSearch::HitSink {
  std::vector<Hit> hits;
  void add(const Hit& hit) override { hits.push_back(hit); }
};

std::string page(const std::string& body) {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Fox "
         "tales</title><style>p.fox{}</style></head><body>" +
         body + "</body></html>";
}

struct Found {
  std::vector<Hit> hits;
  bool parsed = false;
};

Found search(const std::string& xhtml, const std::string& query, const bool wholeWords = false,
             const size_t capacity = 50, const uint32_t from = 0, const uint32_t until = UINT32_MAX,
             const size_t chunk = 7) {
  const Pattern pattern(query);
  Found found;
  Collected out;
  size_t count = 0;
  ChapterSearch chapter(pattern, wholeWords, out, capacity, count, 3, static_cast<uint32_t>(xhtml.size()), from, until);
  EXPECT_TRUE(chapter.begin());
  // In small pieces, as a stream delivers it: sequences and entities are cut between them.
  for (size_t at = 0; at < xhtml.size() && !*chapter.stopFlag(); at += chunk) {
    const std::string piece = xhtml.substr(at, chunk);
    chapter.sink().write(reinterpret_cast<const uint8_t*>(piece.data()), piece.size());
  }
  found.parsed = chapter.finish();
  EXPECT_EQ(count, out.hits.size()) << "every hit counted is handed on";
  found.hits = std::move(out.hits);
  return found;
}

std::string snippetOf(const Hit& hit) { return std::string(hit.snippet, hit.snippetLength); }

}  // namespace

TEST(BookSearchPattern, IsTheQueryFoldedAndItsSpacesCollapsed) {
  const Pattern pattern("  Große   Straße ");
  const std::string expected = "grosse strasse";
  ASSERT_EQ(expected.size(), pattern.length());
  for (uint8_t i = 0; i < pattern.length(); ++i) EXPECT_EQ(static_cast<uint32_t>(expected[i]), pattern.at(i));
  EXPECT_TRUE(Pattern("   ").empty());
  EXPECT_TRUE(Pattern("\xC2\xAD").empty()) << "a soft hyphen alone is nothing to look for";
}

TEST(BookSearch, FindsTheQueryAtTheOffsetTheLayoutCounts) {
  const Found found = search(page("<p>The quick brown fox.</p><p>Jumps over the lazy dog.</p>"), "fox");
  ASSERT_TRUE(found.parsed);
  ASSERT_EQ(1u, found.hits.size());
  EXPECT_EQ(3, found.hits[0].spine);
  EXPECT_EQ(13u, found.hits[0].offset) << "Thequickbrown: 13 visible bytes before it; the title and style not counted";
  EXPECT_NE(std::string::npos, snippetOf(found.hits[0]).find("quick brown fox. Jumps over"));
}

TEST(BookSearch, IgnoresCaseDiacriticsAndPunctuationStyles) {
  const std::string xhtml = page("<p>Le CAFÉ d’Élise — ouvert</p>");
  EXPECT_EQ(1u, search(xhtml, "cafe").hits.size());
  EXPECT_EQ(1u, search(xhtml, "d'elise - ouvert").hits.size());
  EXPECT_EQ(1u, search(page("<p>Die Straße</p>"), "STRASSE").hits.size());
}

TEST(BookSearch, MatchesAcrossInlineMarkupAndWhiteSpaceButNotAcrossWords) {
  const std::string xhtml = page("<p>a <i>very</i>\n   <b>old</b> tale, and an<span>other</span></p>");
  EXPECT_EQ(1u, search(xhtml, "very old tale").hits.size());
  EXPECT_EQ(1u, search(xhtml, "another").hits.size()) << "inline markup inside a word joins it";
  EXPECT_TRUE(search(page("<p>end</p><p>start</p>"), "endstart").hits.empty()) << "a block ends a word";
}

TEST(BookSearch, WholeWordsLeavesOutMatchesInsideLongerWords) {
  const std::string xhtml = page("<p>The theory of the other, then the end.</p>");
  EXPECT_EQ(6u, search(xhtml, "the").hits.size());
  EXPECT_EQ(3u, search(xhtml, "the", /*wholeWords=*/true).hits.size());
  EXPECT_EQ(1u, search(page("<p>the</p>"), "the", true).hits.size()) << "the chapter's end ends the word";
}

TEST(BookSearch, ReadsEntitiesAsTheLayoutDoesAndCountsTheirExpansion) {
  // &nbsp; is HTML's (the layout expands it, two bytes), &amp; XML's own (one).
  const Found found = search(page("<p>Tom&nbsp;&amp;&nbsp;Jerry</p>"), "tom & jerry");
  ASSERT_EQ(1u, found.hits.size());
  EXPECT_EQ(0u, found.hits[0].offset);
  EXPECT_EQ(8u, search(page("<p>Tom&nbsp;&amp;&nbsp;Jerry</p>"), "jerry").hits[0].offset);
}

TEST(BookSearch, LeavesOutTheTextOfHeadScriptAndStyle) {
  const Found found =
      search(page("<script>var fox = 1;</script><p>no f-o-x here</p><style>.fox{}</style><p>a fox</p>"), "fox");
  ASSERT_EQ(1u, found.hits.size());
  EXPECT_EQ(12u, found.hits[0].offset) << "nof-o-xhere, a: the script's and style's text are not counted";
}

TEST(BookSearch, KeepsOnlyTheHitsInItsRange) {
  const std::string xhtml = page("<p>one fox, two fox, three fox</p>");
  const std::vector<Hit> all = search(xhtml, "fox").hits;
  ASSERT_EQ(3u, all.size());
  const std::vector<Hit> tail = search(xhtml, "fox", false, 50, all[1].offset).hits;
  ASSERT_EQ(2u, tail.size());
  EXPECT_EQ(all[1].offset, tail[0].offset);
  const std::vector<Hit> head = search(xhtml, "fox", false, 50, 0, all[1].offset).hits;
  ASSERT_EQ(1u, head.size());
  EXPECT_EQ(all[0].offset, head[0].offset);
}

TEST(BookSearch, StopsWhenItsHitsAreFull) {
  const Found found = search(page("<p>fox fox fox fox</p><p>" + std::string(2000, 'x') + "</p>"), "fox", false, 2);
  ASSERT_TRUE(found.parsed);
  ASSERT_EQ(2u, found.hits.size());
  EXPECT_LT(found.hits[0].offset, found.hits[1].offset);
}

TEST(BookSearch, PlacesEachHitInItsChapterByItsBytes) {
  const std::string filler(2000, 'y');
  const Found found = search(page("<p>fox</p><p>" + filler + "</p><p>fox</p><p>" + filler + "</p><p>fox</p>"), "fox");
  ASSERT_EQ(3u, found.hits.size());
  EXPECT_LT(found.hits[0].chapterPermille, 100);
  EXPECT_GT(found.hits[1].chapterPermille, 400);
  EXPECT_LT(found.hits[1].chapterPermille, 600);
  EXPECT_GT(found.hits[2].chapterPermille, 900);
  EXPECT_LT(found.hits[2].chapterPermille, 1000);
}

TEST(BookSearch, HandsOnManyCloseHitsInTheOrderFound) {
  // More hits than snippets kept open at once, each wanting the text after it: they still go out in order, whole.
  std::string text;
  for (int i = 0; i < 40; ++i) text += "ox" + std::to_string(i % 10) + " ";
  const Found found = search(page("<p>" + text + "</p>"), "ox");
  ASSERT_EQ(40u, found.hits.size());
  for (size_t i = 1; i < found.hits.size(); ++i) EXPECT_LT(found.hits[i - 1].offset, found.hits[i].offset) << i;
  EXPECT_EQ(0u, snippetOf(found.hits[0]).find("ox0 ox1"));
}

TEST(BookSearchHitFile, GivesBackTheHitsByIndex) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("book_search_hits_" + std::to_string(static_cast<long>(getpid())) + ".bin");
  BookSearch::HitFile file;
  ASSERT_TRUE(file.create(path.string()));
  for (uint16_t i = 0; i < 40; ++i) {
    Hit hit;
    hit.spine = i;
    hit.chapterPermille = static_cast<uint16_t>(i * 10);
    hit.offset = 1000u + i;
    const std::string snippet = "hit " + std::to_string(i);
    hit.snippetLength = static_cast<uint8_t>(snippet.size());
    snippet.copy(hit.snippet, snippet.size());
    file.add(hit);
  }
  ASSERT_TRUE(file.close());
  EXPECT_EQ(40u, file.written());
  Hit window[16];
  ASSERT_EQ(16u, BookSearch::HitFile::read(path.string(), 16, window, 16));
  EXPECT_EQ(16, window[0].spine);
  EXPECT_EQ(160, window[0].chapterPermille);
  EXPECT_EQ(1031u, window[15].offset);
  EXPECT_EQ("hit 31", snippetOf(window[15]));
  EXPECT_EQ(8u, BookSearch::HitFile::read(path.string(), 32, window, 16)) << "the file ends after 40";
  EXPECT_EQ(0u, BookSearch::HitFile::read(path.string(), 40, window, 16));
  std::filesystem::remove(path);
  EXPECT_EQ(0u, BookSearch::HitFile::read(path.string(), 0, window, 16)) << "no file, nothing read";
}

TEST(BookSearch, CutsASnippetAtACharacterAndSurvivesMalformedText) {
  const Found found = search(page("<p>" + std::string(40, 'a') + " ééééééééééé fox ééééééééééééééééééééééééééééé</p>"),
                             "fox", false, 50, 0, UINT32_MAX, 3);
  ASSERT_EQ(1u, found.hits.size());
  const std::string snippet = snippetOf(found.hits[0]);
  EXPECT_LE(snippet.size(), Hit::kSnippetBytes);
  EXPECT_NE(std::string::npos, snippet.find("fox"));
  EXPECT_NE(0xC3, static_cast<unsigned char>(snippet.back())) << "no lead byte left without its continuation";
  // A broken sequence counts its bytes, as the layout's count does, and the text after it is still found.
  const Found broken = search(page("<p>ab\xC3 fox</p>"), "fox");
  ASSERT_EQ(1u, broken.hits.size());
  EXPECT_EQ(3u, broken.hits[0].offset);
}
