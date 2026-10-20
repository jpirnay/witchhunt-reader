// The search template the OPDS browser takes from a catalog's OpenSearch description
// (lib/OpdsParser/OpenSearchDescription.h).
#include <OpenSearchDescription.h>
#include <gtest/gtest.h>

#include <string>

namespace {
std::string templateOf(const std::string& xml) { return OpenSearch::searchTemplate(xml.data(), xml.size()); }

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
