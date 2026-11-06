// Upstream's rules for Indic words on the page: bionic reading does not split one (each half would shape on its
// own), and no drop cap is captured from Indic text (it draws codepoint by codepoint, which would split the
// cluster). Each case lays a one-chapter book out through the host pipeline and reads the first page back.
#include <gtest/gtest.h>

#include <algorithm>
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

struct Word {
  std::string text;
  bool bold;
};

struct FirstPage {
  std::vector<Word> words;  // every word of every line, in order
  int dropCaps = 0;
};

struct IndicRules : testing::Test {
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("indic_rules_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  FirstPage firstPage(const std::string& body, const bool bionic) {
    syntheticBook(work, 0, body);
    pipeline_harness::Profile profile;
    profile.viewportWidth = 1600;
    profile.bionicReadingEnabled = bionic;
    FirstPage result;
    bool seen = false;
    const bool ok = pipeline_harness::forEachPage(
        (work / "book0.epub").string(), (work / "pages0").string(), profile,
        [&](int, const Section&, const Page& page, GfxRenderer&) {
          if (seen) return;
          seen = true;
          for (const auto& element : page.elements) {
            if (element->getTag() != TAG_PageLine) continue;
            const TextBlock& block = *static_cast<const PageLine&>(*element).getBlock();
            // A drop cap is a line of its own, set larger than the text.
            if (block.getRenderStyle().fontSizeMultiplier > 1.0f) ++result.dropCaps;
            for (uint16_t i = 0; i < block.wordCount(); ++i) {
              result.words.push_back({block.wordText(i), (block.wordStyle(i) & EpdFontFamily::BOLD) != 0});
            }
          }
        });
    EXPECT_TRUE(ok);
    return result;
  }
};

// Each page's words and its footnotes' numbers, for a one-chapter book of `body`.
struct PageText {
  std::vector<std::string> words;
  std::vector<std::string> footnotes;
};

std::vector<PageText> pagesOf(const fs::path& work, const int index, const std::string& body) {
  syntheticBook(work, index, body);
  pipeline_harness::Profile profile;
  std::vector<PageText> pages;
  const std::string tag = std::to_string(index);
  const bool ok =
      pipeline_harness::forEachPage((work / ("book" + tag + ".epub")).string(), (work / ("pages" + tag)).string(),
                                    profile, [&](int, const Section&, const Page& page, GfxRenderer&) {
                                      PageText& text = pages.emplace_back();
                                      for (const auto& element : page.elements) {
                                        if (element->getTag() != TAG_PageLine) continue;
                                        const TextBlock& block = *static_cast<const PageLine&>(*element).getBlock();
                                        for (uint16_t i = 0; i < block.wordCount(); ++i)
                                          text.words.emplace_back(block.wordText(i));
                                      }
                                      for (const auto& footnote : page.footnotes)
                                        text.footnotes.emplace_back(footnote.number);
                                    });
  EXPECT_TRUE(ok);
  return pages;
}

const std::string kFiller =
    " of this opening paragraph wraps beside a drop cap, line after line, until the cap is passed and the text "
    "goes on for a good while longer so that two full lines are filled before the paragraph ends.";

}  // namespace

TEST_F(IndicRules, BionicReadingLeavesAShapedScriptWordWhole) {
  // A pure Indic word has no Latin span to bold; the third word does, and is the one bionic reading would cut.
  const FirstPage page = firstPage("<p>हिन्दी reading readingहिन्दी</p>", true);
  ASSERT_EQ(4u, page.words.size());
  EXPECT_EQ("हिन्दी", page.words[0].text);
  EXPECT_FALSE(page.words[0].bold);
  EXPECT_EQ("read", page.words[1].text);
  EXPECT_TRUE(page.words[1].bold);
  EXPECT_EQ("ing", page.words[2].text);
  EXPECT_FALSE(page.words[2].bold);
  EXPECT_EQ("readingहिन्दी", page.words[3].text);
  EXPECT_FALSE(page.words[3].bold);
}

TEST_F(IndicRules, NoDropCapIsCapturedIntoAnIndicLetter) {
  const FirstPage indic =
      firstPage("<p><span style=\"float: left; font-size: 3em\">हि</span>न्दी" + kFiller + "</p>", false);
  EXPECT_EQ(0, indic.dropCaps);
  ASSERT_FALSE(indic.words.empty());
  EXPECT_EQ("हिन्दी", indic.words[0].text);

  // The same markup over a Latin letter still takes its drop cap.
  fs::remove_all(work);
  fs::create_directories(work);
  const FirstPage latin =
      firstPage("<p><span style=\"float: left; font-size: 3em\">T</span>his" + kFiller + "</p>", false);
  EXPECT_EQ(1, latin.dropCaps);
}

TEST_F(IndicRules, AFootnoteAfterMergedWordsStaysOnItsAnchorsPage) {
  // A style change inside a syllable (हिन्द<b>ी</b>) cuts the parser's word in two, and layout merges the parts back
  // (deviation 7). A footnote's anchor is placed by the tokens the parser added, so a line counts a merged token as
  // its parts: the same text unsplit pages the same, footnotes included, and each footnote is on the page that draws
  // its anchor, the last line of a page as well.
  const auto body = [](const bool split) {
    std::string paragraph = "<p>";
    for (int i = 1; i <= 600; ++i) {
      paragraph += split ? "हिन्द<b>ी</b>" : "हिन्दी";
      if (i % 5 == 0) paragraph += " <a href=\"#f" + std::to_string(i) + "\">" + std::to_string(i) + "</a>";
      paragraph += " ";
    }
    return paragraph + "</p>";
  };
  const std::vector<PageText> whole = pagesOf(work, 0, body(false));
  const std::vector<PageText> split = pagesOf(work, 1, body(true));
  ASSERT_GT(whole.size(), 2u);
  ASSERT_EQ(whole.size(), split.size());
  for (size_t p = 0; p < whole.size(); ++p) {
    EXPECT_EQ(whole[p].words, split[p].words) << "page " << p;
    EXPECT_EQ(whole[p].footnotes, split[p].footnotes) << "page " << p;
    for (const std::string& number : split[p].footnotes) {
      EXPECT_NE(std::find(split[p].words.begin(), split[p].words.end(), number), split[p].words.end())
          << "footnote " << number << " on page " << p;
    }
  }
}
