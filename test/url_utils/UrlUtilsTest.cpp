#include <gtest/gtest.h>

#include <string>

#include "../../src/util/UrlUtils.h"

TEST(UrlUtils, AbsoluteUrlRemainsUnchanged) {
  ASSERT_EQ(
      UrlUtils::buildUrl("https://catalog.example.com/opds/feed.xml", "https://example.com/books/download/test.epub"),
      "https://example.com/books/download/test.epub");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/feed.xml", "//cdn.example.com/books/test.epub"),
            "https://cdn.example.com/books/test.epub");
}

TEST(UrlUtils, RootRelativeUrlUsesHostRoot) {
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/feed.xml", "/books/download/test.epub"),
            "https://catalog.example.com/books/download/test.epub");
}

TEST(UrlUtils, RelativeUrlUsesFeedDirectory) {
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/feed.xml", "download/test.epub"),
            "https://catalog.example.com/opds/download/test.epub");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/sub", "download/test.epub"),
            "https://catalog.example.com/opds/download/test.epub");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/sub/", "download/test.epub"),
            "https://catalog.example.com/opds/sub/download/test.epub");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/feed.xml", "images//cover.jpg"),
            "https://catalog.example.com/opds/images//cover.jpg");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/feed.xml", "subdir/"),
            "https://catalog.example.com/opds/subdir/");
}

TEST(UrlUtils, ParentRelativeUrlResolvesCorrectly) {
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/sub/feed.xml", "../download/test.epub"),
            "https://catalog.example.com/opds/download/test.epub");
}

TEST(UrlUtils, UrlsWithQueryAndFragmentResolveCorrectly) {
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/sub?auth=1", "download/test.epub"),
            "https://catalog.example.com/opds/download/test.epub");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/sub/#section", "download/test.epub?format=epub#start"),
            "https://catalog.example.com/opds/sub/download/test.epub?format=epub#start");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/feed.xml?auth=1#top", "download/test.epub"),
            "https://catalog.example.com/opds/download/test.epub");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/feed.xml", "/books?id=/../cover"),
            "https://catalog.example.com/books?id=/../cover");
  ASSERT_EQ(
      UrlUtils::buildUrl("https://catalog.example.com/opds/feed.xml", "download/test.epub?next=/../cover#frag/../x"),
      "https://catalog.example.com/opds/download/test.epub?next=/../cover#frag/../x");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/feed.xml", "download/test.epub#frag?x=1"),
            "https://catalog.example.com/opds/download/test.epub#frag?x=1");
}

TEST(UrlUtils, RootRelativeUrlsWithQueryAndFragmentResolveCorrectly) {
  ASSERT_EQ(
      UrlUtils::buildUrl("https://catalog.example.com/opds/feed.xml?auth=1", "/books/download/test.epub?format=epub"),
      "https://catalog.example.com/books/download/test.epub?format=epub");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/sub/#section", "/books/download/test.epub#start"),
            "https://catalog.example.com/books/download/test.epub#start");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/sub?auth=1#top",
                               "/books/download/test.epub?format=epub#start"),
            "https://catalog.example.com/books/download/test.epub?format=epub#start");
}

TEST(UrlUtils, HostOnlyBaseUrlsWithQueryAndFragmentResolveCorrectly) {
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com?auth=1", "download/test.epub"),
            "https://catalog.example.com/download/test.epub");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com#top", "/books/download/test.epub"),
            "https://catalog.example.com/books/download/test.epub");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com?auth=1", "?page=2"), "https://catalog.example.com?page=2");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com#top", "#latest"), "https://catalog.example.com#latest");
}

TEST(UrlUtils, QueryAndFragmentReferencesAreRfcCompliant) {
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/feed.xml?auth=1", "#top"),
            "https://catalog.example.com/opds/feed.xml?auth=1#top");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com/opds/feed.xml?auth=1#old", "?page=2"),
            "https://catalog.example.com/opds/feed.xml?page=2");
  ASSERT_EQ(UrlUtils::buildUrl("https://catalog.example.com?auth=1#old", "#latest"),
            "https://catalog.example.com?auth=1#latest");
}

TEST(UrlUtils, ExtractHostnameHandlesPortsAndIpv6) {
  ASSERT_EQ(UrlUtils::extractHostname("https://catalog.example.com:8080/path"), "catalog.example.com");
  ASSERT_EQ(UrlUtils::extractHostname("catalog.example.com:8080/opds"), "catalog.example.com");
  ASSERT_EQ(UrlUtils::extractHostname("https://user:pass@[2001:db8::1]:8080/path?x=1#frag"), "2001:db8::1");
  ASSERT_EQ(UrlUtils::extractHostname("http://[fe80::1234]#top"), "fe80::1234");
}

TEST(UrlUtils, ExtractHostnameRejectsMalformedInputs) {
  ASSERT_EQ(UrlUtils::extractHostname("https:///path"), "");
  ASSERT_EQ(UrlUtils::extractHostname("http://[]"), "");
  ASSERT_EQ(UrlUtils::extractHostname("http://@/x"), "");
  ASSERT_EQ(UrlUtils::extractHostname("http://[::1"), "");
  ASSERT_EQ(UrlUtils::extractHostname("http://[::1]x"), "");
  ASSERT_EQ(UrlUtils::extractHostname("mailto:user@example.com"), "");
  ASSERT_EQ(UrlUtils::extractHostname("urn:isbn:1234567890"), "");
}

TEST(UrlUtils, RepairsMistypedSchemeSeparator) {
  // The failure this guards: a hand-typed "https:/host" has no "://", so every
  // scheme check treats it as scheme-less and prepends a second scheme.
  ASSERT_EQ(UrlUtils::repairSchemeSeparator("https:/kosync.example.com"), "https://kosync.example.com");
  ASSERT_EQ(UrlUtils::repairSchemeSeparator("http:/example.com:8080/path"), "http://example.com:8080/path");
  ASSERT_EQ(UrlUtils::repairSchemeSeparator("https:example.com"), "https://example.com");
  ASSERT_EQ(UrlUtils::repairSchemeSeparator("https:///example.com"), "https://example.com");
  ASSERT_EQ(UrlUtils::repairSchemeSeparator("HTTPS:/example.com"), "https://example.com");
}

TEST(UrlUtils, CollapsesDoubledScheme) {
  ASSERT_EQ(UrlUtils::repairSchemeSeparator("http://https://example.com"), "https://example.com");
  ASSERT_EQ(UrlUtils::repairSchemeSeparator("http://https:/example.com/path"), "https://example.com/path");
}

TEST(UrlUtils, LeavesWellFormedAndUnrelatedUrlsUnchanged) {
  ASSERT_EQ(UrlUtils::repairSchemeSeparator("https://example.com/path"), "https://example.com/path");
  ASSERT_EQ(UrlUtils::repairSchemeSeparator("http://192.168.0.5:8080"), "http://192.168.0.5:8080");
  ASSERT_EQ(UrlUtils::repairSchemeSeparator("example.com"), "example.com");
  // host:port without a scheme must not be mistaken for a scheme separator
  ASSERT_EQ(UrlUtils::repairSchemeSeparator("localhost:8080"), "localhost:8080");
  ASSERT_EQ(UrlUtils::repairSchemeSeparator("ftp:/example.com"), "ftp:/example.com");
  ASSERT_EQ(UrlUtils::repairSchemeSeparator("https://"), "https://");
  ASSERT_EQ(UrlUtils::repairSchemeSeparator("https:"), "https:");
  ASSERT_EQ(UrlUtils::repairSchemeSeparator(""), "");
}

TEST(UrlUtils, EnsureProtocolRepairsBeforeItPrepends) {
  ASSERT_EQ(UrlUtils::ensureProtocol("example.com/feed"), "http://example.com/feed");
  // A mistyped scheme must be repaired, never treated as scheme-less.
  ASSERT_EQ(UrlUtils::ensureProtocol("https:/example.com"), "https://example.com");
  ASSERT_EQ(UrlUtils::ensureProtocol("http:/example.com"), "http://example.com");
  ASSERT_EQ(UrlUtils::ensureProtocol("https://example.com"), "https://example.com");
}
