// Deviation 8: the chapter parser keeps a lone ZWJ or ZWNJ after a letter of an Indic block (U+0900-0DFF), where the
// shaper reads it (a half form, a chillu, a cluster kept apart), and drops every other joiner with the rest of the
// default-ignorables. A second joiner drops both and the rest of the run. Each case reads the word texts of the pages
// the host pipeline lays out for a one-chapter book.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"
#include "PipelineRunner.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

const std::string kZwj = "\xE2\x80\x8D";
const std::string kZwnj = "\xE2\x80\x8C";

struct Joiners : testing::Test {
  fs::path work;
  int bookCount = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("joiner_parse_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // The text of every word the body lays out, in order.
  std::vector<std::string> wordsOf(const std::string& body) {
    const int index = bookCount++;
    syntheticBook(work, index, body);
    std::vector<std::string> words;
    // Wide enough that no long word is split at a line's end, which would add a hyphen to its first part.
    pipeline_harness::Profile profile;
    profile.viewportWidth = 1600;
    const bool ok = pipeline_harness::forEachPage((work / ("book" + std::to_string(index) + ".epub")).string(),
                                                  (work / ("pages" + std::to_string(index))).string(), profile,
                                                  [&](int, const Section&, const Page& page, GfxRenderer&) {
                                                    for (const auto& element : page.elements) {
                                                      if (element->getTag() != TAG_PageLine) continue;
                                                      const TextBlock& block =
                                                          *static_cast<const PageLine&>(*element).getBlock();
                                                      for (uint16_t i = 0; i < block.wordCount(); ++i)
                                                        words.push_back(block.wordText(i));
                                                    }
                                                  });
    EXPECT_TRUE(ok);
    return words;
  }

  // The words' texts run together: a long word is several glued tokens, and an inline element's text is one more.
  std::string textOf(const std::string& body) {
    std::string text;
    for (const std::string& word : wordsOf(body)) text += word;
    return text;
  }
};

using Words = std::vector<std::string>;

}  // namespace

TEST_F(Joiners, AHalfFormJoinerAfterAViramaIsKept) {
  EXPECT_EQ(wordsOf("<p>क्" + kZwj + "ष</p>"), Words{"क्" + kZwj + "ष"});
}

TEST_F(Joiners, AZwnjBetweenConsonantsIsKept) {
  EXPECT_EQ(wordsOf("<p>क" + kZwnj + "ष</p>"), Words{"क" + kZwnj + "ष"});
}

TEST_F(Joiners, AChilluJoinerAtAWordsEndIsKept) {
  EXPECT_EQ(wordsOf("<p>അവന്" + kZwj + " വന്നു</p>"), (Words{"അവന്" + kZwj, "വന്നു"}));
}

TEST_F(Joiners, AChilluJoinerAtABlocksEndIsKept) { EXPECT_EQ(wordsOf("<p>അവന്" + kZwj + "</p>"), Words{"അവന്" + kZwj}); }

// A chillu's joiner stays with its word when the word is flushed before the joiner settles: at a <br/>, at a block
// child, and at an inline element's end followed by a space or by the block's end.
TEST_F(Joiners, AChilluJoinerSurvivesTheFlushOfItsWord) {
  const Words verse = {"അവന്" + kZwj, "വന്നു"};
  EXPECT_EQ(wordsOf("<p>അവന്" + kZwj + "<br/>വന്നു</p>"), verse);
  EXPECT_EQ(wordsOf("<div>അവന്" + kZwj + "<p>വന്നു</p></div>"), verse);
  EXPECT_EQ(wordsOf("<p><b>അവന്" + kZwj + "</b> വന്നു</p>"), verse);
  EXPECT_EQ(wordsOf("<p><b>അവന്" + kZwj + "</b></p>"), Words{"അവന്" + kZwj});
}

// A stray lead byte at a text's end is that text's, not the next text's.
TEST_F(Joiners, AMalformedTailStaysInItsOwnText) {
  EXPECT_EQ(wordsOf("<p>voil\xE0</p><p>Il</p>"), (Words{"voil\xE0", "Il"}));
  EXPECT_EQ(wordsOf("<p>voil\xE2\x80</p><p>Il</p>"), (Words{"voil\xE2\x80", "Il"}));
}

// A joiner and the letter after it are never parted by the word buffer's limit.
TEST_F(Joiners, AJoinerStaysWithTheLetterAfterItAtTheBufferLimit) {
  // 2 + 65 * 3 = 197 bytes in the word buffer (limit 200) when the joiner comes; the joiner and ष need 6 more.
  std::string word = "ab";
  for (int i = 0; i < 65; ++i) word += "क";
  // The parser's cut shows in a paragraph that needs bidi, which merges no Indic tokens (it shapes nothing: the line
  // pass's pieces are not cut at runs); one that shapes merges the two back into the word they were cut from
  // (deviation 7).
  const Words words = wordsOf("<p dir=\"rtl\">" + word + kZwj + "ष</p>");
  ASSERT_GE(words.size(), 2u);
  EXPECT_EQ(words.back(), kZwj + "ष");
  std::string all;
  for (const std::string& w : words) all += w;
  EXPECT_EQ(all, word + kZwj + "ष");
  EXPECT_EQ(wordsOf("<p>" + word + kZwj + "ष</p>"), Words{word + kZwj + "ष"});
}

// A guard, not a RED test: this pins that a joiner after anything but a letter of the shaped blocks is dropped.
TEST_F(Joiners, AJoinerAfterAnythingElseIsDropped) {
  EXPECT_EQ(wordsOf("<p>a" + kZwj + "b</p>"), Words{"ab"});
  EXPECT_EQ(wordsOf("<p>1" + kZwj + "</p>"), Words{"1"});
  EXPECT_EQ(wordsOf("<p>x " + kZwj + "y</p>"), (Words{"x", "y"}));
  EXPECT_EQ(wordsOf("<p>क् " + kZwnj + "ष</p>"), (Words{"क्", "ष"}));
}

TEST_F(Joiners, ARunOfJoinersIsDroppedWhole) {
  EXPECT_EQ(wordsOf("<p>क" + kZwj + kZwj + "ष</p>"), Words{"कष"});
  std::string run;
  for (int i = 0; i < 300; ++i) run += kZwnj;
  EXPECT_EQ(wordsOf("<p>क" + run + "ष</p>"), Words{"कष"});
}

// The SAX parser flushes its text every 256 bytes, so the filler puts the cut before, inside and after the virama and
// the joiner (as TextNodeRules.ACodepointSplitAtTheChunkBoundaryStillLandsExactly sweeps it).
TEST_F(Joiners, AJoinerSplitFromItsLetterByAChunkBoundaryIsKept) {
  // The ASCII prefix of 0 to 2 bytes moves the cut through every byte of the 3-byte sequences, the 1- and 2-byte
  // tails included.
  for (int prefixLen = 0; prefixLen <= 2; ++prefixLen) {
    const std::string prefix(static_cast<size_t>(prefixLen), 'a');
    for (int n = 80; n <= 92; ++n) {
      std::string filler = prefix;
      for (int i = 0; i < n; ++i) filler += "क";
      EXPECT_EQ(textOf("<p>" + filler + "्" + kZwj + "ष</p>"), filler + "्" + kZwj + "ष")
          << "prefix=" << prefixLen << " n=" << n;
      EXPECT_EQ(textOf("<p>" + filler + "्" + kZwj + kZwj + "ष</p>"), filler + "्ष")
          << "prefix=" << prefixLen << " n=" << n;
    }
  }
}

TEST_F(Joiners, AJoinerAtAnInlineElementsEdgeIsKept) {
  // The joiner waits across the edge and opens the next token, ahead of the letter it precedes. The parser's tokens
  // show in a paragraph that needs bidi, which merges no Indic tokens (it shapes nothing: the line pass's pieces are
  // not cut at runs).
  const Words split = {"क्", kZwj + "ष"};
  EXPECT_EQ(wordsOf("<p dir=\"rtl\">क्<b>" + kZwj + "ष</b></p>"), split);
  EXPECT_EQ(wordsOf("<p dir=\"rtl\">क्" + kZwj + "<b>ष</b></p>"), split);
  // A second joiner across the edge still drops both.
  EXPECT_EQ(wordsOf("<p dir=\"rtl\">क्" + kZwj + "<b>" + kZwj + "ष</b></p>"), (Words{"क्", "ष"}));
  // A paragraph that shapes merges the two (deviation 7: the glue falls inside a syllable), the joiner in its place.
  EXPECT_EQ(wordsOf("<p>क्<b>" + kZwj + "ष</b></p>"), Words{"क्" + kZwj + "ष"});
  EXPECT_EQ(wordsOf("<p>क्" + kZwj + "<b>" + kZwj + "ष</b></p>"), Words{"क्ष"});
}

// Deviation 8, Arabic: Persian writes its half-space as ZWNJ, so a joiner after a letter of the Arabic blocks is kept
// too. The Arabic strings are escapes.
TEST_F(Joiners, APersianZwnjIsKept) {
  const std::string word = "\u06A9" + kZwnj + "\u0647";  // keh, ZWNJ, heh
  EXPECT_EQ(wordsOf("<p>" + word + "</p>"), Words{word});
  // An RTL line lists its words in visual order.
  EXPECT_EQ(wordsOf("<p dir=\"rtl\">" + word + " \u0628</p>"), (Words{"\u0628", word}));
}

TEST_F(Joiners, AZwnjAfterAnArabicLetterAcrossAnElementEdgeIsKept) {
  // The joiner opens the token after the edge; the two tokens of an RTL line are listed in visual order.
  const std::string both = kZwnj + "\u0647" + "\u06A9";
  EXPECT_EQ(textOf("<p>\u06A9<b>" + kZwnj + "\u0647</b></p>"), both);
  EXPECT_EQ(textOf("<p>\u06A9" + kZwnj + "<b>\u0647</b></p>"), both);
  // After a space it is still dropped.
  EXPECT_EQ(wordsOf("<p>\u06A9 " + kZwnj + "\u0647</p>"), (Words{"\u0647", "\u06A9"}));
}

TEST_F(Joiners, ARunOfTwoJoinersIsDropped) {
  EXPECT_EQ(wordsOf("<p>\u06A9" + kZwnj + kZwj + "\u0647</p>"), Words{"\u06A9\u0647"});
  EXPECT_EQ(wordsOf("<p>\u06A9" + kZwnj + kZwnj + kZwnj + "\u0647</p>"), Words{"\u06A9\u0647"});
}

// Deviation 8 says a letter or a mark: a joiner after an Arabic digit, the Arabic comma or the full stop U+06D4 is
// dropped like one after any other non-letter.
TEST_F(Joiners, AJoinerAfterAnArabicNonLetterIsDropped) {
  for (const char* sign : {"\u0661", "\u060C", "\u06D4", "\u061B"}) {
    EXPECT_EQ(wordsOf(std::string("<p>") + sign + kZwnj + "\u0628</p>"), Words{std::string(sign) + "\u0628"}) << sign;
    EXPECT_EQ(wordsOf(std::string("<p>") + sign + kZwj + "\u0628</p>"), Words{std::string(sign) + "\u0628"}) << sign;
  }
}
