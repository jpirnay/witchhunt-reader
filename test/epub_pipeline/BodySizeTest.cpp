// The size of a book's running text (BodySize, for Normalize Font Size): the chapter parser styles the book's largest
// spine item without laying it out, and the size most of its characters are set at, headings left out, is the body
// size; it is kept in the book's cache and forgotten when the CSS is compiled again.
#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "BuildArena.h"
#include "Epub.h"
#include "Epub/BodySize.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"
#include "GfxRenderer.h"
#include "PipelineRunner.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

struct BodySizeTest : testing::Test {
  fs::path work;
  int books = 0;
  GfxRenderer renderer;
  static constexpr int kFontId = 1;  // the stub renderer's metrics are the same for every font

  void SetUp() override {
    work = fs::temp_directory_path() / ("body_size_" + std::to_string(getpid()) + "_" +
                                        testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  // One stylesheet and the chapters (their <body>s) in spine order.
  std::shared_ptr<Epub> book(const std::string& css, const std::vector<std::string>& bodies) {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile "
            "full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n");
    std::string manifest = "<item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n";
    std::string spine;
    for (size_t i = 0; i < bodies.size(); ++i) {
      const std::string id = "c" + std::to_string(i);
      manifest += "<item id=\"" + id + "\" href=\"" + id + ".xhtml\" media-type=\"application/xhtml+xml\"/>\n";
      spine += "<itemref idref=\"" + id + "\"/>";
      zip.add("OEBPS/" + id + ".xhtml",
              "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
              "<title>T</title><link rel=\"stylesheet\" href=\"style.css\" type=\"text/css\"/></head><body>" +
                  bodies[i] + "</body></html>\n");
    }
    zip.add("OEBPS/content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"uid\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
            "<dc:identifier id=\"uid\">body-size</dc:identifier><dc:title>Sizes</dc:title>"
            "<dc:language>en</dc:language></metadata><manifest>" +
                manifest + "</manifest><spine>" + spine + "</spine></package>\n");
    zip.add("OEBPS/style.css", css);
    const std::string path = (work / ("book" + std::to_string(books++) + ".epub")).string();
    zip.write(path);
    auto epub = std::make_shared<Epub>(path, (work / "cache").string());
    EXPECT_TRUE(epub->load(true));
    epub->setupCacheDir();
    return epub;
  }

  BodySize::Detected detect(const Epub& epub, BuildArena* scratch = nullptr) {
    return BodySize::detect(epub, renderer, kFontId, scratch);
  }

  // `n` paragraphs of `cls` holding `text`.
  static std::string paragraphs(const char* cls, const std::string& text, const int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += std::string("<p class=\"") + cls + "\">" + text + "</p>";
    return out;
  }
};

const std::string kProse =
    "The running text of a book is most of its characters: page after page of it, set at one size, with the "
    "headings, the notes and the captions around it a handful of words each.";

// Its characters, as the census counts them: the words', without the spaces between them.
size_t proseChars(const std::string& text) {
  size_t n = 0;
  for (const char c : text) n += c != ' ';
  return n;
}

TEST(BodySizeOf, TheSizeWithTheMostCharacters) {
  BodySize::Census census;
  census.add(89, 900);
  census.add(120, 300);
  census.add(75, 400);
  EXPECT_EQ(89, BodySize::bodySizeOf(census));
}

TEST(BodySizeOf, NearTheBodyIsTheBody) {
  for (const int pct : {90, 95, 100, 105, 110}) {
    BodySize::Census census;
    census.add(pct, 1000);
    EXPECT_EQ(100, BodySize::bodySizeOf(census)) << pct;
  }
  for (const int pct : {89, 111}) {
    BodySize::Census census;
    census.add(pct, 1000);
    EXPECT_EQ(pct, BodySize::bodySizeOf(census)) << pct;
  }
}

TEST(BodySizeOf, TooLittleTextDecidesNothing) {
  BodySize::Census census;
  census.add(80, BodySize::kMinChars - 1);
  EXPECT_EQ(100, BodySize::bodySizeOf(census));
  census.add(80, 1);
  EXPECT_EQ(80, BodySize::bodySizeOf(census));
}

TEST(BodySizeOf, ATieGoesToTheSmallerAndAWordsRangeBoundsIt) {
  BodySize::Census tie;
  tie.add(80, 500);
  tie.add(120, 500);
  EXPECT_EQ(80, BodySize::bodySizeOf(tie));
  BodySize::Census tiny;
  tiny.add(20, 1000);  // below the smallest size a word can take
  EXPECT_EQ(100, BodySize::bodySizeOf(tiny));
  BodySize::Census huge;
  huge.add(300, 1000);  // the top bucket: 255 % and up
  EXPECT_EQ(100, BodySize::bodySizeOf(huge));
}

// The corpus book: every paragraph `small` (8/9), the title page before it set far larger.
TEST_F(BodySizeTest, SmallRunningTextIsTheBodySize) {
  auto epub = std::make_shared<Epub>(CORPUS_DIR "/test_body_size.epub", (work / "cache").string());
  ASSERT_TRUE(epub->load(true));
  const auto start = std::chrono::steady_clock::now();
  const BodySize::Detected found = detect(*epub);
  const auto us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
  std::printf("census: %u chars, %u at %d %%, %zu bytes in %lld us\n", found.chars, found.bodyChars, found.topPct,
              found.bytesRead, static_cast<long long>(us));
  EXPECT_FALSE(found.failed);
  EXPECT_EQ(1, found.spineIndex) << "the largest item, not the title page";
  EXPECT_EQ(89, found.pct);
  EXPECT_EQ(89, found.topPct);
}

// A heading has more characters than the prose here; it is not running text.
TEST_F(BodySizeTest, HeadingsAreLeftOut) {
  std::string longHeading;
  for (int i = 0; i < 12; ++i) longHeading += "A Very Long Chapter Title That Goes On ";
  auto epub = book("h1 { font-size: large } .tx { font-size: small }",
                   {"<h1>" + longHeading + "</h1><h2>" + longHeading + "</h2>" + paragraphs("tx", kProse, 3)});
  const BodySize::Detected found = detect(*epub);
  EXPECT_FALSE(found.failed);
  EXPECT_EQ(89, found.pct);
  EXPECT_EQ(3 * proseChars(kProse), found.chars);
}

// Many short notes, fewer long paragraphs: characters decide, not paragraphs.
TEST_F(BodySizeTest, MostCharactersNotMostParagraphs) {
  const std::string css = ".tx { font-size: small } .xs { font-size: x-small }";
  auto prose = book(css, {paragraphs("xs", "Note.", 40) + paragraphs("tx", kProse, 4)});
  EXPECT_EQ(89, detect(*prose).pct);
  auto notes = book(css, {paragraphs("xs", kProse, 4) + paragraphs("tx", "Line.", 40)});
  EXPECT_EQ(75, detect(*notes).pct);
}

// The largest spine item is measured: a front matter item set otherwise does not count.
TEST_F(BodySizeTest, TheLargestItemIsMeasured) {
  auto epub = book(".tx { font-size: small } .big { font-size: x-large }",
                   {paragraphs("big", kProse, 2), paragraphs("tx", kProse, 8), paragraphs("big", "End.", 1)});
  const BodySize::Detected found = detect(*epub);
  EXPECT_EQ(1, found.spineIndex);
  EXPECT_EQ(89, found.pct);
}

// Plain prose, and prose within the band of it, is the reader's size already.
TEST_F(BodySizeTest, PlainOrNearBodyTextIs100) {
  auto plain = book("h1 { font-size: 2em } .note { font-size: x-small }",
                    {"<h1>Title</h1>" + paragraphs("x", kProse, 6) + paragraphs("note", "A note.", 3)});
  EXPECT_EQ(100, detect(*plain).pct);
  auto near = book(".tx { font-size: 0.95em }", {paragraphs("tx", kProse, 6)});
  EXPECT_EQ(100, detect(*near).pct);
}

// Inline sizes count: a body-relative one (a keyword) Normalize Font Size takes the body size out of, and an em span
// in a paragraph without a size of its own, which it cannot reach, as body text.
TEST_F(BodySizeTest, SpansCountAsNormalizationReachesThem) {
  auto keyword = book(".s { font-size: small }", {paragraphs("x", "<span class=\"s\">" + kProse + "</span>", 6)});
  EXPECT_EQ(89, detect(*keyword).pct);
  auto em = book(".s { font-size: 0.8em }", {paragraphs("x", "<span class=\"s\">" + kProse + "</span>", 6)});
  EXPECT_EQ(100, detect(*em).pct);
  // In a paragraph of its own size, an em span is that size's share of it: 89 % of 75 %.
  auto inSized = book(".tx { font-size: small } .s { font-size: 0.75em }",
                      {paragraphs("tx", "<span class=\"s\">" + kProse + "</span>", 6)});
  EXPECT_EQ(67, detect(*inSized).pct);
}

// Tables and images are left out of the census (nothing is laid out or read for them); the text around them counts.
TEST_F(BodySizeTest, TablesAndImagesAreLeftOut) {
  std::string table = "<table>";
  for (int i = 0; i < 20; ++i) table += "<tr><td class=\"big\">" + kProse + "</td><td>" + kProse + "</td></tr>";
  table += "</table>";
  auto epub = book(".tx { font-size: small } .big { font-size: x-large }",
                   {paragraphs("tx", kProse, 3) + "<p><img src=\"missing.png\" alt=\"a picture\"/></p>" + table});
  const BodySize::Detected found = detect(*epub);
  EXPECT_FALSE(found.failed);
  EXPECT_EQ(89, found.pct);
  EXPECT_EQ(3 * proseChars(kProse), found.chars);
}

// The lent region hosts the rules, the parser state and the read, and gets them all back; the answer is the same.
TEST_F(BodySizeTest, ALentRegionGivesTheSameAnswer) {
  auto epub = book(".tx { font-size: small }", {paragraphs("tx", kProse, 40)});
  BuildArena lent(48 * 1024);
  ASSERT_TRUE(lent.valid());
  const BodySize::Detected found = detect(*epub, &lent);
  EXPECT_FALSE(found.failed);
  EXPECT_EQ(89, found.pct);
  EXPECT_EQ(detect(*epub).chars, found.chars);
  EXPECT_EQ(0u, lent.used()) << "the census left something in the region";
  EXPECT_GT(lent.highWater(), 0u) << "the census did not use the region";
}

// Measured once and kept: prepare() writes it, read() gives it back, a CSS compiled again forgets it.
TEST_F(BodySizeTest, KeptUntilTheCssIsCompiledAgain) {
  auto epub = book(".tx { font-size: small }", {paragraphs("tx", kProse, 6)});
  const std::string cache = epub->getCachePath();
  EXPECT_EQ(0, BodySize::read(cache)) << "not measured yet";
  EXPECT_EQ(89, BodySize::prepare(*epub, renderer, kFontId, nullptr));
  EXPECT_EQ(89, BodySize::read(cache));
  // A reopened book reads it back.
  {
    Epub again(epub->getPath(), (work / "cache").string());
    ASSERT_TRUE(again.load(true));
    EXPECT_EQ(89, BodySize::read(again.getCachePath()));
  }
  // The CSS cache of another version (a firmware update) is compiled again at the next load, and the measurement goes.
  ASSERT_TRUE(fs::remove(cache + "/css_rules.cache"));
  Epub reopened(epub->getPath(), (work / "cache").string());
  ASSERT_TRUE(reopened.load(true));
  EXPECT_EQ(0, BodySize::read(cache));
  BodySize::forget(cache);  // nothing there: no harm
  EXPECT_EQ(89, BodySize::prepare(reopened, renderer, kFontId, nullptr));
  BodySize::forget(cache);
  EXPECT_EQ(0, BodySize::read(cache));
}

// Without its compiled CSS (the book's style off when it was opened) nothing is measured, and nothing is written.
TEST_F(BodySizeTest, NothingWithoutTheCompiledCss) {
  auto epub = book(".tx { font-size: small }", {paragraphs("tx", kProse, 6)});
  const std::string cache = epub->getCachePath();
  Epub unstyled(epub->getPath(), (work / "cache").string());
  ASSERT_TRUE(fs::remove(cache + "/css_rules.cache"));
  ASSERT_TRUE(unstyled.load(true, /*skipLoadingCss=*/true));
  EXPECT_EQ(0, BodySize::prepare(unstyled, renderer, kFontId, nullptr));
  EXPECT_FALSE(fs::exists(cache + "/bodysize.bin"));
}

// ---- Laid out relative to it (Normalize Font Size) ---------------------------------------------------------------

// The size each line of `epubPath`'s spine `spine` is set at, by the line's first word, laid out into `cache` as the
// reader does (the harness measures the body size as the reader's open does): the line's block size (no size ladder
// in the harness, so the multiplier is the whole size) times its first word's.
std::map<std::string, float> lineSizes(const std::string& epubPath, const fs::path& cache, const bool normalization,
                                       const int spine) {
  pipeline_harness::Profile profile;
  profile.fontSizeNormalization = normalization;
  std::map<std::string, float> sizes;
  EXPECT_TRUE(pipeline_harness::forEachPage(
      epubPath, cache.string(), profile, [&sizes, spine](const int at, const Section&, const Page& page, GfxRenderer&) {
        if (at != spine) return;
        for (const auto& element : page.elements) {
          if (element->getTag() != TAG_PageLine) continue;
          const TextBlock& line = *static_cast<const PageLine&>(*element).getBlock();
          if (line.wordCount() == 0) continue;
          sizes.emplace(line.wordText(0), line.getRenderStyle().fontSizeMultiplier * line.wordSizePct(0) / 100.0f);
        }
      }));
  return sizes;
}

const std::string kCorpusBook = CORPUS_DIR "/test_body_size.epub";

// Normalization on: the running text (small, 8/9) is the reader's size, and the title (large, 6/5), the subtitle
// (medium) and the small print (x-small, 3/4) keep their proportion to it: the CSS Fonts 4 scale over 89 %.
TEST_F(BodySizeTest, OnTheRunningTextIsTheReadersSize) {
  const auto sizes = lineSizes(kCorpusBook, work / "on", true, 1);
  EXPECT_EQ(89, BodySize::read(Epub::cachePathFor(kCorpusBook, (work / "on").string())));
  ASSERT_EQ(1u, sizes.count("Chapter"));
  EXPECT_NEAR(1.2f / 0.89f, sizes.at("Chapter"), 0.002f);  // 135 %
  EXPECT_NEAR(1.0f / 0.89f, sizes.at("In"), 0.002f);       // 112 %: outside the band, kept
  EXPECT_NEAR(0.75f / 0.89f, sizes.at("Small"), 0.002f);   // 84 %
  EXPECT_FLOAT_EQ(1.0f, sizes.at("It"));                   // the first paragraph's first line
  for (const auto& [word, size] : sizes) {
    if (word != "Chapter" && word != "In" && word != "Small") {
      EXPECT_FLOAT_EQ(1.0f, size) << word;
    }
  }
}

// Normalization off: the sizes as the book's CSS states them.
TEST_F(BodySizeTest, OffTheBooksSizesStand) {
  const auto sizes = lineSizes(kCorpusBook, work / "off", false, 1);
  ASSERT_EQ(1u, sizes.count("Chapter"));
  EXPECT_NEAR(1.2f, sizes.at("Chapter"), 0.001f);
  EXPECT_FLOAT_EQ(1.0f, sizes.at("In"));
  EXPECT_NEAR(0.75f, sizes.at("Small"), 0.001f);
  EXPECT_NEAR(8.0f / 9, sizes.at("It"), 0.001f);
}

// A book whose running text is the reader's size already is laid out as before: its body size is 100.
TEST_F(BodySizeTest, PlainBodyTextIsUnchanged) {
  auto epub = book(".note { font-size: x-small } h1 { font-size: large }",
                   {"<h1>Title</h1>" + paragraphs("x", kProse, 6) + paragraphs("note", "A note.", 2)});
  const auto sizes = lineSizes(epub->getPath(), work / "plain", true, 0);
  EXPECT_EQ(100, BodySize::read(Epub::cachePathFor(epub->getPath(), (work / "plain").string())));
  ASSERT_EQ(1u, sizes.count("Title"));
  EXPECT_NEAR(1.2f, sizes.at("Title"), 0.001f);
  EXPECT_NEAR(0.75f, sizes.at("A"), 0.001f);
  for (const auto& [word, size] : sizes) {
    if (word != "Title" && word != "A") {
      EXPECT_FLOAT_EQ(1.0f, size) << word;
    }
  }
}

}  // namespace
