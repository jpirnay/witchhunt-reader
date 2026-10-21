#include <OpdsParser.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {
bool parseSingleBookEntry(OpdsEntry& entryOut, const char* href, const char* type = "application/epub+zip") {
  const std::string xml = std::string(R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <author><name>Example Author</name></author>
    <id>book-1</id>
    <link rel="http://opds-spec.org/acquisition" type=")") +
                          type + R"(" href=")" + href + R"("/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size());
  parser.flush();

  if (parser.error()) {
    ADD_FAILURE() << "parser.error() returned true";
    return false;
  }
  if (entries.size() != 1) {
    ADD_FAILURE() << "entries.size() == " << entries.size() << ", expected 1";
    return false;
  }
  entryOut = entries.front();
  return true;
}

bool assertSingleFormat(const OpdsEntry& entry, const char* formatKey, const char* fileExtension) {
  if (entry.type != OpdsEntryType::BOOK) {
    ADD_FAILURE() << "entry.type != OpdsEntryType::BOOK";
    return false;
  }
  if (entry.acquisitionLinks.size() != 1) {
    ADD_FAILURE() << "entry.acquisitionLinks.size() == " << entry.acquisitionLinks.size() << ", expected 1";
    return false;
  }
  if (entry.acquisitionLinks[0].formatKey != formatKey) {
    ADD_FAILURE() << "formatKey actual='" << entry.acquisitionLinks[0].formatKey << "' expected='" << formatKey << "'";
    return false;
  }
  if (entry.acquisitionLinks[0].fileExtension != fileExtension) {
    ADD_FAILURE() << "fileExtension actual='" << entry.acquisitionLinks[0].fileExtension << "' expected='"
                  << fileExtension << "'";
    return false;
  }
  return true;
}

std::vector<OpdsEntry> parseFeed(const char* xml, OpdsParser& parser) {
  std::vector<OpdsEntry> entries;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();
  return entries;
}
}  // namespace

TEST(OpdsParser, EpubExtension) {
  OpdsEntry entry;
  ASSERT_TRUE(parseSingleBookEntry(entry, "/books/example.epub"));
  ASSERT_TRUE(assertSingleFormat(entry, "epub", ".epub"));
}

TEST(OpdsParser, KepubDoubleExtension) {
  OpdsEntry entry;
  ASSERT_TRUE(parseSingleBookEntry(entry, "/books/example.kepub.epub"));
  ASSERT_TRUE(assertSingleFormat(entry, "kepub", ".kepub.epub"));
}

TEST(OpdsParser, BareKepubExtension) {
  OpdsEntry entry;
  ASSERT_TRUE(parseSingleBookEntry(entry, "/books/example.kepub"));
  ASSERT_TRUE(assertSingleFormat(entry, "kepub", ".kepub.epub"));
}

TEST(OpdsParser, SlashTerminatedKepubPath) {
  OpdsEntry entry;
  ASSERT_TRUE(parseSingleBookEntry(entry, "/opds/download/6516/kepub/"));
  ASSERT_TRUE(assertSingleFormat(entry, "kepub", ".kepub.epub"));
}

TEST(OpdsParser, SlashTerminatedEpubPath) {
  OpdsEntry entry;
  ASSERT_TRUE(parseSingleBookEntry(entry, "/opds/download/6516/epub/"));
  ASSERT_TRUE(assertSingleFormat(entry, "epub", ".epub"));
}

TEST(OpdsParser, DistinctAcquisitionFormatsRemainSeparate) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <author><name>Example Author</name></author>
    <id>book-2</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub"/>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.kepub.epub"/>
    <link rel="http://opds-spec.org/acquisition" type="text/plain" href="/books/example.txt"/>
    <link rel="http://opds-spec.org/acquisition" type="text/markdown" href="/books/example.md"/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();

  ASSERT_TRUE(!parser.error());
  ASSERT_EQ(entries.size(), static_cast<size_t>(1));
  const auto& links = entries.front().acquisitionLinks;
  ASSERT_EQ(links.size(), static_cast<size_t>(4));
  ASSERT_EQ(links[0].formatKey, "epub");
  ASSERT_EQ(links[1].formatKey, "kepub");
  ASSERT_EQ(links[2].formatKey, "txt");
  ASSERT_EQ(links[3].formatKey, "md");
}

TEST(OpdsParser, UnsupportedMimeType) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <author><name>Example Author</name></author>
    <id>book-3</id>
    <link rel="http://opds-spec.org/acquisition" type="application/x-mobipocket-ebook" href="/books/example.mobi"/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();

  ASSERT_TRUE(!parser.error());
  // Kept, as a book without acquisition links: the browser's sign for "Format not supported". It
  // used to vanish from the list.
  ASSERT_EQ(entries.size(), static_cast<size_t>(1));
  EXPECT_EQ(entries[0].type, OpdsEntryType::BOOK);
  EXPECT_EQ(entries[0].href, "/books/example.mobi");
  EXPECT_TRUE(entries[0].acquisitionLinks.empty());
}

TEST(OpdsParser, EmptyHrefOrType) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Empty Href</title>
    <author><name>Example Author</name></author>
    <id>book-4</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href=""/>
  </entry>
  <entry>
    <title>Empty Type</title>
    <author><name>Example Author</name></author>
    <id>book-5</id>
    <link rel="http://opds-spec.org/acquisition" type="" href="/books/example.epub"/>
  </entry>
  <entry>
    <title>Missing Href</title>
    <author><name>Example Author</name></author>
    <id>book-6</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip"/>
  </entry>
  <entry>
    <title>Missing Type</title>
    <author><name>Example Author</name></author>
    <id>book-7</id>
    <link rel="http://opds-spec.org/acquisition" href="/books/example.epub"/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();

  ASSERT_TRUE(!parser.error());
  // An empty or missing href leaves nothing to download, so those entries drop. An empty or missing
  // type does not: the href names an .epub, and that is the format.
  ASSERT_EQ(entries.size(), static_cast<size_t>(2));
  EXPECT_EQ(entries[0].title, "Empty Type");
  EXPECT_EQ(entries[1].title, "Missing Type");
  for (const auto& entry : entries) {
    if (assertSingleFormat(entry, "epub", ".epub")) {
      EXPECT_EQ(entry.acquisitionLinks[0].mimeType, "application/epub+zip");
    }
  }
}

TEST(OpdsParser, DuplicateAcquisitionLinks) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <author><name>Example Author</name></author>
    <id>book-8</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub"/>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example-copy.epub"/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();

  ASSERT_TRUE(!parser.error());
  ASSERT_EQ(entries.size(), static_cast<size_t>(1));
  const auto& links = entries.front().acquisitionLinks;
  ASSERT_EQ(links.size(), static_cast<size_t>(2));
  ASSERT_EQ(links[0].formatKey, "epub");
  ASSERT_EQ(links[0].href, "/books/example.epub");
  ASSERT_EQ(links[1].formatKey, "epub");
  ASSERT_EQ(links[1].href, "/books/example-copy.epub");
}

TEST(OpdsParser, IdenticalHrefAcquisitionLinksAreDeduplicated) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <author><name>Example Author</name></author>
    <id>book-9</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub"/>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub"/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();

  ASSERT_TRUE(!parser.error());
  ASSERT_EQ(entries.size(), static_cast<size_t>(1));
  const auto& links = entries.front().acquisitionLinks;
  ASSERT_EQ(links.size(), static_cast<size_t>(1));
  ASSERT_EQ(links[0].formatKey, "epub");
  ASSERT_EQ(links[0].href, "/books/example.epub");
}

TEST(OpdsParser, SlashVariantHrefAcquisitionLinksAreDeduplicated) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <author><name>Example Author</name></author>
    <id>book-10</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub"/>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub/"/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();

  ASSERT_TRUE(!parser.error());
  ASSERT_EQ(entries.size(), static_cast<size_t>(1));
  const auto& links = entries.front().acquisitionLinks;
  ASSERT_EQ(links.size(), static_cast<size_t>(1));
  ASSERT_EQ(links[0].formatKey, "epub");
  ASSERT_EQ(links[0].href, "/books/example.epub");
}

// A translator or illustrator listed after the author must not replace them: the name is shown in
// the list and goes into the download's file name.
TEST(OpdsParser, FirstAuthorIsKept) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <author><name>Leo Tolstoy</name></author>
    <author><name>Louise Maude</name></author>
    <id>book-11</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub"/>
  </entry>
</feed>)";

  std::vector<OpdsEntry> entries;
  OpdsParser parser;
  parser.onEntryParsed = [&](OpdsEntry e) { entries.push_back(std::move(e)); };
  parser.write(reinterpret_cast<const uint8_t*>(xml), strlen(xml));
  parser.flush();

  ASSERT_TRUE(!parser.error());
  ASSERT_EQ(entries.size(), static_cast<size_t>(1));
  EXPECT_EQ(entries.front().author, "Leo Tolstoy");
}

// The IANA link relations list "prev" beside "previous"; a catalog that uses it lost its "Previous
// page" row. From witchling-reader commit 18e5f3b1 (syfq91).
TEST(OpdsParser, PrevRelIsAPreviousPageLink) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <link rel="prev" href="/opds/catalog?p=2" type="application/atom+xml;profile=opds-catalog"/>
  <link rel="next" href="/opds/catalog?p=4" type="application/atom+xml;profile=opds-catalog"/>
</feed>)";
  OpdsParser parser;
  parseFeed(xml, parser);
  ASSERT_FALSE(parser.error());
  EXPECT_EQ(parser.getPrevPageUrl(), "/opds/catalog?p=2");
  EXPECT_EQ(parser.getNextPageUrl(), "/opds/catalog?p=4");
}

TEST(OpdsParser, PreviousRelIsStillAPreviousPageLink) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <link rel="previous" href="/opds/catalog?p=1" type="application/atom+xml;profile=opds-catalog"/>
</feed>)";
  OpdsParser parser;
  parseFeed(xml, parser);
  ASSERT_FALSE(parser.error());
  EXPECT_EQ(parser.getPrevPageUrl(), "/opds/catalog?p=1");
}

// A cover link without type= was dropped, so the catalog showed no cover for the book.
// From witchling-reader commit 111f6ecc (syfq91).
TEST(OpdsParser, CoverLinkWithoutTypeIsKept) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Example Book</title>
    <id>b1</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub"/>
    <link rel="http://opds-spec.org/image" href="/covers/1.jpg"/>
  </entry>
  <entry>
    <title>Typed Cover</title>
    <id>b2</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example2.epub"/>
    <link rel="http://opds-spec.org/image" type="image/png" href="/covers/2.png"/>
  </entry>
</feed>)";
  OpdsParser parser;
  const auto entries = parseFeed(xml, parser);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].imageHref, "/covers/1.jpg");
  EXPECT_EQ(entries[1].imageHref, "/covers/2.png");
}

TEST(OpdsParser, CoverLinkRejectsThumbnailAndNonImageType) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Thumbnail Only</title>
    <id>b3</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example.epub"/>
    <link rel="http://opds-spec.org/image/thumbnail" href="/covers/thumb.jpg"/>
  </entry>
  <entry>
    <title>Non-Image Cover</title>
    <id>b4</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/example2.epub"/>
    <link rel="http://opds-spec.org/image" type="text/html" href="/covers/page.html"/>
  </entry>
</feed>)";
  OpdsParser parser;
  const auto entries = parseFeed(xml, parser);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_TRUE(entries[0].imageHref.empty());
  EXPECT_TRUE(entries[1].imageHref.empty());
}

// An acquisition link with no type= whose href names an .epub was dropped, and the book with it,
// although the file name says what it is.
TEST(OpdsParser, UntypedEpubAcquisitionIsDownloadable) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Untyped</title>
    <id>b5</id>
    <link rel="http://opds-spec.org/acquisition" href="/books/untyped.epub"/>
  </entry>
</feed>)";
  OpdsParser parser;
  const auto entries = parseFeed(xml, parser);
  ASSERT_EQ(entries.size(), 1u);
  ASSERT_TRUE(assertSingleFormat(entries[0], "epub", ".epub"));
  EXPECT_EQ(entries[0].href, "/books/untyped.epub");
  // The mime type decides whether the download gets its cover and a fresh cache.
  EXPECT_EQ(entries[0].acquisitionLinks[0].mimeType, "application/epub+zip");
}

// The generic binary type says no more than none at all; the query string is not part of the name.
TEST(OpdsParser, OctetStreamAcquisitionIsReadFromItsFileName) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Generic Type</title>
    <id>b6</id>
    <link rel="http://opds-spec.org/acquisition/open-access" type="application/octet-stream"
          href="/get/Book.KEPUB.EPUB?token=abc"/>
    <link rel="http://opds-spec.org/acquisition" type="application/octet-stream" href="/get/book.txt#start"/>
    <link rel="http://opds-spec.org/acquisition" href="/get/book.xtc"/>
  </entry>
</feed>)";
  OpdsParser parser;
  const auto entries = parseFeed(xml, parser);
  ASSERT_EQ(entries.size(), 1u);
  const auto& links = entries[0].acquisitionLinks;
  ASSERT_EQ(links.size(), 3u);
  EXPECT_EQ(links[0].formatKey, "kepub");
  EXPECT_EQ(links[0].fileExtension, ".kepub.epub");
  EXPECT_EQ(links[1].formatKey, "txt");
  EXPECT_EQ(links[2].formatKey, "xtc");
}

// A shop or loan page is no download, whatever its address ends in.
TEST(OpdsParser, UntypedBuyLinkIsNotADownload) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>For Sale</title>
    <id>b7</id>
    <link rel="http://opds-spec.org/acquisition/buy" href="https://shop.example/book.epub"/>
    <link rel="http://opds-spec.org/acquisition/borrow" href="https://library.example/book.epub"/>
  </entry>
</feed>)";
  OpdsParser parser;
  const auto entries = parseFeed(xml, parser);
  EXPECT_TRUE(entries.empty());
}

// A file name that is not a format the reader opens says nothing without a type: the reader cannot
// tell what it would download, so the book is one it cannot open.
TEST(OpdsParser, UntypedLinkWithAnUnknownNameIsNotSupported) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Unknown</title>
    <id>b8</id>
    <link rel="http://opds-spec.org/acquisition" href="/get/12345"/>
  </entry>
</feed>)";
  OpdsParser parser;
  const auto entries = parseFeed(xml, parser);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].type, OpdsEntryType::BOOK);
  EXPECT_TRUE(entries[0].acquisitionLinks.empty());
}

// From witchling-reader commit 111f6ecc (syfq91): a book offered only as PDF vanished from the list.
TEST(OpdsParser, UnsupportedOnlyEntryIsListed) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>PDF Only</title>
    <id>b9</id>
    <link rel="http://opds-spec.org/acquisition/open-access" type="application/pdf" href="/books/x.pdf"/>
  </entry>
</feed>)";
  OpdsParser parser;
  const auto entries = parseFeed(xml, parser);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].type, OpdsEntryType::BOOK);
  EXPECT_EQ(entries[0].href, "/books/x.pdf");
  EXPECT_TRUE(entries[0].acquisitionLinks.empty());
}

TEST(OpdsParser, MixedSupportedAndUnsupportedFormatsKeepSupported) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Unsupported First</title>
    <id>m1</id>
    <link rel="http://opds-spec.org/acquisition" type="application/pdf" href="/books/mixed-1.pdf"/>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/mixed-1.epub"/>
  </entry>
  <entry>
    <title>Supported First</title>
    <id>m2</id>
    <link rel="http://opds-spec.org/acquisition" type="application/epub+zip" href="/books/mixed-2.epub"/>
    <link rel="http://opds-spec.org/acquisition" type="application/pdf" href="/books/mixed-2.pdf"/>
  </entry>
</feed>)";
  OpdsParser parser;
  const auto entries = parseFeed(xml, parser);
  ASSERT_EQ(entries.size(), 2u);
  for (const auto& entry : entries) {
    EXPECT_EQ(entry.type, OpdsEntryType::BOOK);
    ASSERT_EQ(entry.acquisitionLinks.size(), 1u);
    EXPECT_EQ(entry.acquisitionLinks[0].formatKey, "epub");
  }
  EXPECT_EQ(entries[0].href, "/books/mixed-1.epub");
  EXPECT_EQ(entries[1].href, "/books/mixed-2.epub");
}

// The regression in the fork's version of the rule above: a series entry that also carries a shop
// link became a book with nothing to download, and the series could no longer be opened.
TEST(OpdsParser, NavigationEntryWithBuyLinkStaysNavigation) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Series X</title>
    <id>n1</id>
    <link rel="subsection" type="application/atom+xml;profile=opds-catalog;kind=acquisition" href="/opds/series/x"/>
    <link rel="http://opds-spec.org/acquisition/buy" type="text/html" href="https://shop.example/x"/>
  </entry>
</feed>)";
  OpdsParser parser;
  const auto entries = parseFeed(xml, parser);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].type, OpdsEntryType::NAVIGATION);
  EXPECT_EQ(entries[0].href, "/opds/series/x");
}

// A navigation link wins over a download the reader cannot open, in either order.
TEST(OpdsParser, NavigationLinkBeatsAnUnsupportedDownload) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Download First</title>
    <id>n2</id>
    <link rel="http://opds-spec.org/acquisition" type="application/pdf" href="/books/n2.pdf"/>
    <link rel="subsection" type="application/atom+xml;profile=opds-catalog;kind=acquisition" href="/opds/n2"/>
  </entry>
  <entry>
    <title>Navigation First</title>
    <id>n3</id>
    <link rel="subsection" type="application/atom+xml;profile=opds-catalog;kind=acquisition" href="/opds/n3"/>
    <link rel="http://opds-spec.org/acquisition" type="application/pdf" href="/books/n3.pdf"/>
  </entry>
</feed>)";
  OpdsParser parser;
  const auto entries = parseFeed(xml, parser);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].type, OpdsEntryType::NAVIGATION);
  EXPECT_EQ(entries[0].href, "/opds/n2");
  EXPECT_EQ(entries[1].type, OpdsEntryType::NAVIGATION);
  EXPECT_EQ(entries[1].href, "/opds/n3");
}

// --- A feed's own search templates (<link rel="search" href="...{searchTerms}...">) ---------------
// The rule of an OpenSearch description's templates (OpenSearch::TemplatePicker, see
// OpenSearchDescriptionTest.cpp for its cases); these check that the feed goes through it.

namespace {
// The feed's search template; osdUrl gets the description it names, if any.
std::string feedSearchTemplate(const std::string& links, std::string* osdUrl = nullptr) {
  const std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
)" + links + R"(
</feed>)";
  OpdsParser parser;
  parseFeed(xml.c_str(), parser);
  EXPECT_FALSE(parser.error());
  if (osdUrl != nullptr) *osdUrl = parser.getOsdUrl();
  return parser.getSearchTemplate();
}
}  // namespace

TEST(OpdsParser, FeedSearchTemplateOfItsOnlyLink) {
  EXPECT_EQ(feedSearchTemplate(R"(<link rel="search" type="application/atom+xml" href="/opds/search/{searchTerms}"/>)"),
            "/opds/search/{searchTerms}");
}

// The last template used to win, whatever its type: here the JSON suggestions.
TEST(OpdsParser, FeedSearchTakesTheAtomTemplate) {
  EXPECT_EQ(feedSearchTemplate(R"(
  <link rel="search" type="text/html" href="/search?q={searchTerms}"/>
  <link rel="search" type="application/atom+xml" href="/opds/search?q={searchTerms}"/>
  <link rel="search" type="application/x-suggestions+json" href="/suggest?q={searchTerms}"/>)"),
            "/opds/search?q={searchTerms}");
}

TEST(OpdsParser, FeedSearchPrefersAnOpdsCatalogFeedInEitherOrder) {
  EXPECT_EQ(feedSearchTemplate(R"(
  <link rel="search" type="application/atom+xml;profile=opds-catalog;kind=acquisition" href="/opds/{searchTerms}"/>
  <link rel="search" type="application/atom+xml" href="/atom/{searchTerms}"/>)"),
            "/opds/{searchTerms}");
  EXPECT_EQ(feedSearchTemplate(R"(
  <link rel="search" type="application/atom+xml" href="/atom/{searchTerms}"/>
  <link rel="search" type="application/atom+xml;profile=opds-catalog;kind=acquisition" href="/opds/{searchTerms}"/>)"),
            "/opds/{searchTerms}");
}

// With no template the browser can read, it has none of its own, and goes to the description the
// feed names (OpdsBookBrowserActivity::fetchFeed() fetches it only while the template is empty).
TEST(OpdsParser, FeedSearchWithOnlyHtmlAndJsonLeavesTheDescription) {
  std::string osdUrl;
  EXPECT_EQ(feedSearchTemplate(R"(
  <link rel="search" type="text/html" href="/search?q={searchTerms}"/>
  <link rel="search" type="application/opensearchdescription+xml" href="/osd.xml"/>
  <link rel="search" type="application/json" href="/api/search?q={searchTerms}"/>)",
                               &osdUrl),
            "");
  EXPECT_EQ(osdUrl, "/osd.xml");
}

TEST(OpdsParser, FeedSearchUntypedIsTheLastResort) {
  EXPECT_EQ(feedSearchTemplate(R"(
  <link rel="search" href="/untyped/{searchTerms}"/>
  <link rel="search" type="text/html" href="/html/{searchTerms}"/>)"),
            "/untyped/{searchTerms}");
  EXPECT_EQ(feedSearchTemplate(R"(
  <link rel="search" type="application/atom+xml" href="/atom/{searchTerms}"/>
  <link rel="search" href="/untyped/{searchTerms}"/>)"),
            "/atom/{searchTerms}");
}

// A shop page alone is not a book the reader cannot open, it is no book at all for the reader.
TEST(OpdsParser, BuyLinkAloneIsNotListed) {
  const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Shop Only</title>
    <id>s1</id>
    <link rel="http://opds-spec.org/acquisition/buy" type="text/html" href="https://shop.example/s1"/>
  </entry>
</feed>)";
  OpdsParser parser;
  const auto entries = parseFeed(xml, parser);
  EXPECT_TRUE(entries.empty());
}
