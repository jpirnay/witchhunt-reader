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

// A guard, not a RED test: every joiner was dropped before, and these still are.
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
  for (int n = 80; n <= 92; ++n) {
    std::string filler;
    for (int i = 0; i < n; ++i) filler += "क";
    EXPECT_EQ(textOf("<p>" + filler + "्" + kZwj + "ष</p>"), filler + "्" + kZwj + "ष") << "n=" << n;
    EXPECT_EQ(textOf("<p>" + filler + "्" + kZwj + kZwj + "ष</p>"), filler + "्ष") << "n=" << n;
  }
}

TEST_F(Joiners, AJoinerAtAnInlineElementsEdgeIsKept) {
  EXPECT_EQ(textOf("<p>क्<b>" + kZwj + "ष</b></p>"), "क्" + kZwj + "ष");
  EXPECT_EQ(textOf("<p>क्" + kZwj + "<b>ष</b></p>"), "क्" + kZwj + "ष");
  // A second joiner across the edge still drops both.
  EXPECT_EQ(textOf("<p>क्" + kZwj + "<b>" + kZwj + "ष</b></p>"), "क्ष");
}
