// The search template the OPDS browser takes from a catalog's OpenSearch description
// (lib/OpdsParser/OpenSearchDescription.h).
#include <OpenSearchDescription.h>
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {
std::string templateOf(const std::string& xml) { return OpenSearch::searchTemplate(xml.data(), xml.size()); }

std::string fixture(const char* name) {
  std::ifstream in(std::string(OPDS_FIXTURE_DIR) + "/" + name, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string descriptionWith(const std::string& urls) {
  return R"(<?xml version="1.0" encoding="UTF-8"?>
<OpenSearchDescription xmlns="http://a9.com/-/spec/opensearch/1.1/">
  <ShortName>Catalog</ShortName>
)" + urls +
         R"(
</OpenSearchDescription>)";
}
}  // namespace

TEST(OpenSearchDescription, TemplateOfItsOnlyUrl) {
  EXPECT_EQ(templateOf(descriptionWith(R"(<Url type="application/atom+xml" template="/opds/search/{searchTerms}"/>)")),
            "/opds/search/{searchTerms}");
}

TEST(OpenSearchDescription, PrefixedUrlElement) {
  EXPECT_EQ(templateOf(R"(<?xml version="1.0"?>
<os:OpenSearchDescription xmlns:os="http://a9.com/-/spec/opensearch/1.1/">
  <os:Url type="application/atom+xml" template="https://example.org/search?q={searchTerms}"/>
</os:OpenSearchDescription>)"),
            "https://example.org/search?q={searchTerms}");
}

TEST(OpenSearchDescription, TemplateWithoutSearchTermsIsNoTemplate) {
  EXPECT_EQ(templateOf(descriptionWith(R"(<Url type="application/atom+xml" template="/opds/new"/>)")), "");
}

TEST(OpenSearchDescription, NoUrlNoTemplate) {
  EXPECT_EQ(templateOf(descriptionWith("")), "");
  EXPECT_EQ(templateOf(""), "");
}

// A description cut off after its <Url> keeps the template read before the parse error.
TEST(OpenSearchDescription, TruncatedDescriptionKeepsWhatItRead) {
  const std::string xml =
      descriptionWith(R"(<Url type="application/atom+xml" template="/opds/search/{searchTerms}"/>)");
  EXPECT_EQ(templateOf(xml.substr(0, xml.find("</OpenSearchDescription>"))), "/opds/search/{searchTerms}");
}

// Project Gutenberg's description, as served at https://www.gutenberg.org/catalog/osd-books.xml (the
// rel="search" link of https://m.gutenberg.org/ebooks.opds/) on 2026-10-20. It lists an HTML page,
// the Atom feed, and last the JSON suggestions, which the browser used to search: /ebooks/suggest/
// answers a 404, so every search ended in "Failed to fetch feed".
TEST(OpenSearchDescription, GutenbergSearchesItsAtomFeed) {
  const std::string xml = fixture("gutenberg-osd-books.xml");
  ASSERT_FALSE(xml.empty());
  EXPECT_EQ(templateOf(xml), "http://m.gutenberg.org/ebooks/search.opds/?query={searchTerms}");
}

TEST(OpenSearchDescription, AnOpdsCatalogFeedBeatsAnyOtherAtomFeed) {
  EXPECT_EQ(templateOf(descriptionWith(R"(
  <Url type="application/atom+xml;profile=opds-catalog" template="/opds/search/{searchTerms}"/>
  <Url type="application/atom+xml" template="/atom/search/{searchTerms}"/>)")),
            "/opds/search/{searchTerms}");
  EXPECT_EQ(templateOf(descriptionWith(R"(
  <Url type="application/atom+xml" template="/atom/search/{searchTerms}"/>
  <Url type="application/atom+xml;profile=opds-catalog" template="/opds/search/{searchTerms}"/>)")),
            "/opds/search/{searchTerms}");
}

TEST(OpenSearchDescription, TheFirstOfTheBestWins) {
  EXPECT_EQ(templateOf(descriptionWith(R"(
  <Url type="application/atom+xml" template="/first/{searchTerms}"/>
  <Url type="application/atom+xml" template="/second/{searchTerms}"/>)")),
            "/first/{searchTerms}");
  EXPECT_EQ(templateOf(descriptionWith(R"(
  <Url type="application/atom+xml;profile=opds-catalog;kind=acquisition" template="/first/{searchTerms}"/>
  <Url type="application/atom+xml;profile=opds-catalog" template="/second/{searchTerms}"/>)")),
            "/first/{searchTerms}");
}

TEST(OpenSearchDescription, TheMediaTypeIgnoresCaseSpacesAndQuotes) {
  EXPECT_EQ(templateOf(descriptionWith(R"(
  <Url type=" Application/Atom+XML ; kind=acquisition ; Profile=&quot;OPDS-Catalog&quot; " template="/opds/{searchTerms}"/>
  <Url type="application/atom+xml" template="/atom/{searchTerms}"/>)")),
            "/opds/{searchTerms}");
  EXPECT_EQ(templateOf(descriptionWith(R"(
  <Url type="APPLICATION/ATOM+XML" template="/atom/{searchTerms}"/>
  <Url template="/untyped/{searchTerms}"/>)")),
            "/atom/{searchTerms}");
}

// The browser reads Atom feeds only: a search with any of these could never show a result.
TEST(OpenSearchDescription, NoTemplateForAnythingButAtom) {
  EXPECT_EQ(templateOf(descriptionWith(R"(
  <Url type="text/html" template="/search?q={searchTerms}"/>
  <Url type="application/x-suggestions+json" rel="suggestions" template="/suggest?q={searchTerms}"/>
  <Url type="application/json" template="/api/search?q={searchTerms}"/>
  <Url type="application/opds+json" template="/opds2/search{?query}&amp;q={searchTerms}"/>
  <Url type="application/rss+xml" template="/rss?q={searchTerms}"/>
  <Url type="application/atom+xmlx" template="/atomx?q={searchTerms}"/>)")),
            "");
}

// rel says what the template is for; only "results" (the default) is a search.
TEST(OpenSearchDescription, ARelOtherThanResultsIsNeverUsed) {
  EXPECT_EQ(templateOf(descriptionWith(R"(
  <Url type="application/atom+xml" rel="suggestions" template="/suggest/{searchTerms}"/>
  <Url type="application/atom+xml" rel="self" template="/osd.xml?q={searchTerms}"/>)")),
            "");
  EXPECT_EQ(templateOf(descriptionWith(R"(
  <Url type="application/atom+xml" rel="suggestions" template="/suggest/{searchTerms}"/>
  <Url type="application/atom+xml" rel="results" template="/search/{searchTerms}"/>)")),
            "/search/{searchTerms}");
}

// OpenSearch requires the type, and every template was taken before this rule, so one without a type
// is still used, but only when there is no Atom one.
TEST(OpenSearchDescription, AnUntypedTemplateIsTheLastResort) {
  EXPECT_EQ(templateOf(descriptionWith(R"(
  <Url type="text/html" template="/html/{searchTerms}"/>
  <Url template="/untyped/{searchTerms}"/>
  <Url type="" template="/empty-type/{searchTerms}"/>)")),
            "/untyped/{searchTerms}");
  EXPECT_EQ(templateOf(descriptionWith(R"(
  <Url template="/untyped/{searchTerms}"/>
  <Url type="application/atom+xml" template="/atom/{searchTerms}"/>)")),
            "/atom/{searchTerms}");
}
