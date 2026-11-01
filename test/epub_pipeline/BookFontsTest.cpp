// A book's embedded body font (BookFonts, the Publisher Fonts setting): which family the body text is set
// in, and the font files copied out of the EPUB as plain TrueType, obfuscation undone.
//
// The body-font cases are ported from crosspoint-reader PR #3934's PublisherBodyFontTest (Julia Nguyen),
// asked of the whole book here instead of one chapter's layout.
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "BuildArena.h"
#include "Epub.h"
#include "Epub/BookFonts.h"
#include "Epub/css/CssParser.h"
#include "SaxParser/SaxParser.h"
#include "StoredZipWriter.h"
#include "ZipFile.h"

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string hex(const uint8_t* bytes, const size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    out += digits[bytes[i] >> 4];
    out += digits[bytes[i] & 15];
  }
  return out;
}

const std::string kTrueType = readFile(FONT_FIXTURE_DIR "/Ubuntu/Ubuntu-Regular.ttf");
const std::string kTrueTypeBold = readFile(FONT_FIXTURE_DIR "/Ubuntu/Ubuntu-Bold.ttf");
const std::string kCff = readFile(FONT_FIXTURE_DIR "/OpenDyslexic/OpenDyslexic-Regular.otf");

std::string obfuscated(std::string font, const uint8_t* key, const size_t keyLen, const size_t count) {
  for (size_t i = 0; i < count && i < font.size(); ++i) font[i] = static_cast<char>(font[i] ^ key[i % keyLen]);
  return font;
}

struct BookFontsTest : testing::Test {
  fs::path work;
  int books = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("book_fonts_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  struct File {
    std::string href;
    std::string mediaType;
    std::string bytes;
  };

  static constexpr const char* kIdentifiers = "<dc:identifier id=\"uid\">urn:isbn:978-0-00-000000-2</dc:identifier>";

  // One stylesheet, chapters in spine order (kMissing: listed, but not in the archive), any extra files;
  // identifiers as the OPF should carry them, and its guide.
  std::shared_ptr<Epub> book(const std::string& css, const std::vector<std::string>& chapters,
                             const std::vector<File>& extra = {}, const std::string& identifiers = kIdentifiers,
                             const std::string& guide = {}) {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile "
            "full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n");
    std::string manifest = "<item id=\"css\" href=\"Styles/style.css\" media-type=\"text/css\"/>\n";
    std::string spine;
    for (size_t i = 0; i < chapters.size(); ++i) {
      const std::string id = "c" + std::to_string(i);
      manifest += "<item id=\"" + id + "\" href=\"Text/" + id + ".xhtml\" media-type=\"application/xhtml+xml\"/>\n";
      spine += "<itemref idref=\"" + id + "\"/>";
      if (chapters[i] == kMissing) continue;
      zip.add("OEBPS/Text/" + id + ".xhtml",
              "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
              "<title>T</title><link rel=\"stylesheet\" href=\"../Styles/style.css\" type=\"text/css\"/></head>" +
                  chapters[i] + "</html>\n");
    }
    for (size_t i = 0; i < extra.size(); ++i) {
      manifest += "<item id=\"x" + std::to_string(i) + "\" href=\"" + extra[i].href.substr(6) + "\" media-type=\"" +
                  extra[i].mediaType + "\"/>\n";
      zip.add(extra[i].href, extra[i].bytes);
    }
    zip.add("OEBPS/content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"uid\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">" +
                identifiers + "<dc:title>Fonts</dc:title><dc:language>en</dc:language></metadata><manifest>" +
                manifest + "</manifest><spine>" + spine + "</spine>" + guide + "</package>\n");
    zip.add("OEBPS/Styles/style.css", css);
    const std::string path = (work / ("book" + std::to_string(books++) + ".epub")).string();
    zip.write(path);
    auto epub = std::make_shared<Epub>(path, (work / "cache").string());
    EXPECT_TRUE(epub->load(true));
    epub->setupCacheDir();
    return epub;
  }

  static constexpr const char* kMissing = "(missing)";
  static uint32_t family(const char* name) { return CssParser::fontFamilyHash(name); }

  // `@font-face` rules for `families`, each with a face of its own name in the book (faceFiles), all one file.
  static std::string faceRules(const std::vector<std::string>& families) {
    std::string css;
    for (const std::string& name : families) {
      css += "@font-face { font-family: " + name + "; src: url(../Fonts/" + name + ".ttf); }\n";
    }
    return css;
  }
  static std::vector<File> faceFiles(const std::vector<std::string>& families) {
    std::vector<File> files;
    for (const std::string& name : families) files.push_back({"OEBPS/Fonts/" + name + ".ttf", "font/ttf", kTrueType});
    return files;
  }
  // The families found, with the book's face records as prepare() has them.
  static BookFonts::Found ranked(const Epub& epub) {
    std::string records;
    EXPECT_TRUE(CssParser::loadFontFaces(epub.getCachePath(), records));
    return found(epub, records);
  }

  // The body family's identity, as Stage A compared it.
  static uint32_t detect(const Epub& epub) { return CssFontFamily::identity(found(epub).body); }
  static BookFonts::Found found(const Epub& epub, const std::string& records = {}) {
    bool failed = true;
    const BookFonts::Found found = BookFonts::detectFamilies(epub, nullptr, &failed, records);
    EXPECT_FALSE(failed);
    return found;
  }
};

const char* kProbeCss =
    "@font-face { font-family: EBGaramond; src: url(../Fonts/g.ttf); }\n"
    ".indent, .noindent { font-family: EBGaramond, serif; }\n"
    ".date, h1 { font-family: YatraOne, sans-serif; }\n"
    ".block { font-family: Roboto, sans-serif; }\n";

// A chapter body of `<p class='cls'>` paragraphs, at least `bytes` of markup.
std::string prose(const char* cls, const size_t bytes) {
  std::string body = "<body>";
  while (body.size() < bytes) {
    body += std::string("<p class='") + cls + "'>Narrative prose, the kind a chapter is made of.</p>";
  }
  return body + "</body>";
}

}  // namespace

TEST(BookFontsWeights, AddWeightedRoundsAndSaturates) {
  EXPECT_EQ(200u, BookFonts::addWeighted(0, 100, 512));
  EXPECT_EQ(5u, BookFonts::addWeighted(0, 3, 384));  // 4.5 rounds up
  EXPECT_EQ(UINT32_MAX, BookFonts::addWeighted(UINT32_MAX - 10, 100, 256));
  EXPECT_EQ(UINT32_MAX, BookFonts::addWeighted(0, 1u << 30, 1u << 20));
}

TEST(BookFontsWeights, AWeightScalesAnItemReadInPartAndASubsampledSpine) {
  EXPECT_EQ(6400u, BookFonts::probeWeight256(200 * 1024, 8 * 1024, 1, 1));
  EXPECT_EQ(256u, BookFonts::probeWeight256(0, 8 * 1024, 1, 1));  // size unknown: as read
  EXPECT_EQ(512u, BookFonts::probeWeight256(4096, 4096, 128, 64));
}

TEST(BookFontsKeys, TheIdpfKeyHashesTheIdentifierWithoutWhiteSpace) {
  uint8_t key[20];
  ASSERT_TRUE(BookFonts::idpfKey(" a b\tc\r\n", key));
  EXPECT_EQ("a9993e364706816aba3e25717850c26c9cd0d89d", hex(key, 20));  // SHA-1("abc")
  EXPECT_FALSE(BookFonts::idpfKey(" \n", key));
}

TEST(BookFontsKeys, TheAdobeKeyIsTheUuidsSixteenBytes) {
  uint8_t key[16];
  ASSERT_TRUE(BookFonts::adobeKey("URN:UUID:01234567-89ab-cdef-0123-456789ABCDEF", key));
  EXPECT_EQ("0123456789abcdef0123456789abcdef", hex(key, 16));
  EXPECT_FALSE(BookFonts::adobeKey("urn:isbn:9780000000002", key));
  EXPECT_FALSE(BookFonts::adobeKey("urn:uuid:0123", key));
  EXPECT_FALSE(BookFonts::adobeKey("urn:uuid:01234567-89ab-cdef-0123-456789abcdefaa", key));
}

TEST_F(BookFontsTest, ParagraphBodyFontDoesNotComeFromALeadingDateOrHeading) {
  const auto epub =
      book(kProbeCss, {"<body><h1>Heading heading heading</h1><p class='date'>A long date line, a long date</p>"
                       "<p class='noindent'>Ordinary body paragraph with enough narrative text.</p>"
                       "<p class='indent'>More narrative with an <span class='block'>inline</span> accent.</p>"
                       "<p class='block'>Quotation</p></body>"});
  EXPECT_EQ(family("EBGaramond"), detect(*epub));
}

TEST_F(BookFontsTest, TheFamilyWithTheMostTextWinsOverTheBodysOwn) {
  // The body names one family, the paragraphs set most of the text in another: the text's family is the body
  // font. (A book's sheets are merged, so a `body` rule can come from a page no chapter is styled by.)
  const auto epub =
      book(kProbeCss, {"<body style='font-family: EBGaramond'><p>Body</p>"
                       "<p class='block'>A much longer paragraph in an intentionally different font.</p></body>"});
  EXPECT_EQ(family("Roboto"), detect(*epub));
}

TEST_F(BookFontsTest, TheBodysFamilyCountsForTheTextThatInheritsIt) {
  const auto epub = book(kProbeCss, {"<body class='indent'><h1 class='date'>Title</h1><p>Narrative prose that "
                                     "inherits the body's family.</p><p class='block'>Short aside</p></body>"});
  EXPECT_EQ(family("EBGaramond"), detect(*epub));
}

TEST_F(BookFontsTest, AnInlineAccentDoesNotChooseTheParagraphFamily) {
  const auto epub = book(
      kProbeCss, {"<body><p class='indent'><span class='block'>A much longer inline accent</span> Body</p></body>"});
  EXPECT_EQ(family("EBGaramond"), detect(*epub));
}

TEST_F(BookFontsTest, ReaderFontParagraphsGiveNoFamily) {
  const auto epub =
      book(kProbeCss, {"<body><p>Ordinary prose stays in the selected reader font without an embedded family.</p>"
                       "<p class='block'>Quotation</p></body>"});
  EXPECT_EQ(0u, detect(*epub));
}

TEST_F(BookFontsTest, DivParagraphsCountAndTheCountRunsAcrossChapters) {
  const auto epub = book(kProbeCss, {"<body><div class='block'>Short</div></body>",
                                     "<body><div class='noindent'>Much longer narrative in the second chapter, which "
                                     "is where the book's body text starts.</div></body>"});
  EXPECT_EQ(family("EBGaramond"), detect(*epub));
}

TEST_F(BookFontsTest, OnlyTheHeadOfAChapterIsRead) {
  // One item: the probe reads its first 64 KB (kProbeMaxItemBytes).
  std::string chapter = "<body><p class='block'>First ";
  for (size_t i = 0; i < 12000; ++i) chapter += "prose ";  // ~72 KB
  chapter += "</p>";
  // ~100 KB more, in another family: it would win if the probe read on.
  for (int i = 0; i < 3000; ++i) chapter += "<p class='indent'>Late text the probe never reaches.</p>";
  chapter += "</body>";
  const auto epub = book(kProbeCss, {chapter});
  EXPECT_EQ(family("Roboto"), detect(*epub));
}

TEST_F(BookFontsTest, AnItemIsReadNoFurtherThanItsCap) {
  // Inside a long text run or attribute value the parser reports no progress: the read itself stops at the cap.
  std::string text = "<body><p class='block'>";
  for (int i = 0; i < 40000; ++i) text += "prose ";  // ~240 KB, one text node
  text += "</p></body>";
  const std::string attribute =
      "<body><img src='data:image/png;base64," + std::string(240 * 1024, 'A') + "'/><p class='indent'>Text.</p></body>";
  for (const std::string& chapter : {text, attribute}) {
    const auto epub = book(kProbeCss, {chapter});
    EXPECT_LE(found(*epub).bytesRead, BookFonts::kProbeMaxItemBytes);
  }
}

TEST_F(BookFontsTest, EachKindOfElementIsResolvedOnce) {
  // A book is the same few elements over and over: the probe resolves an element's CSS once per tag, class and id,
  // as the chapter parser does, not once per element (each a lookup that can read the card).
  std::string chapter = "<body>";
  for (int i = 0; i < 300; ++i) chapter += "<p class='indent'>Prose.</p><p class='block'>More.</p>";
  const auto epub = book(kProbeCss, {chapter + "</body>"});
  EXPECT_EQ(family("EBGaramond"), detect(*epub));
  EXPECT_LE(epub->getCssParser()->getResolveStats().resolveCalls, 10u);  // html, head, title, link, body, two p's
}

TEST_F(BookFontsTest, TheProbeResolvesAsLeanAsASectionBuild) {
  // Its memo stands in for the resolver's hot rule cache, as the chapter parser's does: heap between the lean floor
  // (24 KB) and the full one (40 KB), as an X3's first open has it, is enough to decide.
  const auto epub = book(kProbeCss, {prose("indent", 2 * 1024)});
  struct Heap {
    Heap() { ESP.setFreeHeap(32 * 1024); }
    ~Heap() { ESP.setFreeHeap(200 * 1024); }
  } heap;
  EXPECT_EQ(family("EBGaramond"), detect(*epub));  // found() expects no failure
}

TEST_F(BookFontsTest, TheBodyIsFoundPastTheSixthSpineItem) {
  std::vector<std::string> chapters(7, "<body><p class='block'>Front matter.</p></body>");
  chapters.push_back(prose("indent", 4 * 1024));
  const auto epub = book(kProbeCss, chapters);
  EXPECT_EQ(family("EBGaramond"), detect(*epub));
}

TEST_F(BookFontsTest, ALargeChapterReadInPartOutweighsSmallItemsReadWhole) {
  // 60 items: each is read up to ~8.5 KB. The two long chapters' heads stand for 300 KB each.
  std::vector<std::string> chapters(58, prose("block", 8 * 1024));
  chapters.push_back(prose("indent", 300 * 1024));
  chapters.push_back(prose("indent", 300 * 1024));
  const auto epub = book(kProbeCss, chapters);
  EXPECT_EQ(family("EBGaramond"), detect(*epub));
}

TEST_F(BookFontsTest, ALongSpineIsSampledWithinTheBudget) {
  const auto epub = book(kProbeCss, std::vector<std::string>(100, prose("indent", 20 * 1024)));
  const size_t walks = zipScanStats().walks;
  const BookFonts::Found families = found(*epub);
  EXPECT_LE(zipScanStats().walks - walks, 2u);  // read through the spine's stats, not looked up by name
  EXPECT_EQ(64, families.itemsRead);
  EXPECT_LE(families.bytesRead, 64u * (8 * 1024 + 4 * 1024));
  EXPECT_EQ(family("EBGaramond"), CssFontFamily::identity(families.body));
}

TEST_F(BookFontsTest, AnUnreadableItemIsSkipped) {
  const std::string chapter = prose("indent", 2 * 1024);
  const auto epub = book(kProbeCss, {chapter, kMissing, chapter}, {{"OEBPS/Fonts/g.ttf", "font/ttf", kTrueType}});
  EXPECT_EQ(family("EBGaramond"), detect(*epub));  // found() expects no failure
  EXPECT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
}

TEST_F(BookFontsTest, TheTextReferenceDoesNotHideEarlierItems) {
  const std::string chapter = prose("indent", 4 * 1024);
  const auto epub = book(kProbeCss, {chapter, chapter, "<body><p class='block'>A short page.</p></body>"}, {},
                         kIdentifiers, "<guide><reference type='text' title='Start' href='Text/c2.xhtml'/></guide>");
  ASSERT_EQ(2, epub->getSpineIndexForTextReference());
  EXPECT_EQ(family("EBGaramond"), detect(*epub));
}

TEST_F(BookFontsTest, ABinarySpineItemIsNoText) {
  std::string binary = "<body>";
  for (int i = 0; i < 2048; ++i) binary += static_cast<char>(i & 0xFF);
  binary += "</body>";
  const auto epub = book(kProbeCss, {binary, prose("indent", 2 * 1024)});
  EXPECT_EQ(family("EBGaramond"), detect(*epub));  // found() expects no failure
}

TEST_F(BookFontsTest, AnEmbeddedTrueTypeBodyIsCopiedWithItsBoldFace) {
  const auto epub = book(
      "@font-face { font-family: 'Ubuntu Book'; src: url(../Fonts/u.woff2) format('woff2'), url(../Fonts/u.ttf); }\n"
      "@font-face { font-family: 'Ubuntu Book'; font-weight: bold; src: url(../Fonts/ub.ttf); }\n"
      "@font-face { font-family: 'Ubuntu Book'; font-weight: 600; src: url(../Fonts/semi.ttf); }\n"
      "body { font-family: 'Ubuntu Book', serif }\n",
      {"<body><p>Body text with <b>bold words</b>.</p></body>"},  // the bold face is copied for its text
      {{"OEBPS/Fonts/u.ttf", "font/ttf", kTrueType},
       {"OEBPS/Fonts/ub.ttf", "font/ttf", kTrueTypeBold},
       {"OEBPS/Fonts/semi.ttf", "font/ttf", kTrueType}});
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
  std::string name;
  ASSERT_EQ(BookFonts::State::Family, BookFonts::read(epub->getCachePath(), &name));
  EXPECT_EQ("Ubuntu Book", name);
  const fs::path dir = BookFonts::familyDir(epub->getCachePath());
  EXPECT_EQ(kTrueType, readFile(dir / "0.ttf"));
  EXPECT_EQ(kTrueTypeBold, readFile(dir / "1.ttf"));  // weight 700 nearer than 600, whose file is not copied
  EXPECT_FALSE(fs::exists(dir / "2.ttf"));
  EXPECT_FALSE(fs::exists(dir / "3.ttf"));
}

TEST_F(BookFontsTest, AnIdpfObfuscatedFaceIsUndoneWithTheUniqueIdentifier) {
  const std::string uid = "urn:uuid:9f1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9";
  uint8_t key[20];
  ASSERT_TRUE(BookFonts::idpfKey(uid, key));
  const auto epub = book("@font-face { font-family: Body; src: url(../Fonts/b.ttf); }\np { font-family: Body }\n",
                         {"<body><p>Body text.</p></body>"},
                         {{"OEBPS/Fonts/b.ttf", "application/vnd.ms-opentype", obfuscated(kTrueType, key, 20, 1040)}},
                         "<dc:identifier id=\"other\">urn:isbn:978-0-00-000000-2</dc:identifier>"
                         "<dc:identifier id=\"uid\">  " +
                             uid + "\n</dc:identifier>");
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
  EXPECT_EQ(kTrueType, readFile(fs::path(BookFonts::familyDir(epub->getCachePath())) / "0.ttf"));
}

TEST_F(BookFontsTest, AnAdobeObfuscatedFaceIsUndoneWithTheUuid) {
  uint8_t key[16];
  ASSERT_TRUE(BookFonts::adobeKey("urn:uuid:0123456789abcdef0123456789abcdef", key));
  BuildArena arena(52 * 1024);  // the lent framebuffer's share: rings and parser state come from it
  ASSERT_TRUE(arena.valid());
  const auto epub = book("@font-face { font-family: Body; src: url(../Fonts/b.ttf); }\np { font-family: Body }\n",
                         {"<body><p>Body text.</p></body>"},
                         {{"OEBPS/Fonts/b.ttf", "font/ttf", obfuscated(kTrueType, key, 16, 1024)}},
                         "<dc:identifier id=\"uid\">urn:uuid:0123456789abcdef0123456789abcdef</dc:identifier>");
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, &arena));
  EXPECT_GE(arena.highWater(), SaxParser::stateBytes());  // the parser state came from it
  EXPECT_EQ(0u, arena.used());                            // and everything was given back
  EXPECT_EQ(kTrueType, readFile(fs::path(BookFonts::familyDir(epub->getCachePath())) / "0.ttf"));
}

TEST_F(BookFontsTest, AnEmbeddedCffFaceIsAccepted) {
  ASSERT_EQ(0, kCff.compare(0, 4, "OTTO"));
  const auto cff = book("@font-face { font-family: Body; src: url(../Fonts/b.otf); }\np { font-family: Body }\n",
                        {"<body><p>Body text.</p></body>"}, {{"OEBPS/Fonts/b.otf", "font/otf", kCff}});
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*cff, nullptr));
  // Under the copy name every face has; discovery reads the content.
  EXPECT_EQ(kCff, readFile(fs::path(BookFonts::familyDir(cff->getCachePath())) / "0.ttf"));
}

TEST_F(BookFontsTest, AnObfuscatedCffFaceIsAccepted) {
  const std::string uid = "urn:uuid:9f1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9";
  uint8_t key[20];
  ASSERT_TRUE(BookFonts::idpfKey(uid, key));
  const auto epub = book("@font-face { font-family: Body; src: url(../Fonts/b.otf); }\np { font-family: Body }\n",
                         {"<body><p>Body text.</p></body>"},
                         {{"OEBPS/Fonts/b.otf", "application/vnd.ms-opentype", obfuscated(kCff, key, 20, 1040)}},
                         "<dc:identifier id=\"uid\">" + uid + "</dc:identifier>");
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
  EXPECT_EQ(kCff, readFile(fs::path(BookFonts::familyDir(epub->getCachePath())) / "0.ttf"));
}

TEST_F(BookFontsTest, WoffCollectionsAndUnknownBytesStayRefused) {
  // WOFF, WOFF2, a collection, and bytes that are neither a font nor obfuscated with this book's keys (encrypted).
  const char* const tags[] = {"wOFF", "wOF2", "ttcf"};
  for (const char* tag : tags) {
    std::string bytes = kTrueType;
    bytes.replace(0, 4, tag);
    const auto epub = book("@font-face { font-family: Body; src: url(../Fonts/b.ttf); }\np { font-family: Body }\n",
                           {"<body><p>Body text.</p></body>"}, {{"OEBPS/Fonts/b.ttf", "font/ttf", bytes}});
    EXPECT_EQ(BookFonts::State::None, BookFonts::prepare(*epub, nullptr)) << tag;
    EXPECT_FALSE(fs::exists(fs::path(BookFonts::familyDir(epub->getCachePath())) / "0.ttf")) << tag;
  }
  const auto encrypted =
      book("@font-face { font-family: Body; src: url(../Fonts/b.ttf); }\np { font-family: Body }\n",
           {"<body><p>Body text.</p></body>"}, {{"OEBPS/Fonts/b.ttf", "font/ttf", std::string(4096, '\x5a')}});
  EXPECT_EQ(BookFonts::State::None, BookFonts::prepare(*encrypted, nullptr));
}

TEST_F(BookFontsTest, ABookExaminedBeforeIsExaminedAgain) {
  // A CFF book was examined as None by the version before this one: its record must not stand.
  const auto epub = book("@font-face { font-family: Body; src: url(../Fonts/b.otf); }\np { font-family: Body }\n",
                         {"<body><p>Body text.</p></body>"}, {{"OEBPS/Fonts/b.otf", "font/otf", kCff}});
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
  const fs::path state = fs::path(epub->getCachePath()) / "fonts.bin";
  std::string bytes = readFile(state);
  ASSERT_FALSE(bytes.empty());
  ASSERT_EQ(static_cast<char>(BookFonts::kVersion), bytes[0]);
  bytes[0] = 5;  // the version before CFF faces
  bytes[1] = 0;  // and its verdict on this book: None
  {
    std::ofstream out(state, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  EXPECT_EQ(BookFonts::State::Pending, BookFonts::read(epub->getCachePath()));
}

namespace {
// Sets the first byte (the version) of a cache file.
void setVersionByte(const fs::path& file, const char version) {
  std::string bytes = readFile(file);
  ASSERT_FALSE(bytes.empty()) << file;
  bytes[0] = version;
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
const char* const kCffBookCss =
    "@font-face { font-family: Body; src: url(../Fonts/b.otf); }\np { font-family: Body; "
    "font-style: italic }\n";
}  // namespace

// An upgrade that changes the book cache and the CSS cache versions: the book as an older firmware left it.
TEST_F(BookFontsTest, AStaleCssCacheIsCompiledAgainWhenTheBookCacheIsRebuilt) {
  const auto first = book(kCffBookCss, {"<body><p>Body text.</p></body>"}, {{"OEBPS/Fonts/b.otf", "font/otf", kCff}});
  const fs::path cache = first->getCachePath();
  ASSERT_TRUE(fs::exists(cache / "css_rules.cache"));
  ASSERT_TRUE(fs::exists(cache / "css_fonts.bin"));
  const char old = static_cast<char>(CssParser::CSS_CACHE_VERSION - 1);
  setVersionByte(cache / "book.bin", 1);
  setVersionByte(cache / "css_rules.cache", old);
  setVersionByte(cache / "css_fonts.bin", old);

  Epub again(first->getPath(), (work / "cache").string());
  ASSERT_TRUE(again.load(true));
  EXPECT_EQ(static_cast<char>(CssParser::CSS_CACHE_VERSION), readFile(cache / "css_rules.cache")[0]);
  EXPECT_EQ(static_cast<char>(CssParser::CSS_CACHE_VERSION), readFile(cache / "css_fonts.bin")[0]);
  ASSERT_NE(nullptr, again.getCssParser());
  EXPECT_EQ(CssFontStyle::Italic, again.getCssParser()->resolveStyle("p", "").fontStyle) << "the rules apply";
}

TEST_F(BookFontsTest, AStaleCssCacheNeedsFirstOpenIndexing) {
  const auto first = book(kCffBookCss, {"<body><p>Body text.</p></body>"}, {{"OEBPS/Fonts/b.otf", "font/otf", kCff}});
  EXPECT_FALSE(Epub(first->getPath(), (work / "cache").string()).needsFirstOpenIndexing());
  setVersionByte(fs::path(first->getCachePath()) / "css_rules.cache",
                 static_cast<char>(CssParser::CSS_CACHE_VERSION - 1));
  EXPECT_TRUE(Epub(first->getPath(), (work / "cache").string()).needsFirstOpenIndexing());
}

// The device case: the older firmware refused the book's CFF face, and the upgrade changed every cache version.
TEST_F(BookFontsTest, ABookExaminedByAnOlderFirmwareGetsItsCffFontAfterTheUpgrade) {
  const auto first = book(kCffBookCss, {"<body><p>Body text.</p></body>"}, {{"OEBPS/Fonts/b.otf", "font/otf", kCff}});
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*first, nullptr));
  const fs::path cache = first->getCachePath();
  const char old = static_cast<char>(CssParser::CSS_CACHE_VERSION - 1);
  setVersionByte(cache / "fonts.bin", 5);
  {
    std::string bytes = readFile(cache / "fonts.bin");
    bytes[1] = 0;  // None
    std::ofstream out(cache / "fonts.bin", std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  fs::remove_all(fs::path(BookFonts::familyDir(first->getCachePath())));
  setVersionByte(cache / "book.bin", 1);
  setVersionByte(cache / "css_rules.cache", old);
  setVersionByte(cache / "css_fonts.bin", old);
  ASSERT_EQ(BookFonts::State::Pending, BookFonts::read(first->getCachePath()));

  Epub again(first->getPath(), (work / "cache").string());
  ASSERT_TRUE(again.load(true));
  again.setupCacheDir();
  EXPECT_EQ(BookFonts::State::Family, BookFonts::prepare(again, nullptr));
  EXPECT_EQ(kCff, readFile(fs::path(BookFonts::familyDir(again.getCachePath())) / "0.ttf"));
}

// Not "no faces": the file is there, in a form this firmware cannot read, so nothing is decided from it.
TEST_F(BookFontsTest, AFontFacesFileOfAnotherVersionLeavesTheBookPending) {
  const auto epub = book(kCffBookCss, {"<body><p>Body text.</p></body>"}, {{"OEBPS/Fonts/b.otf", "font/otf", kCff}});
  const fs::path cache = epub->getCachePath();
  setVersionByte(cache / "css_fonts.bin", static_cast<char>(CssParser::CSS_CACHE_VERSION - 1));
  EXPECT_EQ(BookFonts::State::Pending, BookFonts::prepare(*epub, nullptr));
  EXPECT_FALSE(fs::exists(cache / "fonts.bin"));
  EXPECT_FALSE(fs::exists(fs::path(BookFonts::familyDir(epub->getCachePath())) / "0.ttf"));
}

TEST_F(BookFontsTest, ABookWithoutAnEmbeddedBodyFamilyIsNone) {
  const auto plain = book("p { margin: 0 }\n", {"<body><p>Body text.</p></body>"});
  EXPECT_EQ(BookFonts::State::Pending, BookFonts::read(plain->getCachePath()));
  EXPECT_EQ(BookFonts::State::None, BookFonts::prepare(*plain, nullptr));
  EXPECT_EQ(BookFonts::State::None, BookFonts::read(plain->getCachePath()));

  // A face is declared, but the body is in another family.
  const auto other =
      book("@font-face { font-family: Display; src: url(../Fonts/d.ttf); }\nh1 { font-family: Display }\n",
           {"<body><h1>Title</h1><p>Body text.</p></body>"}, {{"OEBPS/Fonts/d.ttf", "font/ttf", kTrueType}});
  EXPECT_EQ(BookFonts::State::None, BookFonts::prepare(*other, nullptr));

  // A face whose file is missing from the book.
  const auto missing = book("@font-face { font-family: Body; src: url(../Fonts/gone.ttf); }\np { font-family: Body }\n",
                            {"<body><p>Body text.</p></body>"});
  EXPECT_EQ(BookFonts::State::None, BookFonts::prepare(*missing, nullptr));
}

TEST_F(BookFontsTest, AChapterThatCannotBeReadIsRetriedTwiceMore) {
  const auto epub = book("@font-face { font-family: Body; src: url(../Fonts/b.ttf); }\np { font-family: Body }\n",
                         {kMissing}, {{"OEBPS/Fonts/b.ttf", "font/ttf", kTrueType}});
  EXPECT_EQ(BookFonts::State::Pending, BookFonts::prepare(*epub, nullptr));
  EXPECT_EQ(BookFonts::State::Pending, BookFonts::prepare(*epub, nullptr));
  EXPECT_EQ(BookFonts::State::None, BookFonts::prepare(*epub, nullptr));  // given up on
  EXPECT_EQ(BookFonts::State::None, BookFonts::read(epub->getCachePath()));
}

TEST_F(BookFontsTest, AnAccentLimitHoldsUnderItsSettingsAndIsForgottenUnderOthers) {
  // The memory ladder's FewerFamilies step: kept across sessions, so a restart does not fail the same way again.
  const std::string cache = (work / "limit").string();
  fs::create_directories(cache);
  EXPECT_EQ(BookFonts::kMaxAccents, BookFonts::accentLimit(cache, 7));
  BookFonts::setAccentLimit(cache, 7, 2);
  EXPECT_EQ(2, BookFonts::accentLimit(cache, 7));
  BookFonts::setAccentLimit(cache, 7, 0);
  EXPECT_EQ(0, BookFonts::accentLimit(cache, 7));
  EXPECT_EQ(BookFonts::kMaxAccents, BookFonts::accentLimit(cache, 8));  // other font settings: forgotten
  EXPECT_EQ(BookFonts::kMaxAccents, BookFonts::accentLimit(cache, 7));  // for good
  BookFonts::setAccentLimit(cache, 7, 1);
  BookFonts::setAccentLimit(cache, 7, BookFonts::kMaxAccents);
  EXPECT_FALSE(fs::exists(cache + "/fontlimit.bin"));
}

// ---- Stage B phase 1: the body's generic family and the accent family -------------------------------------

namespace {

const std::string kInter = readFile(FONT_FIXTURE_DIR "/Inter/Inter-Regular.ttf");
const std::string kNotoSans = readFile(FONT_FIXTURE_DIR "/NotoSans/NotoSans-Regular.ttf");

// The body in Body, chapter titles and headings in Display, a dedication in Script.
const char* kAccentCss =
    "@font-face { font-family: Body; src: url(../Fonts/b.ttf); }\n"
    "@font-face { font-family: Display; src: url(../Fonts/d.ttf); }\n"
    "@font-face { font-family: Script; src: url(../Fonts/s.ttf); }\n"
    "p { font-family: Body, serif }\n"
    "h1, h2 { font-family: Display, sans-serif }\n"
    ".dedication, .aside { font-family: Script }\n";

const char* kAccentChapter =
    "<body><h1>One</h1><p class='dedication'>For someone</p>"
    "<p>Body prose with an <span class='aside'>inline aside</span> and more body prose, long enough to be the "
    "body text by far, as body text is.</p><p>More body prose.</p>"
    "<h2><span class='aside'>A heading set in its span</span></h2><h2>Two</h2><h2>Three</h2></body>";

}  // namespace

TEST_F(BookFontsTest, TheBodysGenericFamilyIsKept) {
  const auto epub = book("body { font-family: serif }\nh1 { font-family: sans-serif }\n",
                         {"<body><h1>Title</h1><p>Prose in the generic serif the body names.</p></body>"});
  const BookFonts::Found families = found(*epub);
  EXPECT_EQ(CssFontFamily::kSerif, families.body);
  EXPECT_EQ(0u, families.accents[0]);  // a generic family is no accent
  // Probed although the book embeds no font, so the reader knows what its body is set in.
  ASSERT_EQ(BookFonts::State::None, BookFonts::prepare(*epub, nullptr));
  BookFonts::Families read;
  EXPECT_EQ(BookFonts::State::None, BookFonts::read(epub->getCachePath(), nullptr, &read));
  EXPECT_EQ(CssFontFamily::kSerif, read.body);
  EXPECT_EQ(0u, read.accents[0]);
}

TEST_F(BookFontsTest, TheGenericOfMostOfTheBodyTextIsTheBodys) {
  // A family counts as one with and without its fallback; its most-used list gives the generic.
  const auto epub = book("p { font-family: Body, sans-serif }\n.short { font-family: Body }\n",
                         {"<body><p>The longer run of body text, long enough to count.</p>"
                          "<p class='short'>Short.</p></body>"});
  EXPECT_EQ(family("Body") | CssFontFamily::kSansSerif, found(*epub).body);
}

TEST_F(BookFontsTest, TheAccentIsTheFamilyWithTheMostOtherText) {
  const auto epub = book(kAccentCss, {kAccentChapter},
                         {{"OEBPS/Fonts/b.ttf", "font/ttf", kTrueType},
                          {"OEBPS/Fonts/d.ttf", "font/ttf", kInter},
                          {"OEBPS/Fonts/s.ttf", "font/ttf", kNotoSans}});
  std::string records;
  ASSERT_TRUE(CssParser::loadFontFaces(epub->getCachePath(), records));
  // Display: three headings, 64 bytes each at least (192). Script: the dedication and the heading set in its span,
  // 64 each, and the inline aside's 12 (140).
  const BookFonts::Found families = found(*epub, records);
  EXPECT_EQ(family("Body") | CssFontFamily::kSerif, families.body);
  EXPECT_EQ(family("Display"), families.accents[0]);
  EXPECT_EQ(family("Script"), families.accents[1]);  // the second accent family
  EXPECT_EQ(0u, families.accents[2]);

  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
  std::string body;
  std::string accent;
  std::string second;
  BookFonts::Families read;
  ASSERT_EQ(BookFonts::State::Family, BookFonts::read(epub->getCachePath(), &body, &read));
  EXPECT_EQ(BookFonts::State::Family, BookFonts::read(epub->getCachePath(), &accent, nullptr, 0));
  EXPECT_EQ(BookFonts::State::Family, BookFonts::read(epub->getCachePath(), &second, nullptr, 1));
  EXPECT_EQ("Body", body);
  EXPECT_EQ("Display", accent);
  EXPECT_EQ("Script", second);
  EXPECT_EQ(families.body, read.body);
  EXPECT_EQ(family("Display"), read.accents[0]);
  EXPECT_EQ(family("Script"), read.accents[1]);
  EXPECT_EQ(kTrueType, readFile(fs::path(BookFonts::familyDir(epub->getCachePath())) / "0.ttf"));
  EXPECT_EQ(kInter, readFile(fs::path(BookFonts::familyDir(epub->getCachePath(), 0)) / "0.ttf"));
  EXPECT_EQ(kNotoSans, readFile(fs::path(BookFonts::familyDir(epub->getCachePath(), 1)) / "0.ttf"));
}

TEST_F(BookFontsTest, AHeadingSetInASpanCountsForTheSpansFamily) {
  // The span heading counts for Script (140 with it), not Display (64 without Two and Three).
  std::string chapter = kAccentChapter;
  chapter.replace(chapter.find("<h2>Two</h2><h2>Three</h2>"), 26, "");
  const auto epub = book(kAccentCss, {chapter},
                         {{"OEBPS/Fonts/b.ttf", "font/ttf", kTrueType},
                          {"OEBPS/Fonts/d.ttf", "font/ttf", kInter},
                          {"OEBPS/Fonts/s.ttf", "font/ttf", kNotoSans}});
  std::string records;
  ASSERT_TRUE(CssParser::loadFontFaces(epub->getCachePath(), records));
  EXPECT_EQ(family("Script"), found(*epub, records).accents[0]);
  EXPECT_EQ(family("Display"), found(*epub, records).accents[1]);
}

TEST_F(BookFontsTest, AnAccentWithOnlyABoldFaceIsDrawnInIt) {
  std::string css = kAccentCss;
  css.insert(css.find("src: url(../Fonts/d.ttf)"), "font-weight: bold; ");  // Display has only a bold face
  const auto epub = book(css, {kAccentChapter},
                         {{"OEBPS/Fonts/b.ttf", "font/ttf", kTrueType},
                          {"OEBPS/Fonts/d.ttf", "font/ttf", kInter},
                          {"OEBPS/Fonts/s.ttf", "font/ttf", kNotoSans}});
  std::string records;
  ASSERT_TRUE(CssParser::loadFontFaces(epub->getCachePath(), records));
  const BookFonts::Found families = found(*epub, records);
  EXPECT_EQ(family("Display"), families.accents[0]);  // the most text, as with a regular face
  EXPECT_EQ(family("Script"), families.accents[1]);
  // Its one face is copied as the regular, once: the bold falls back to it.
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
  EXPECT_EQ(kInter, readFile(fs::path(BookFonts::familyDir(epub->getCachePath(), 0)) / "0.ttf"));
  EXPECT_FALSE(fs::exists(fs::path(BookFonts::familyDir(epub->getCachePath(), 0)) / "1.ttf"));
}

TEST_F(BookFontsTest, ARefusedAccentGivesItsPlaceToTheNext) {
  std::string woff = kTrueType;
  woff.replace(0, 4, "wOFF");
  const auto epub = book(kAccentCss, {kAccentChapter},
                         {{"OEBPS/Fonts/b.ttf", "font/ttf", kTrueType},
                          {"OEBPS/Fonts/d.ttf", "font/woff", woff},
                          {"OEBPS/Fonts/s.ttf", "font/ttf", kNotoSans}});
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
  BookFonts::Families read;
  std::string accent;
  std::string second = "stale";
  EXPECT_EQ(BookFonts::State::Family, BookFonts::read(epub->getCachePath(), &accent, &read, 0));
  EXPECT_EQ(BookFonts::State::Family, BookFonts::read(epub->getCachePath(), &second, nullptr, 1));
  EXPECT_EQ(family("Script"), read.accents[0]);  // Display's WOFF is refused: Script is the first accent now
  EXPECT_EQ(0u, read.accents[1]);
  EXPECT_EQ("Script", accent);
  EXPECT_EQ("", second);
  EXPECT_EQ(kNotoSans, readFile(fs::path(BookFonts::familyDir(epub->getCachePath(), 0)) / "0.ttf"));
  EXPECT_FALSE(fs::exists(BookFonts::familyDir(epub->getCachePath(), 1)));
  EXPECT_EQ(kTrueType, readFile(fs::path(BookFonts::familyDir(epub->getCachePath())) / "0.ttf"));
}

TEST_F(BookFontsTest, AnAccentIsCopiedWhenTheBodyIsNotEmbedded) {
  const auto epub = book(
      "@font-face { font-family: Display; src: url(../Fonts/d.ttf); }\nh1, h2 { font-family: Display }\n",
      {"<body><h1>Title</h1><p>Body prose in the reader's font, long enough to be the body.</p><h2>Two</h2></body>"},
      {{"OEBPS/Fonts/d.ttf", "font/ttf", kInter}});
  ASSERT_EQ(BookFonts::State::None, BookFonts::prepare(*epub, nullptr));
  std::string accent;
  BookFonts::Families read;
  EXPECT_EQ(BookFonts::State::None, BookFonts::read(epub->getCachePath(), &accent, &read, 0));
  EXPECT_EQ("Display", accent);
  EXPECT_EQ(family("Display"), read.accents[0]);
  EXPECT_EQ(0u, read.body);
  EXPECT_EQ(kInter, readFile(fs::path(BookFonts::familyDir(epub->getCachePath(), 0)) / "0.ttf"));
}

// ---- Stage B phase 3: several accent families -----------------------------------------------------------

namespace {

// Four families besides the body, ranked by their text: Display (three headings, 192), Script (142), Code (75), Rare
// (64, later in the book).
const char* kFourAccentCss =
    "@font-face { font-family: Body; src: url(../Fonts/b.ttf); }\n"
    "@font-face { font-family: Display; src: url(../Fonts/d.ttf); }\n"
    "@font-face { font-family: Script; src: url(../Fonts/s.ttf); }\n"
    "@font-face { font-family: Code; src: url(../Fonts/c.ttf); }\n"
    "@font-face { font-family: Rare; src: url(../Fonts/r.ttf); }\n"
    "p { font-family: Body }\nh1, h2 { font-family: Display }\n.script { font-family: Script }\n"
    "pre, code { font-family: Code, monospace }\n.rare { font-family: Rare }\n";

const char* kFourAccentChapter =
    "<body><h1>One</h1><h2>Two</h2><h2>Three</h2><p class='script'>For someone</p>"
    "<p>Body prose, long enough to be the body text by far, with <code>inline code</code> and a "
    "<span class='script'>scripted aside</span> in it, as body text is.</p><p>More body prose.</p>"
    "<h2 class='script'>A heading in the second accent</h2><pre>code block</pre><p class='rare'>Rare</p></body>";

}  // namespace

TEST_F(BookFontsTest, TheAccentFamiliesAreRankedByTheirTextAndTheFirstThreeCopied) {
  const auto epub = book(kFourAccentCss, {kFourAccentChapter},
                         {{"OEBPS/Fonts/b.ttf", "font/ttf", kTrueType},
                          {"OEBPS/Fonts/d.ttf", "font/ttf", kInter},
                          {"OEBPS/Fonts/s.ttf", "font/ttf", kNotoSans},
                          {"OEBPS/Fonts/c.ttf", "font/ttf", kTrueTypeBold},
                          {"OEBPS/Fonts/r.ttf", "font/ttf", kTrueType}});
  std::string records;
  ASSERT_TRUE(CssParser::loadFontFaces(epub->getCachePath(), records));
  const BookFonts::Found families = found(*epub, records);
  EXPECT_EQ(family("Display"), families.accents[0]);
  EXPECT_EQ(family("Script"), families.accents[1]);
  // Code: the code block's 64 and the inline code's 11; Rare: its block's 64.
  EXPECT_EQ(family("Code"), families.accents[2]);

  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
  BookFonts::Families read;
  std::string names[BookFonts::kMaxAccents];
  for (int n = 0; n < BookFonts::kMaxAccents; ++n) {
    EXPECT_EQ(BookFonts::State::Family, BookFonts::read(epub->getCachePath(), &names[n], &read, n));
  }
  EXPECT_EQ("Display", names[0]);
  EXPECT_EQ("Script", names[1]);
  EXPECT_EQ("Code", names[2]);
  EXPECT_EQ(family("Code"), read.accents[2]);
  EXPECT_EQ(kInter, readFile(fs::path(BookFonts::familyDir(epub->getCachePath(), 0)) / "0.ttf"));
  EXPECT_EQ(kNotoSans, readFile(fs::path(BookFonts::familyDir(epub->getCachePath(), 1)) / "0.ttf"));
  EXPECT_EQ(kTrueTypeBold, readFile(fs::path(BookFonts::familyDir(epub->getCachePath(), 2)) / "0.ttf"));
  EXPECT_FALSE(fs::exists(BookFonts::familyDir(epub->getCachePath(), 3)));  // Rare: past kMaxAccents
}

// ---- Ranked by their text ---------------------------------------------------------------------------------

TEST_F(BookFontsTest, AnInlineOnlyFamilyIsAnAccent) {
  std::string chapter = "<body>";
  for (int i = 0; i < 20; ++i) {
    chapter += "<p>";
    for (int j = 0; j < 5; ++j) chapter += "Du<span class='hand'>n</span>thorpe ";
    chapter += "</p>";
  }
  const auto epub = book(faceRules({"Body", "Hand"}) + "p { font-family: Body }\n.hand { font-family: Hand }\n",
                         {chapter + "</body>"}, faceFiles({"Body", "Hand"}));
  EXPECT_EQ(family("Hand"), ranked(*epub).accents[0]);  // in no block of its own
}

TEST_F(BookFontsTest, TheAccentWithMoreTextOutranksOneWithMoreShortBlocks) {
  std::string chapter = prose("body", 4 * 1024);
  chapter.erase(chapter.size() - 7);                                       // "</body>"
  for (int i = 0; i < 12; ++i) chapter += "<p class='ad'>Buy it now</p>";  // 12 blocks of 10 bytes: 768
  std::string epigraph;
  for (int i = 0; i < 100; ++i) epigraph += "words ";
  for (int i = 0; i < 2; ++i) chapter += "<p class='epi'>" + epigraph + "</p>";  // 2 blocks of 600 bytes
  const auto epub = book(faceRules({"Body", "Ads", "Epigraph"}) +
                             ".body { font-family: Body }\n.ad { font-family: Ads }\n.epi { font-family: Epigraph }\n",
                         {chapter + "</body>"}, faceFiles({"Body", "Ads", "Epigraph"}));
  const BookFonts::Found families = ranked(*epub);
  EXPECT_EQ(family("Epigraph"), families.accents[0]);
  EXPECT_EQ(family("Ads"), families.accents[1]);
}

TEST_F(BookFontsTest, HeadingsAcrossChaptersOutrankAFewInlineWords) {
  // Display: ten 8-byte headings, 64 each. Script: twenty 20-byte spans.
  const std::string chapter =
      "<body><h2>Chapter.</h2><p>Body prose with <span class='script'>twenty bytes of text</span> in it and "
      "<span class='script'>twenty bytes of text</span> again, as the body text of a chapter goes on.</p></body>";
  const auto epub = book(faceRules({"Body", "Display", "Script"}) +
                             "p { font-family: Body }\nh2 { font-family: Display }\n.script { font-family: Script }\n",
                         std::vector<std::string>(10, chapter), faceFiles({"Body", "Display", "Script"}));
  const BookFonts::Found families = ranked(*epub);
  EXPECT_EQ(family("Display"), families.accents[0]);
  EXPECT_EQ(family("Script"), families.accents[1]);
}

TEST_F(BookFontsTest, ANegligibleFamilyTakesNoSlot) {
  std::vector<std::string> chapters(8, prose("body", 50 * 1024));  // ~280 KB of text
  chapters.push_back("<body><p class='rare'>Rare</p></body>");
  const auto epub = book(faceRules({"Body", "Rare"}) + ".body { font-family: Body }\n.rare { font-family: Rare }\n",
                         chapters, faceFiles({"Body", "Rare"}));
  EXPECT_EQ(0u, ranked(*epub).accents[0]);
}

TEST_F(BookFontsTest, ASmallFamilyInAShortBookKeepsItsSlot) {
  const auto epub =
      book(faceRules({"Body", "Rare"}) + ".body { font-family: Body }\n.rare { font-family: Rare }\n",
           {prose("body", 2 * 1024), "<body><p class='rare'>Rare</p></body>"}, faceFiles({"Body", "Rare"}));
  EXPECT_EQ(family("Rare"), ranked(*epub).accents[0]);
}

TEST_F(BookFontsTest, FoundCarriesTheStylesOfTheChosenFamilies) {
  // Each block's 5 bytes and its 59-byte shortfall to 64, in its first run's style; one item, read whole.
  const auto epub = book(faceRules({"Body"}) + "p { font-family: Body }\n",
                         {"<body><p>Plain</p><p><i>Slant</i></p><p><b>Heavy</b></p></body>"}, faceFiles({"Body"}));
  const BookFonts::Found families = ranked(*epub);
  ASSERT_EQ(family("Body"), CssFontFamily::identity(families.body));
  EXPECT_EQ(64u, families.bodyStyles[0]);
  EXPECT_EQ(64u, families.bodyStyles[1]);  // bold
  EXPECT_EQ(64u, families.bodyStyles[2]);  // italic
  EXPECT_EQ(0u, families.bodyStyles[3]);
}

TEST_F(BookFontsTest, AMiseryShapedBookGetsItsTypewriterAndHandFilledLetters) {
  // Misery's shape: an ad page and a part title in families of their own before the text; the typed manuscript's
  // paragraphs inside a chapter, with letters filled in by hand inside its words.
  std::string italic;
  for (int i = 0; i < 100; ++i) italic += "italic";  // 600 bytes
  std::string typed;
  for (int i = 0; i < 100; ++i) {
    typed += "Du<span class='hand'>n</span>thorpe i<span class='hand'>n</span> the rai<span class='hand'>n</span>. ";
  }
  std::string chapter = prose("body", 20 * 1024);
  chapter.erase(chapter.size() - 7);  // "</body>"
  for (int i = 0; i < 5; ++i) chapter += "<p class='typed'>" + typed + "</p>";
  std::vector<std::string> chapters = {
      "<body><img src='c.jpg'/></body>",
      "<body><p class='ad'>Get updates</p><p class='ad'>Sign up</p><p class='ad'>Click</p><p class='ad'>Now</p></body>",
      prose("body", 8 * 1024)};
  for (int i = 0; i < 5; ++i) chapters.push_back("<body><p class='body'>Front matter.</p></body>");
  chapters.push_back("<body><p class='part'>PART I</p><p class='part'><i>" + italic + "</i></p></body>");
  chapters.push_back(chapter + "</body>");
  const std::vector<std::string> names = {"Body", "Ads", "Narrow", "Typed", "Hand"};
  const auto epub = book(faceRules(names) +
                             ".body { font-family: Body }\n.ad { font-family: Ads }\n.part { font-family: Narrow }\n"
                             ".typed { font-family: Typed }\n.hand { font-family: Hand }\n",
                         chapters, faceFiles(names));
  const BookFonts::Found families = ranked(*epub);
  EXPECT_EQ(family("Body"), CssFontFamily::identity(families.body));
  EXPECT_EQ(family("Typed"), families.accents[0]);
  EXPECT_EQ(family("Hand"), families.accents[1]);
  EXPECT_EQ(family("Narrow"), families.accents[2]);  // Ads, 4 short blocks, out-ranked
  EXPECT_EQ(64u, families.accentStyles[2][0]);       // PART I
  EXPECT_EQ(600u, families.accentStyles[2][2]);      // its italic line
}

// ---- The faces copied: those whose style carries text -----------------------------------------------------

// Scores and masks are indexed by EpdFontFamily style: 0 regular, 1 bold, 2 italic, 3 bold italic; 0b0101 is
// regular and italic.
TEST(BookFontsStyles, TheFacesKeptAreThoseWithText) {
  EXPECT_EQ(0b0101, BookFonts::keptStyles({427731, 121, 32091, 6}, 0b1111));  // Misery's body: R, I
  EXPECT_EQ(0b0101, BookFonts::keptStyles({362, 0, 1471, 0}, 0b0101));        // BkNarrow: both
  EXPECT_EQ(0b0100, BookFonts::keptStyles({10, 0, 2000, 0}, 0b0101));         // italic alone
  EXPECT_EQ(0b0101, BookFonts::keptStyles({5000, 10, 3000, 900}, 0b1111));    // bold italic goes with bold
  EXPECT_EQ(0b0001, BookFonts::keptStyles({0, 0, 0, 0}, 0b1111));             // no text: the regular alone
  EXPECT_EQ(0b0010, BookFonts::keptStyles({0, 0, 0, 0}, 0b0010));             // a bold-only family: its bold
  EXPECT_EQ(0b0011, BookFonts::keptStyles({50, 10, 0, 0}, 0b0011));           // a short book keeps its bold
}

TEST_F(BookFontsTest, ABoldUsedForAFewWordsIsNotCopied) {
  std::string chapter = prose("body", 30 * 1024);
  chapter.insert(chapter.size() - 7, "<p class='body'>Prose with <b>two words</b> in bold.</p>");
  const auto epub = book(
      "@font-face { font-family: Body; src: url(../Fonts/b.ttf); }\n"
      "@font-face { font-family: Body; font-weight: bold; src: url(../Fonts/bb.ttf); }\n.body { font-family: Body }\n",
      {chapter}, {{"OEBPS/Fonts/b.ttf", "font/ttf", kTrueType}, {"OEBPS/Fonts/bb.ttf", "font/ttf", kTrueTypeBold}});
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
  const fs::path dir = BookFonts::familyDir(epub->getCachePath());
  EXPECT_EQ(kTrueType, readFile(dir / "0.ttf"));
  EXPECT_FALSE(fs::exists(dir / "1.ttf"));  // drawn emboldened from the regular
}

TEST_F(BookFontsTest, AnItalicDominantAccentKeepsItsItalicAsItsRegular) {
  std::string italic;
  for (int i = 0; i < 250; ++i) italic += "<p class='part'><i>An italic line of a part title page.</i></p>";
  std::vector<std::string> chapters = {prose("body", 20 * 1024), "<body><p class='part'>PART</p>" + italic + "</body>"};
  const auto epub = book(
      "@font-face { font-family: Body; src: url(../Fonts/b.ttf); }\n"
      "@font-face { font-family: Narrow; src: url(../Fonts/n.ttf); }\n"
      "@font-face { font-family: Narrow; font-style: italic; src: url(../Fonts/ni.ttf); }\n"
      ".body { font-family: Body }\n.part { font-family: Narrow }\n",
      chapters,
      {{"OEBPS/Fonts/b.ttf", "font/ttf", kTrueType},
       {"OEBPS/Fonts/n.ttf", "font/ttf", kTrueType},
       {"OEBPS/Fonts/ni.ttf", "font/ttf", kNotoSans}});
  ASSERT_EQ(BookFonts::State::Family, BookFonts::prepare(*epub, nullptr));
  const fs::path dir = BookFonts::familyDir(epub->getCachePath(), 0);
  EXPECT_EQ(kNotoSans, readFile(dir / "0.ttf"));  // PART's 64 bytes are under 1 % of its 9 KB
  EXPECT_FALSE(fs::exists(dir / "2.ttf"));
}
