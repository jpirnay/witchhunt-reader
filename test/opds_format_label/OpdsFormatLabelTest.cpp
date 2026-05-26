#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../../src/activities/browser/OpdsFormatLabel.h"

namespace {
OpdsAcquisitionLink makeLink(const char* href, const char* formatKey) {
  return OpdsAcquisitionLink{href, "application/epub+zip", formatKey, ".epub"};
}
}  // namespace

TEST(OpdsFormatLabel, UniqueFormatUsesBaseLabel) {
  const auto link = makeLink("/books/example.epub", "epub");
  const std::vector<OpdsAcquisitionLink> links{link};

  ASSERT_EQ(opdsFormatSelectionLabel(link, links, "catalog.example.com"), "EPUB");
}

TEST(OpdsFormatLabel, DuplicateAbsoluteUrlsIncludeHostname) {
  const auto primary = makeLink("https://mirror-a.example.com/books/example.epub", "epub");
  const auto secondary = makeLink("https://mirror-b.example.com/books/example.epub", "epub");
  const std::vector<OpdsAcquisitionLink> links{primary, secondary};

  ASSERT_EQ(opdsFormatSelectionLabel(primary, links, "catalog.example.com"), "EPUB - mirror-a.example.com");
  ASSERT_EQ(opdsFormatSelectionLabel(secondary, links, "catalog.example.com"), "EPUB - mirror-b.example.com");
}

TEST(OpdsFormatLabel, DuplicateRootRelativeUrlsUseServerHostname) {
  const auto primary = makeLink("/opds/download/1/epub", "epub");
  const auto secondary = makeLink("/opds/download/2/epub", "epub");
  const std::vector<OpdsAcquisitionLink> links{primary, secondary};

  ASSERT_EQ(opdsFormatSelectionLabel(primary, links, "https://catalog.example.com/opds"),
            "EPUB - catalog.example.com (1)");
  ASSERT_EQ(opdsFormatSelectionLabel(secondary, links, "https://catalog.example.com/opds"),
            "EPUB - catalog.example.com (2)");
}

TEST(OpdsFormatLabel, DuplicateRelativeUrlsUseServerHostname) {
  const auto primary = makeLink("download/1.epub", "epub");
  const auto secondary = makeLink("download/2.epub", "epub");
  const std::vector<OpdsAcquisitionLink> links{primary, secondary};

  ASSERT_EQ(opdsFormatSelectionLabel(primary, links, "catalog.example.com/opds"), "EPUB - catalog.example.com (1)");
  ASSERT_EQ(opdsFormatSelectionLabel(secondary, links, "catalog.example.com/opds"), "EPUB - catalog.example.com (2)");
}

TEST(OpdsFormatLabel, DuplicateAbsoluteUrlsSameHostnameIncludeNumbering) {
  const auto primary = makeLink("https://mirror.example.com/books/example.epub", "epub");
  const auto secondary = makeLink("https://mirror.example.com/books/example-copy.epub", "epub");
  const std::vector<OpdsAcquisitionLink> links{primary, secondary};

  ASSERT_EQ(opdsFormatSelectionLabel(primary, links, "catalog.example.com"), "EPUB - mirror.example.com (1)");
  ASSERT_EQ(opdsFormatSelectionLabel(secondary, links, "catalog.example.com"), "EPUB - mirror.example.com (2)");
}

TEST(OpdsFormatLabel, BatchLabelBuilderMatchesPerLinkLabels) {
  const auto first = makeLink("https://mirror.example.com/books/example.epub", "epub");
  const auto second = makeLink("https://mirror.example.com/books/example-copy.epub", "epub");
  const auto third = makeLink("/books/example.txt", "txt");
  const std::vector<OpdsAcquisitionLink> links{first, second, third};

  const auto labels = buildOpdsFormatSelectionLabels(links, "https://catalog.example.com/opds");
  ASSERT_EQ(labels.size(), static_cast<size_t>(3));
  ASSERT_EQ(labels[0], opdsFormatSelectionLabel(first, links, "https://catalog.example.com/opds"));
  ASSERT_EQ(labels[1], opdsFormatSelectionLabel(second, links, "https://catalog.example.com/opds"));
  ASSERT_EQ(labels[2], opdsFormatSelectionLabel(third, links, "https://catalog.example.com/opds"));
}
