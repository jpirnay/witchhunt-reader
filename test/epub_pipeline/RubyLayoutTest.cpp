// <ruby>, <rt> and <rp> in a chapter become ruby groups on the lines that hold their bases (spec 7.1): a
// <ruby> with one <rt> per base is mono ruby, one <rt> after several bases a group, <rp> is dropped. Markup
// that is not a well-formed ruby loses no text: it flows inline.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

using RubyList = std::vector<std::tuple<size_t, size_t, std::string>>;

struct LaidLine {
  std::vector<std::string> words;
  std::vector<int> x;
  RubyList ruby;
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
      if (annotation.kind != TextBlock::AnnotationKind::Ruby) continue;
      line.ruby.emplace_back(annotation.firstWord, annotation.wordCount, std::string(annotation.text));
    }
    return line;
  }

  // Every line of the chapter, in page order; a table cell's lines come where the table stands.
  std::vector<LaidLine> lines(const std::string& body) {
    const std::string doc =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C"
        "</title></head><body>\n" +
        body + "\n</body></html>\n";
    auto epub = syntheticBookFromDocument(work, books++, doc);
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
  // An <rt> with no base appends only to a group of its own <ruby>.
  const auto l = lines("<p>漢<ruby>字<rt>じ</rt></ruby><ruby><rt>よみ</rt></ruby></p>");
  EXPECT_EQ(joined(l), "漢字よみ");
  ASSERT_EQ(l.size(), 1u);
  EXPECT_EQ(l[0].ruby, (RubyList{{1, 1, "じ"}}));
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
