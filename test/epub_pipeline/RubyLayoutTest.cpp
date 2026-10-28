// <ruby>, <rt> and <rp> in a chapter become ruby groups on the lines that hold their bases (spec 7.1): a
// <ruby> with one <rt> per base is mono ruby, one <rt> after several bases a group, <rp> is dropped. Markup
// that is not a well-formed ruby loses no text: it flows inline.
#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "PipelineRunner.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

using RubyList = std::vector<std::tuple<size_t, size_t, std::string>>;

struct LaidLine {
  std::vector<std::string> words;
  std::vector<int> x;
  RubyList ruby;
  RubyList emphasis;  // (first, count, mark as a decimal) of each emphasis run
  int y = 0;
  bool inTable = false;
};

struct RubyLayout : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_ruby_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  static LaidLine laid(const TextBlock& block, const int y, const bool inTable) {
    LaidLine line;
    line.y = y;
    line.inTable = inTable;
    for (uint16_t w = 0; w < block.wordCount(); ++w) {
      line.words.emplace_back(block.wordText(w));
      line.x.push_back(block.wordXpos(w));
    }
    for (uint16_t a = 0; a < block.annotationCount(); ++a) {
      const auto annotation = block.annotation(a);
      if (annotation.kind == TextBlock::AnnotationKind::Emphasis) {
        line.emphasis.emplace_back(annotation.firstWord, annotation.wordCount, std::to_string(annotation.mark));
        continue;
      }
      if (annotation.kind != TextBlock::AnnotationKind::Ruby) continue;
      line.ruby.emplace_back(annotation.firstWord, annotation.wordCount, std::string(annotation.text));
    }
    return line;
  }

  // Every line of the chapter, in page order; a table cell's lines come where the table stands.
  std::vector<LaidLine> lines(const std::string& body, const std::string& css = "") {
    const std::string doc =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
        "</title>" +
        std::string(css.empty() ? "" : "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>") +
        "</head><body>\n" + body + "\n</body></html>\n";
    auto epub = css.empty() ? syntheticBookFromDocument(work, books++, doc)
                            : syntheticBookFromDocument(work, books++, doc, {{"style.css", "text/css", css}});
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    std::vector<LaidLine> out;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      if (!page) continue;
      for (const auto& el : page->elements) {
        if (el->getTag() == TAG_PageLine) {
          const auto& pageLine = static_cast<const PageLine&>(*el);
          out.push_back(laid(*pageLine.getBlock(), pageLine.yPos + p * 100000, false));
        } else if (el->getTag() == TAG_PageTable) {
          for (const auto& row : static_cast<const PageTableFragment&>(*el).getRows()) {
            for (const auto& cell : row.cells) {
              for (const auto& block : cell.lines) out.push_back(laid(*block, 0, true));
            }
          }
        }
      }
    }
    return out;
  }

  static std::string joined(const std::vector<LaidLine>& lines) {
    std::string out;
    for (const auto& line : lines) {
      for (const auto& word : line.words) out += word;
    }
    return out;
  }
  static bool anyRuby(const std::vector<LaidLine>& lines) {
    for (const auto& line : lines) {
      if (!line.ruby.empty()) return true;
    }
    return false;
  }
  static std::string hanRun(const int count) {
    std::string out;
    for (int i = 0; i < count; ++i) out += "漢";
    return out;
  }
};

constexpr const char* kPrettyPrinted = R"(<p>
  <ruby>
    漢<rp>（</rp><rt>かん</rt><rp>）</rp>
    字<rp>（</rp><rt>じ</rt><rp>）</rp>
  </ruby>は<ruby>難<rt>むずか</rt></ruby>しいですが、<ruby>
    面<rt>おも</rt>
    白<rt>しろ</rt>
  </ruby>いです。
</p>)";

}  // namespace

TEST_F(RubyLayout, MonoRubyAnnotatesEachBaseAndDropsTheParentheses) {
  const auto l =
      lines("<p><ruby>漢<rp>（</rp><rt>かん</rt><rp>）</rp>字<rp>（</rp><rt>じ</rt><rp>）</rp></ruby>は</p>");
  ASSERT_EQ(l.size(), 1u);
  EXPECT_EQ(l[0].words, (std::vector<std::string>{"漢", "字", "は"}));
  EXPECT_EQ(l[0].ruby, (RubyList{{0, 1, "かん"}, {1, 1, "じ"}}));
}

TEST_F(RubyLayout, GroupRubySpansItsWholeBase) {
  const auto l = lines("<p><ruby><rb>吾輩</rb><rt> わが  はい </rt></ruby>は猫</p>");
  ASSERT_EQ(l.size(), 1u);
  EXPECT_EQ(l[0].words, (std::vector<std::string>{"吾", "輩", "は", "猫"}));
  EXPECT_EQ(l[0].ruby, (RubyList{{0, 2, "わが はい"}}));
}

TEST_F(RubyLayout, APrettyPrintedRubyLaysOutWithoutSpaces) {
  const auto l = lines(kPrettyPrinted);
  ASSERT_EQ(l.size(), 1u);
  EXPECT_EQ(l[0].words, (std::vector<std::string>{"漢", "字", "は", "難", "し", "い", "で", "す", "が", "、", "面",
                                                  "白", "い", "で", "す", "。"}));
  std::vector<std::string> texts;
  for (const auto& r : l[0].ruby) texts.push_back(std::get<2>(r));
  EXPECT_EQ(texts, (std::vector<std::string>{"かん", "じ", "むずか", "おも", "しろ"}));
  EXPECT_EQ(l[0].x[2], l[0].x[1] + 16) << "は must follow 字 with no space token and no gap";
}

TEST_F(RubyLayout, StrayRubyMarkupLosesNoText) {
  struct Case {
    const char* body;
    const char* text;
  };
  const Case cases[] = {
      {"<p>前<rt>まえ</rt>後</p>", "前まえ後"},
      {"<p><ruby>漢字</ruby>です</p>", "漢字です"},
      {"<p><ruby><rt>よみ</rt></ruby>本</p>", "よみ本"},
      {"<p><ruby>漢<rtc><rt>かん</rt></rtc></ruby></p>", "漢かん"},
  };
  for (const auto& c : cases) {
    const auto l = lines(c.body);
    EXPECT_EQ(joined(l), c.text) << c.body;
    EXPECT_FALSE(anyRuby(l)) << c.body;
  }
  // A stray reading of several words keeps its spaces; one after a base without a group glues to its text.
  const auto two = lines("<p>前<ruby><rt>two words</rt></ruby>本</p>");
  ASSERT_EQ(two.size(), 1u);
  EXPECT_EQ(two[0].words, (std::vector<std::string>{"前", "two", "words", "本"}));
  const auto reading = lines("<p>字<ruby><rt>a reading</rt></ruby>end</p>");
  ASSERT_EQ(reading.size(), 1u);
  ASSERT_GE(reading[0].words.size(), 3u);
  EXPECT_EQ(reading[0].words[0], "字");
  EXPECT_EQ(reading[0].words[1], "a");
  EXPECT_EQ(reading[0].words[2].substr(0, 7), "reading");
  // The words are the same either way; the spacing is what a mangled reading changes.
  EXPECT_EQ(two[0].x, lines("<p>前two words本</p>")[0].x);
  const auto readingPlain = lines("<p>字a readingend</p>");
  EXPECT_EQ(reading[0].x[2], readingPlain[0].x[2]);
  const auto br = lines("<p><ruby>字<br/>本<rt>a reading</rt></ruby>end</p>");
  const auto brPlain = lines("<p>字<br/>本a readingend</p>");
  ASSERT_EQ(br.size(), 2u);
  ASSERT_EQ(brPlain.size(), 2u);
  EXPECT_EQ(br[1].x[1], brPlain[1].x[1]);  // <rt> start: base glued to its reading
  EXPECT_EQ(br[1].x[2], brPlain[1].x[2]);
  // An <rt> with no base appends only to a group of its own <ruby>.
  const auto l = lines("<p>漢<ruby>字<rt>じ</rt></ruby><ruby><rt>よみ</rt></ruby></p>");
  EXPECT_EQ(joined(l), "漢字よみ");
  ASSERT_EQ(l.size(), 1u);
  EXPECT_EQ(l[0].ruby, (RubyList{{1, 1, "じ"}}));
}

TEST_F(RubyLayout, ATransparentReadingDoesNotHideTheTextAfterIt) {
  // The </rt> close must reset the text-skip scope its element opened, or the next deeper text is dropped.
  const auto l = lines(
      "<p>漢<ruby>字<rt style=\"color: transparent\">じ</rt></ruby>です。</p>"
      "<p><span><em>deep</em></span>tail</p>");
  EXPECT_EQ(joined(l), "漢字です。deeptail");
  EXPECT_FALSE(anyRuby(l));
}

TEST_F(RubyLayout, AReadingOver255BytesKeepsItsPrefixAndTheLine) {
  std::string reading;
  for (int i = 0; i < 100; ++i) reading += "あ";  // 300 bytes
  const auto l = lines("<p>漢字<ruby>猫<rt>" + reading + "</rt></ruby>です</p>");
  EXPECT_EQ(joined(l), "漢字猫です");  // the ruby is wide, so the paragraph may take several lines
  size_t groups = 0;
  for (const auto& line : l) {
    for (const auto& r : line.ruby) {
      ++groups;
      EXPECT_EQ(std::get<2>(r), reading.substr(0, 255));
    }
  }
  EXPECT_EQ(groups, 1u);
}

TEST_F(RubyLayout, ACapCutAfterASpaceLeavesNoTrailingSpace) {
  // 253 letters, a space, then a kana whose bytes the 255-byte cap cuts: the reading ends at the letters.
  const std::string letters(253, 'a');
  const auto l = lines("<p><ruby>漢<rt>" + letters + " あ</rt></ruby></p>");
  size_t groups = 0;
  for (const auto& line : l) {
    for (const auto& r : line.ruby) {
      ++groups;
      EXPECT_EQ(std::get<2>(r), letters);
    }
  }
  EXPECT_EQ(groups, 1u);
}

TEST_F(RubyLayout, AWhitespaceHeavyReadingKeepsItsTail) {
  std::string reading = "あ";
  for (int i = 0; i < 400; ++i) reading += "\n   ";
  reading += "いう";
  const auto l = lines("<p><ruby>漢<rt>" + reading + "</rt></ruby></p>");
  ASSERT_EQ(l.size(), 1u);
  EXPECT_EQ(l[0].ruby, (RubyList{{0, 1, "あ いう"}}));
}

TEST_F(RubyLayout, RubyInATableCellAnnotatesTheCellsWords) {
  const auto l = lines("<table><tr><td>前文<ruby>漢<rt>かん</rt></ruby></td></tr></table>");
  size_t withRuby = 0;
  for (const auto& line : l) {
    if (line.ruby.empty()) continue;
    ++withRuby;
    EXPECT_TRUE(line.inTable) << "a body line has ruby";
    EXPECT_EQ(line.ruby, (RubyList{{2, 1, "かん"}}));
  }
  EXPECT_EQ(withRuby, 1u);
  EXPECT_EQ(joined(l), "前文漢");
}

TEST_F(RubyLayout, ALongGroupIsKeptWholeAndAnOverlongOneFallsBackInline) {
  const auto kept = lines("<p>" + hanRun(90) + "<ruby>" + hanRun(10) + "<rt>ながいよみ</rt></ruby></p>");
  size_t groups = 0;
  for (const auto& line : kept) {
    for (const auto& r : line.ruby) {
      ++groups;
      EXPECT_EQ(std::get<1>(r), 10u);
      EXPECT_EQ(std::get<2>(r), "ながいよみ");
    }
  }
  EXPECT_EQ(groups, 1u);

  const auto inlined = lines("<p>" + hanRun(90) + "<ruby>" + hanRun(140) + "<rt>よみ</rt></ruby></p>");
  EXPECT_FALSE(anyRuby(inlined));
  EXPECT_EQ(joined(inlined), hanRun(90) + hanRun(140) + "よみ");
}

TEST_F(RubyLayout, AMultiWordBaseKeepsTheSpaceBetweenItsWords) {
  const auto plain = lines("<p>See New York now</p>");
  const auto l = lines("<p>See <ruby>New York<rt>ニューヨーク</rt></ruby> now</p>");
  ASSERT_EQ(l.size(), 1u);
  ASSERT_EQ(plain.size(), 1u);
  std::string text;
  for (const auto& w : l[0].words) text += w;
  EXPECT_NE(text.find("New York"), std::string::npos) << "the base reads New York, not NewYork: " << text;
  // The group spans the base's every token, the space included; "York" sits where it does without ruby.
  ASSERT_EQ(l[0].ruby.size(), 1u);
  const auto& [first, count, reading] = l[0].ruby[0];
  EXPECT_EQ(first, 1u);
  EXPECT_EQ(l[0].words[first], "New");
  EXPECT_EQ(l[0].words[first + count - 1], "York");
  EXPECT_EQ(l[0].x[first + count - 1], plain[0].x[2]);
}

TEST_F(RubyLayout, APrettyPrintedBaseGetsNoSpaceFromItsMarkup) {
  const auto l = lines(R"(<p>a<ruby>
  New York
  <rt>ニューヨーク</rt>
</ruby>b</p>)");
  ASSERT_EQ(l.size(), 1u);
  std::string text;
  for (const auto& w : l[0].words) text += w;
  EXPECT_EQ(text, "aNew Yorkb");
}

TEST_F(RubyLayout, ALongMonoRubyElementKeepsEveryReadingOverItsBase) {
  std::string pairs;
  for (int i = 0; i < 80; ++i) pairs += "字<rt>じ</rt>";
  const auto l = lines("<p>" + hanRun(90) + "<ruby>" + pairs + "</ruby></p>");
  size_t groups = 0;
  for (const auto& line : l) {
    groups += line.ruby.size();
    for (const auto& w : line.words) EXPECT_NE(w, "じ") << "a reading drawn inline";
  }
  EXPECT_EQ(groups, 80u);
}

TEST_F(RubyLayout, ReadingsPastTheSsoAreChargedToTheTableRowBudget) {
  // Each pair costs a word and a group (96 B), and the reading's own heap block (19 B) once it is charged: 12 KB is
  // passed at about 106 pairs with the blocks and 126 without them.
  auto tableLines = [&](const int pairs) {
    std::string cell;
    for (int i = 0; i < pairs; ++i) cell += "字<rt>おだいどころ</rt>";
    const auto l = lines("<table><tr><td><ruby>" + cell + "</ruby></td></tr></table>");
    size_t inTable = 0;
    for (const auto& line : l) inTable += line.inTable ? 1 : 0;
    return inTable;
  };
  EXPECT_GT(tableLines(90), 0u) << "a cell under the budget stays a table";
  EXPECT_EQ(tableLines(118), 0u) << "readings over 15 bytes count toward the row budget, as long words do";
}

TEST_F(RubyLayout, ARubyLineIsTallerByHalfAnAscender) {
  const auto l = lines("<p>前</p><p><ruby>漢<rt>かん</rt></ruby></p><p>後</p>");
  ASSERT_EQ(l.size(), 3u);
  EXPECT_EQ((l[2].y - l[1].y) - (l[1].y - l[0].y), 9);
}

TEST_F(RubyLayout, ATableRowWithRubyIsTallerByTheBand) {
  const std::string doc = R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>C</title></head><body>
<table><tr><td>前</td></tr><tr><td><ruby>漢<rt>かん</rt></ruby></td></tr></table>
</body></html>
)";
  auto epub = syntheticBookFromDocument(work, books++, doc);
  Section::BuildParams params;
  params.viewportWidth = 480;
  params.viewportHeight = 800;
  params.lineCompression = 1.0f;
  params.embeddedStyle = true;
  GfxRenderer renderer;
  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
  ASSERT_TRUE(section.loadSectionFile(params));
  section.currentPage = 0;
  const auto page = section.loadPageFromSectionFile();
  ASSERT_TRUE(page);
  bool found = false;
  for (const auto& el : page->elements) {
    if (el->getTag() != TAG_PageTable) continue;
    const auto& rows = static_cast<const PageTableFragment&>(*el).getRows();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[1].height - rows[0].height, 9);
    found = true;
  }
  EXPECT_TRUE(found);
}

TEST_F(RubyLayout, ARubyBookPagesTheSameWithAndWithoutALentArena) {
  auto dump = [&](const size_t lent, const char* name) {
    const fs::path cache = work / name;
    fs::create_directories(cache);
    pipeline_harness::Profile profile;
    profile.lentArenaBytes = lent;
    std::ostringstream out;
    EXPECT_TRUE(pipeline_harness::runAndDump((fs::path(CORPUS_DIR) / "test_cjk_ja_soseki.epub").string(),
                                             cache.string(), profile, out));
    return out.str();
  };
  const std::string plain = dump(0, "plain");
  const std::string lent = dump(52272, "lent");
  EXPECT_EQ(plain, lent);
  EXPECT_NE(plain.find(" PAGE 1 "), std::string::npos) << "more than one page expected";
}

TEST_F(RubyLayout, ACssClassMarksItsSpan) {
  const auto l =
      lines("<p>前<span class=\"bouten\">強調</span>後</p>", ".bouten { -epub-text-emphasis-style: sesame }");
  ASSERT_EQ(l.size(), 1u);
  EXPECT_EQ(l[0].words, (std::vector<std::string>{"前", "強", "調", "後"}));
  EXPECT_EQ(l[0].emphasis, (RubyList{{1, 2, "5"}}));
  EXPECT_TRUE(l[0].ruby.empty());
}

TEST_F(RubyLayout, AnInternalLinkKeepsItsOwnMark) {
  const auto l = lines("<p>前<a class=\"bouten\" href=\"#x\">強調</a>後</p><p id=\"x\">先</p>",
                       ".bouten { -epub-text-emphasis-style: sesame }");
  ASSERT_GE(l.size(), 1u);
  EXPECT_EQ(l[0].words, (std::vector<std::string>{"前", "強", "調", "後"}));
  EXPECT_EQ(l[0].emphasis, (RubyList{{1, 2, "5"}}));
}
