// WebDAVHandler on an in-memory FAT card (stubs/HalStorage.h): the requests a desktop client sends, and what they
// may reach. The handler, the path guard (WebPathGuard, ProtectedPaths), the book-cache helper (WebBookCache),
// FileReplace and FsHelpers are the firmware's own; only the framework's WebServer, the card and the book caches
// are stand-ins.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "BookCacheLog.h"
#include "BookRelocation.h"
#include "HalStorage.h"
#include "HttpFileStreamer.h"
#include "WebDAVHandler.h"
#include "WebServer.h"

namespace {

std::string& streamed() {
  static std::string bytes;
  return bytes;
}

}  // namespace

// What a GET sends: the file's bytes, collected for the test.
bool HttpFileStreamer::streamFileToClient(FsFile& file, NetworkClient&) {
  char buffer[512];
  int got = 0;
  while ((got = file.read(buffer, sizeof(buffer))) > 0) streamed().append(buffer, static_cast<size_t>(got));
  return got == 0;
}

namespace {

class WebDAVHandlerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    clearedBookCaches().clear();
    BookRelocation::movesSeen().clear();
    streamed().clear();

    // The firmware makes /.crosspoint, so SdFat gives it the alias: CROSSP~1, or ~HHHH when that is taken.
    Storage.putFolder("/.crosspoint");
    Storage.alias("/CROSSP~1", "/.crosspoint");
    Storage.put("/.crosspoint/wifi.json", "{\"ssid\":\"home\",\"password\":\"obfuscated\"}");
    Storage.put("/.crosspoint/opds.json", "[]");
    Storage.put("/.crosspoint/koreader.json", "{}");
    Storage.putFolder("/.crosspoint/epub_1234");
    Storage.putFolder("/System Volume Information");
    Storage.putFolder("/Books");
    Storage.put("/Books/novel.epub", "novel bytes");
    Storage.put("/Books/.hidden.epub", "hidden");
  }

  WebServer& request(const HTTPMethod method, const char* uri, const char* depth = nullptr,
                     const char* destination = nullptr) {
    server.setRequest(method, uri);
    if (depth) server.setHeader("Depth", depth);
    if (destination) server.setHeader("Destination", destination);
    handler.handle(server, method, server.uri());
    return server;
  }

  static bool lists(const WebServer& s, const char* href) {
    return s.body.find(std::string("<D:href>") + href + "</D:href>") != std::string::npos;
  }

  WebDAVHandler handler;
  WebServer server;
};

// ── PROPFIND ────────────────────────────────────────────────────────────────

TEST_F(WebDAVHandlerTest, RootListingLeavesProtectedEntriesOut) {
  const WebServer& s = request(HTTP_PROPFIND, "/", "1");
  EXPECT_EQ(s.code, 207);
  EXPECT_TRUE(lists(s, "/"));
  EXPECT_TRUE(lists(s, "/Books/"));
  EXPECT_EQ(s.body.find("crosspoint"), std::string::npos);
  EXPECT_EQ(s.body.find("System"), std::string::npos);
}

TEST_F(WebDAVHandlerTest, FolderListingNamesItsBooks) {
  const WebServer& s = request(HTTP_PROPFIND, "/Books", "1");
  EXPECT_EQ(s.code, 207);
  EXPECT_TRUE(lists(s, "/Books/"));
  EXPECT_TRUE(lists(s, "/Books/novel.epub"));
  EXPECT_NE(s.body.find("<D:getcontentlength>11</D:getcontentlength>"), std::string::npos);
  EXPECT_EQ(s.body.find("hidden"), std::string::npos);
}

// PROPFIND judged no path at all: a listing of /.crosspoint, under any spelling SdFat opens it by, named the
// credential stores and gave their sizes. It answers as the other methods do.
TEST_F(WebDAVHandlerTest, PropfindRefusesProtectedFolders) {
  for (const char* uri : {"/.crosspoint", "/CROSSP~1", "/crossp~1", "/%20.crosspoint", "/.crosspoint.",
                          "/System%20Volume%20Information"}) {
    for (const char* depth : {"0", "1"}) {
      const WebServer& s = request(HTTP_PROPFIND, uri, depth);
      EXPECT_EQ(s.code, 403) << uri << " Depth " << depth;
      EXPECT_EQ(s.body.find("wifi.json"), std::string::npos) << uri << " Depth " << depth;
    }
  }
}

TEST_F(WebDAVHandlerTest, PropfindRefusesProtectedItems) {
  for (const char* uri :
       {"/.crosspoint/wifi.json", "/CROSSP~1/wifi.json", "/.crosspoint/epub_1234", "/Books/.hidden.epub"}) {
    const WebServer& s = request(HTTP_PROPFIND, uri, "0");
    EXPECT_EQ(s.code, 403) << uri;
    EXPECT_EQ(s.body.find("getcontentlength"), std::string::npos) << uri;
  }
}

TEST_F(WebDAVHandlerTest, PropfindOfAMissingPathIsNotFound) {
  EXPECT_EQ(request(HTTP_PROPFIND, "/Books/missing.epub", "0").code, 404);
}

// ── GET ─────────────────────────────────────────────────────────────────────

TEST_F(WebDAVHandlerTest, GetStreamsAnOrdinaryFile) {
  EXPECT_EQ(request(HTTP_GET, "/Books/novel.epub").code, 200);
  EXPECT_EQ(streamed(), "novel bytes");
}

TEST_F(WebDAVHandlerTest, GetRefusesTheCredentialStores) {
  for (const char* uri :
       {"/.crosspoint/wifi.json", "/CROSSP~1/wifi.json", "/%20.crosspoint/wifi.json", "/.crosspoint./opds.json"}) {
    EXPECT_EQ(request(HTTP_GET, uri).code, 403) << uri;
  }
  EXPECT_EQ(streamed(), "");
}

// On a card where another entry held CROSSP~1 when /.crosspoint was made, SdFat gave it a hashed alias, as often as
// not with a letter after the tilde. That spelling used to pass as an ordinary name.
TEST_F(WebDAVHandlerTest, HashedAliasIsRefused) {
  Storage.aliases.clear();
  Storage.alias("/CRO~A3F2", "/.crosspoint");
  for (const char* uri : {"/CRO~A3F2/wifi.json", "/cro~a3f2/opds.json"}) {
    EXPECT_EQ(request(HTTP_GET, uri).code, 403) << uri;
  }
  EXPECT_EQ(streamed(), "");
  EXPECT_EQ(request(HTTP_PROPFIND, "/CRO~A3F2", "1").code, 403);
  EXPECT_EQ(server.body.find("wifi.json"), std::string::npos);
}

// A dot-named file whose own name has a tilde keeps it in its alias, before the tail.
TEST_F(WebDAVHandlerTest, AliasOfADotFileIsRefused) {
  Storage.put("/Books/.~lock.novel.odt#", "lock");
  Storage.alias("/Books/~LOCK_~1.ODT", "/Books/.~lock.novel.odt#");
  EXPECT_EQ(request(HTTP_GET, "/Books/~LOCK_~1.ODT").code, 403);
  EXPECT_EQ(streamed(), "");
}

// Resolution on the card still decides: a file really named like an alias is its own long name.
TEST_F(WebDAVHandlerTest, RealAliasShapedNamesStayReachable) {
  Storage.put("/Books/AB~C0DE.txt", "plain");
  EXPECT_EQ(request(HTTP_GET, "/Books/AB~C0DE.txt").code, 200);
  EXPECT_EQ(streamed(), "plain");
}

// ── COPY ────────────────────────────────────────────────────────────────────

TEST_F(WebDAVHandlerTest, CopyMakesAFullCopy) {
  const std::string bytes(10000, 'x');
  Storage.put("/Books/big.epub", bytes);
  EXPECT_EQ(request(HTTP_COPY, "/Books/big.epub", nullptr, "http://reader.local/Books/copy.epub").code, 201);
  EXPECT_EQ(Storage.content("/Books/copy.epub"), bytes);
  EXPECT_EQ(Storage.content("/Books/big.epub"), bytes);
  EXPECT_FALSE(Storage.exists("/Books/copy.epub.davtmp"));
}

// A read error part-way through ended the copy as if the source had run out: the cut-short copy took the
// destination's place and the client was told it was done.
TEST_F(WebDAVHandlerTest, CopyFailsOnAReadError) {
  Storage.put("/Books/big.epub", std::string(10000, 'x'));
  Storage.failReadsFrom("/Books/big.epub", 4096);
  EXPECT_EQ(request(HTTP_COPY, "/Books/big.epub", nullptr, "http://reader.local/Books/copy.epub").code, 500);
  EXPECT_FALSE(Storage.exists("/Books/copy.epub"));
  EXPECT_FALSE(Storage.exists("/Books/copy.epub.davtmp"));
}

TEST_F(WebDAVHandlerTest, CopyFailingOverAFileKeepsIt) {
  Storage.put("/Books/big.epub", std::string(10000, 'x'));
  Storage.failReadsFrom("/Books/big.epub", 4096);
  Storage.put("/Books/old.epub", "old book");
  EXPECT_EQ(request(HTTP_COPY, "/Books/big.epub", nullptr, "http://reader.local/Books/old.epub").code, 500);
  EXPECT_EQ(Storage.content("/Books/old.epub"), "old book");
  EXPECT_FALSE(Storage.exists("/Books/old.epub.davtmp"));
  EXPECT_TRUE(clearedBookCaches().empty());  // the book that stays keeps its cache
}

// A book copied over another kept the other's cache: its position, bookmarks and laid-out chapters, for different
// bytes. A PUT clears what was kept for the path it writes, and so does a COPY now.
TEST_F(WebDAVHandlerTest, CopyOverABookClearsItsCache) {
  Storage.put("/Books/old.epub", "old book");
  EXPECT_EQ(request(HTTP_COPY, "/Books/novel.epub", nullptr, "http://reader.local/Books/old.epub").code, 204);
  EXPECT_EQ(Storage.content("/Books/old.epub"), "novel bytes");
  EXPECT_EQ(clearedBookCaches(), std::vector<std::string>{"/Books/old.epub"});  // the source keeps its own
}

TEST_F(WebDAVHandlerTest, CopyClearsWhatWasKeptForANewPath) {
  EXPECT_EQ(request(HTTP_COPY, "/Books/novel.epub", nullptr, "http://reader.local/Books/copy.epub").code, 201);
  EXPECT_EQ(clearedBookCaches(), std::vector<std::string>{"/Books/copy.epub"});
}

// A cover written next to a book (novel.jpg beside novel.epub) makes that book's cached covers and thumbnails
// stale, and Home and the Library would go on drawing the old one. The file manager's upload drops them; WebDAV's
// PUT and COPY, through a copy of its helper that left this out, did not.
class WebDAVCoverTest : public WebDAVHandlerTest {
 protected:
  void SetUp() override {
    WebDAVHandlerTest::SetUp();
    cache = BookRelocation::cacheDirFor("/Books/novel.epub");
    Storage.putFolder(cache);
    Storage.put(cache + "/cover.bmp", "old cover");
    Storage.put(cache + "/thumb_240.bmp", "old thumb");
    Storage.put(cache + "/progress.bin", "where the book was left");
    Storage.put("/Books/novel.jpg", "old cover image");
    Storage.put("/Books/new.jpg", "new cover image");
  }

  void expectCoverDropped() const {
    EXPECT_FALSE(Storage.exists((cache + "/cover.bmp").c_str()));
    EXPECT_FALSE(Storage.exists((cache + "/thumb_240.bmp").c_str()));
    EXPECT_TRUE(Storage.exists((cache + "/progress.bin").c_str()));  // the reading state stays
  }

  std::string cache;
};

TEST_F(WebDAVCoverTest, CopyOfANewCoverDropsTheBooksThumbnails) {
  EXPECT_EQ(request(HTTP_COPY, "/Books/new.jpg", nullptr, "http://reader.local/Books/novel.jpg").code, 204);
  EXPECT_EQ(Storage.content("/Books/novel.jpg"), "new cover image");
  expectCoverDropped();
}

TEST_F(WebDAVCoverTest, PutOfANewCoverDropsTheBooksThumbnails) {
  server.setRequest(HTTP_PUT, "/Books/novel.jpg");
  HTTPRaw raw;
  raw.status = RAW_START;
  handler.raw(server, server.uri(), raw);
  raw.status = RAW_WRITE;
  raw.currentSize = 3;
  std::memcpy(raw.buf, "new", 3);
  handler.raw(server, server.uri(), raw);
  raw.status = RAW_END;
  handler.raw(server, server.uri(), raw);
  handler.handle(server, HTTP_PUT, server.uri());
  EXPECT_EQ(server.code, 204);
  EXPECT_EQ(Storage.content("/Books/novel.jpg"), "new");
  expectCoverDropped();
}

// What COPY now matches.
TEST_F(WebDAVHandlerTest, PutClearsTheBookCache) {
  for (const char* uri : {"/Books/novel.epub", "/Books/new.epub"}) {
    clearedBookCaches().clear();
    server.setRequest(HTTP_PUT, uri);
    HTTPRaw raw;
    raw.status = RAW_START;
    handler.raw(server, server.uri(), raw);
    raw.status = RAW_WRITE;
    raw.currentSize = 3;
    std::memcpy(raw.buf, "new", 3);
    handler.raw(server, server.uri(), raw);
    raw.status = RAW_END;
    handler.raw(server, server.uri(), raw);
    handler.handle(server, HTTP_PUT, server.uri());
    EXPECT_EQ(server.code / 100, 2) << uri;
    EXPECT_EQ(Storage.content(uri), "new") << uri;
    EXPECT_EQ(clearedBookCaches(), std::vector<std::string>{uri}) << uri;
  }
}

TEST_F(WebDAVHandlerTest, CopyRefusesProtectedEnds) {
  EXPECT_EQ(request(HTTP_COPY, "/.crosspoint/wifi.json", nullptr, "http://reader.local/Books/wifi.json").code, 403);
  EXPECT_EQ(request(HTTP_COPY, "/Books/novel.epub", nullptr, "http://reader.local/.crosspoint/wifi.json").code, 403);
  EXPECT_FALSE(Storage.exists("/Books/wifi.json"));
  EXPECT_NE(Storage.content("/.crosspoint/wifi.json"), "novel bytes");
}

}  // namespace
