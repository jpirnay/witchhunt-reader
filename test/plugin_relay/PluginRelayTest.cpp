#include <gtest/gtest.h>

#include <string>

#include "network/PluginRelay.h"
#include "util/PluginLocations.h"

using PluginRelay::HeaderList;

TEST(PluginRelayTest, MethodsAreHttpsAndWebDavsOwn) {
  for (const char* m :
       {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "PROPFIND", "MKCOL", "MOVE", "COPY"}) {
    EXPECT_TRUE(PluginRelay::methodAllowed(m)) << m;
  }
  // A tunnel, a reflected request, lower case (the handler upper-cases first) and anything made up are refused.
  for (const char* m : {"CONNECT", "TRACE", "get", "", "GET ", "BREW"}) {
    EXPECT_FALSE(PluginRelay::methodAllowed(m)) << m;
  }
}

TEST(PluginRelayTest, PluginHeadersCannotReframeTheRequest) {
  EXPECT_TRUE(PluginRelay::requestHeaderAllowed("Authorization", "Bearer abc"));
  EXPECT_TRUE(PluginRelay::requestHeaderAllowed("Content-Type", "application/json; charset=utf-8"));
  EXPECT_TRUE(PluginRelay::requestHeaderAllowed("X-Api-Key", "k\tv"));  // a tab is allowed in a value
  // The client writes these itself; a second one would make the request say something else than what is sent.
  for (const char* name : {"Host", "content-length", "Connection", "Transfer-Encoding", "Expect", "Upgrade"}) {
    EXPECT_FALSE(PluginRelay::requestHeaderAllowed(name, "x")) << name;
  }
  // Nothing may end the header line and start another.
  EXPECT_FALSE(PluginRelay::requestHeaderAllowed("X-Evil", "a\r\nHost: other"));
  EXPECT_FALSE(PluginRelay::requestHeaderAllowed("X-Evil", std::string("a\0b", 3)));
  EXPECT_FALSE(PluginRelay::requestHeaderAllowed("X Evil", "a"));
  EXPECT_FALSE(PluginRelay::requestHeaderAllowed("X-Evil:", "a"));
  EXPECT_FALSE(PluginRelay::requestHeaderAllowed("", "a"));
}

TEST(PluginRelayTest, HeaderListIsJsonPairsInOrder) {
  HeaderList list(4096);
  list.add("Content-Type", "text/html");
  list.add("Set-Cookie", "a=1");
  list.add("Set-Cookie", "b=2");
  EXPECT_EQ(list.json(), R"([["Content-Type","text/html"],["Set-Cookie","a=1"],["Set-Cookie","b=2"]])");
  EXPECT_EQ(HeaderList(64).json(), "[]");
}

TEST(PluginRelayTest, HeaderListEscapesWhatWouldBreakTheJsonOrTheHeader) {
  HeaderList list(4096);
  list.add("X-Quote", "say \"hi\" \\ there");
  list.add("X-Ctl", std::string("a\r\nb\x01", 5));
  EXPECT_EQ(list.json(), R"([["X-Quote","say \"hi\" \\ there"],["X-Ctl","a\u000d\u000ab\u0001"]])");
  // UTF-8 passes through as it is: the page decodes it.
  HeaderList utf8(4096);
  utf8.add("X-Title", "Ünï");
  EXPECT_EQ(utf8.json(), "[[\"X-Title\",\"Ünï\"]]");
}

TEST(PluginRelayTest, HeaderListStaysUnderItsCap) {
  HeaderList list(40);
  list.add("A", "1");                     // [["A","1"]] = 11 bytes
  list.add("Big", std::string(64, 'x'));  // would not fit: left out
  list.add("B", "2");                     // [["A","1"],["B","2"]] = 21 bytes
  EXPECT_EQ(list.json(), R"([["A","1"],["B","2"]])");
  EXPECT_EQ(list.dropped(), 1u);
  EXPECT_LE(list.json().size(), 40u);
}

TEST(PluginRelayTest, PluginFoldersAreThoseUnderTheThreeRoots) {
  EXPECT_TRUE(PluginLocations::isInsidePluginFolder("/.crosspoint/plugins/wallabag/token.json"));
  EXPECT_TRUE(PluginLocations::isInsidePluginFolder("/plugins/hello/plugin.js"));
  EXPECT_TRUE(PluginLocations::isInsidePluginFolder("/.plugins/x/sub/file.txt"));
  // The folder itself, the root, a sibling of the root and the firmware's own files are not.
  EXPECT_FALSE(PluginLocations::isInsidePluginFolder("/.crosspoint/plugins/wallabag"));
  EXPECT_FALSE(PluginLocations::isInsidePluginFolder("/.crosspoint/plugins/wallabag/"));
  EXPECT_FALSE(PluginLocations::isInsidePluginFolder("/.crosspoint/plugins/"));
  EXPECT_FALSE(PluginLocations::isInsidePluginFolder("/.crosspoint/plugins"));
  EXPECT_FALSE(PluginLocations::isInsidePluginFolder("/.crosspoint/plugins-old/x/y"));
  EXPECT_FALSE(PluginLocations::isInsidePluginFolder("/.crosspoint/wifi.json"));
  EXPECT_FALSE(PluginLocations::isInsidePluginFolder("/.crosspoint/settings.json"));
  EXPECT_FALSE(PluginLocations::isInsidePluginFolder("/pluginsx/a/b"));
}
