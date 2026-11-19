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

TEST(PluginRelayTest, FetchFailureNamesTheServersAnswer) {
  // A missing file, a third party's block and a server error each say what the server answered.
  EXPECT_EQ(PluginRelay::describeFetchFailure(404).error, "the server answered 404");
  EXPECT_EQ(PluginRelay::describeFetchFailure(451).error, "the server answered 451");
  EXPECT_EQ(PluginRelay::describeFetchFailure(503).error, "the server answered 503");
  // A redirect the download did not follow is an answer too.
  EXPECT_EQ(PluginRelay::describeFetchFailure(302).error, "the server answered 302");
  EXPECT_FALSE(PluginRelay::describeFetchFailure(404).unauthorized);
  // 401 and 403 are marked apart: a catalog with a password grant then signs in again.
  for (const int status : {401, 403}) {
    const PluginRelay::FetchFailure f = PluginRelay::describeFetchFailure(status);
    EXPECT_TRUE(f.unauthorized) << status;
    EXPECT_EQ(f.error, "the server refused the request (" + std::to_string(status) + ")");
  }
  // No answer at all (a transport or TLS error), or a 2xx whose file could not be written: nothing to name.
  for (const int status : {0, -1, -11, 200, 206}) {
    const PluginRelay::FetchFailure f = PluginRelay::describeFetchFailure(status);
    EXPECT_EQ(f.error, "download failed") << status;
    EXPECT_FALSE(f.unauthorized) << status;
  }
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

// The files a plugin's device.json names as its own (token.file, config.file): what the web server lets it read and
// write outside its folder.
TEST(PluginRelayTest, DeviceJsonNamesItsOwnFiles) {
  const std::string dir = "/.crosspoint/plugins/lingo";
  EXPECT_TRUE(PluginLocations::namesPath("token.json", dir, "/.crosspoint/plugins/lingo/token.json"));
  EXPECT_TRUE(PluginLocations::namesPath("/.crosspoint/lingoblend.json", dir, "/.crosspoint/lingoblend.json"));
  EXPECT_TRUE(PluginLocations::namesPath("/.crosspoint/lingoblend.json", dir, "/.CrossPoint/LingoBlend.json"))
      << "FAT names compare without case";
  EXPECT_TRUE(PluginLocations::namesPath("//.crosspoint/./lingoblend.json", dir, "/.crosspoint/lingoblend.json"))
      << "the name is normalised as a request path is";
  EXPECT_FALSE(PluginLocations::namesPath("/.crosspoint/lingoblend.json", dir, "/.crosspoint/settings.json"));
  EXPECT_FALSE(PluginLocations::namesPath("", dir, "/.crosspoint/plugins/lingo"));
  EXPECT_FALSE(PluginLocations::namesPath("../other/token.json", dir, "/.crosspoint/plugins/other/token.json"))
      << "a name with .. climbs out of the folder: it names nothing";
  EXPECT_FALSE(PluginLocations::namesPath("/.crosspoint/x/../wifi.json", dir, "/.crosspoint/wifi.json"));
}
