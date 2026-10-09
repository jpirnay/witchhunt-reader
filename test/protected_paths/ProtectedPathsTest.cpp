// ProtectedPaths decides which card paths the web server and WebDAV refuse. The
// firmware resolves 8.3 aliases on the card; these tests stand a fake in for that
// lookup, so everything else -- segment rules, SdFat's name trimming, the credential
// denylist, the hidden-files opt-in -- runs on the host.

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "ProtectedPaths.h"

using ProtectedPaths::Target;

namespace {

// No entry on the fake card is reached through an alias.
bool noAliases(std::string_view, std::string_view) { return false; }

bool refused(const std::string& path, const Target target = Target::Item, const bool allowHidden = false) {
  return ProtectedPaths::isProtectedPath(path, target, allowHidden, noAliases);
}

}  // namespace

TEST(ProtectedPaths, OrdinaryPathsStayReachable) {
  EXPECT_FALSE(refused("/books/novel.epub"));
  EXPECT_FALSE(refused("/Books/wifi.json"));  // a credential store's NAME, in the wrong place
  EXPECT_FALSE(refused("/", Target::Directory));
  EXPECT_FALSE(refused("/books", Target::Directory));
  EXPECT_FALSE(refused("/books/volume..2.epub"));
}

// The bug this exists for: the old rule judged only "wifi.json".
TEST(ProtectedPaths, EverySegmentIsChecked) {
  EXPECT_TRUE(refused("/.crosspoint/wifi.json"));
  EXPECT_TRUE(refused("/.crosspoint/settings.json"));
  EXPECT_TRUE(refused("/books/.hidden/novel.epub"));
  EXPECT_TRUE(refused("/.crosspoint", Target::Directory));
  EXPECT_TRUE(refused("/System Volume Information/IndexerVolumeGuid"));
  EXPECT_TRUE(refused("/books/XTCache/page.bin"));
}

TEST(ProtectedPaths, SystemFoldersMatchWithoutCase) {
  EXPECT_TRUE(refused("/system volume information"));
  EXPECT_TRUE(refused("/xtcache", Target::Directory));
}

// With hidden files on, dot folders may be walked through, but a dot-named item
// cannot be acted on -- which is what keeps /.crosspoint itself in place.
TEST(ProtectedPaths, HiddenFilesOptInOpensDotFoldersNotDotItems) {
  EXPECT_FALSE(refused("/.crosspoint", Target::Directory, true));
  EXPECT_FALSE(refused("/.crosspoint/settings.json", Target::Item, true));
  EXPECT_FALSE(refused("/.crosspoint/epub_1234/progress.bin", Target::Item, true));
  EXPECT_TRUE(refused("/.crosspoint", Target::Item, true));
  EXPECT_TRUE(refused("/books/.DS_Store", Target::Item, true));
  EXPECT_TRUE(refused("/System Volume Information", Target::Directory, true));
}

TEST(ProtectedPaths, CredentialStoresAreNeverReachable) {
  for (const bool allowHidden : {false, true}) {
    EXPECT_TRUE(refused("/.crosspoint/wifi.json", Target::Item, allowHidden));
    EXPECT_TRUE(refused("/.crosspoint/opds.json", Target::Item, allowHidden));
    EXPECT_TRUE(refused("/.crosspoint/koreader.json", Target::Item, allowHidden));
    EXPECT_TRUE(refused("/.crosspoint/wifi.json.tmp", Target::Item, allowHidden));
    EXPECT_TRUE(refused("/.CrossPoint/WIFI.JSON", Target::Item, allowHidden));
  }
}

// SdFat drops leading spaces and trailing dots and spaces before it opens a name.
TEST(ProtectedPaths, NamesAreJudgedAsSdFatOpensThem) {
  EXPECT_TRUE(refused("/ .crosspoint/settings.json"));
  EXPECT_TRUE(refused("/.crosspoint./settings.json"));
  EXPECT_TRUE(refused("/books/ .hidden"));
  EXPECT_TRUE(refused("/.crosspoint/wifi.json.", Target::Item, true));
  EXPECT_TRUE(refused("/ .crosspoint/ wifi.json", Target::Item, true));
  EXPECT_TRUE(refused("/.crosspoint ./opds.json", Target::Item, true));
  EXPECT_TRUE(refused("/System Volume Information. "));
}

// "/.crosspoint/ " opens "/.crosspoint" itself: the trailing segment names nothing,
// so the item rule must judge ".crosspoint".
TEST(ProtectedPaths, TrailingEmptySegmentDoesNotHideTheItem) {
  EXPECT_TRUE(refused("/.crosspoint/ ", Target::Item, true));
  EXPECT_TRUE(refused("/.crosspoint/...", Target::Item, true));
}

TEST(ProtectedPaths, AliasShapes) {
  using ProtectedPaths::looksLikeShortAlias;
  EXPECT_TRUE(looksLikeShortAlias("CROSSP~1"));
  EXPECT_TRUE(looksLikeShortAlias("WIFI~1.JSO"));
  EXPECT_TRUE(looksLikeShortAlias("a~9.b"));
  EXPECT_FALSE(looksLikeShortAlias("a~b.epub"));         // no digit after the tilde
  EXPECT_FALSE(looksLikeShortAlias("LONGNAME1~1.TXT"));  // base longer than 8
  EXPECT_FALSE(looksLikeShortAlias("NAME~1.EPUB"));      // extension longer than 3
  EXPECT_FALSE(looksLikeShortAlias("A~1.B.C"));          // two dots
  EXPECT_FALSE(looksLikeShortAlias("novel.epub"));
}

// The request names an entry by its alias: refused, wherever it points.
TEST(ProtectedPaths, AliasSpellingIsRefused) {
  std::vector<std::string> asked;
  auto aliasOfCrosspoint = [&asked](const std::string_view prefix, const std::string_view name) {
    asked.emplace_back(prefix);
    return name == "CROSSP~1";
  };
  EXPECT_TRUE(ProtectedPaths::isProtectedPath("/CROSSP~1/wifi.json", Target::Item, true, aliasOfCrosspoint));
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0], "/CROSSP~1");

  asked.clear();
  EXPECT_TRUE(ProtectedPaths::isProtectedPath("/books/CROSSP~1", Target::Directory, false, aliasOfCrosspoint));
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0], "/books/CROSSP~1");
}

// A file genuinely named like an alias is its own long name: the lookup says so, and
// the path stays reachable. Ordinary names never reach the lookup at all.
TEST(ProtectedPaths, RealAliasShapedNamesStayReachable) {
  int lookups = 0;
  auto realName = [&lookups](std::string_view, std::string_view) {
    lookups++;
    return false;
  };
  EXPECT_FALSE(ProtectedPaths::isProtectedPath("/photos/IMG~1.JPG", Target::Item, false, realName));
  EXPECT_EQ(lookups, 1);
  EXPECT_FALSE(ProtectedPaths::isProtectedPath("/books/novel.epub", Target::Item, false, realName));
  EXPECT_EQ(lookups, 1);
}
