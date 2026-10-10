#include <ReleaseJsonParser.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr char DIGEST_HEX[] = "8745d7dc37c60052691a2ef124663e43ffe60a28bf51500615634058b1d1ab1f";

std::string asset(const std::string& name, const std::string& digestField) {
  return R"({"name":")" + name + R"(","size":6133040,)" + digestField +
         R"("browser_download_url":"https://github.com/o/r/releases/download/2.37/)" + name + R"("})";
}

void feedAll(ReleaseJsonParser& parser, const std::string& json) {
  // A byte at a time, as a stream may split it anywhere.
  for (char c : json) parser.feed(&c, 1);
}

bool sameAsDigestHex(const uint8_t* bytes) {
  for (size_t i = 0; i < 32; ++i) {
    char hex[3];
    snprintf(hex, sizeof(hex), "%02x", bytes[i]);
    if (memcmp(hex, DIGEST_HEX + 2 * i, 2) != 0) return false;
  }
  return true;
}

TEST(ReleaseJsonParserDigest, ReadsTheFirmwareAssetsSha256) {
  ReleaseJsonParser parser;
  const std::string digest = std::string(R"("digest":"sha256:)") + DIGEST_HEX + R"(",)";
  feedAll(parser, R"({"tag_name":"2.37","assets":[)" +
                      asset("firmware-x4pro.bin", R"("digest":"sha256:)" + std::string(64, 'a') + R"(",)") + "," +
                      asset("firmware.bin", digest) + "]}");
  ASSERT_TRUE(parser.foundFirmware());
  EXPECT_EQ(parser.getFirmwareSize(), 6133040u);
  ASSERT_TRUE(parser.hasFirmwareSha256());
  EXPECT_TRUE(sameAsDigestHex(parser.getFirmwareSha256()));
}

TEST(ReleaseJsonParserDigest, AnotherAssetsDigestIsNotTaken) {
  ReleaseJsonParser parser;
  feedAll(parser, R"({"tag_name":"2.37","assets":[)" +
                      asset("firmware-x4pro.bin", std::string(R"("digest":"sha256:)") + DIGEST_HEX + R"(",)") + "," +
                      asset("firmware.bin", "") + "]}");
  ASSERT_TRUE(parser.foundFirmware());
  EXPECT_FALSE(parser.hasFirmwareSha256());
}

TEST(ReleaseJsonParserDigest, ANullOrMalformedDigestIsNone) {
  for (const std::string& field : {std::string(R"("digest":null,)"), std::string(R"("digest":"sha512:abcd",)"),
                                   std::string(R"("digest":"sha256:)") + std::string(63, 'a') + R"(",)",
                                   std::string(R"("digest":"sha256:)") + std::string(63, 'a') + R"(g",)"}) {
    ReleaseJsonParser parser;
    feedAll(parser, R"({"tag_name":"2.37","assets":[)" + asset("firmware.bin", field) + "]}");
    ASSERT_TRUE(parser.foundFirmware()) << field;
    EXPECT_FALSE(parser.hasFirmwareSha256()) << field;
  }
}

TEST(ReleaseJsonParserDigest, TheBetaListFormCarriesTheDigestToo) {
  ReleaseJsonParser parser;
  feedAll(parser, R"([{"tag_name":"2.38.0-rc.1","assets":[)" +
                      asset("firmware.bin", std::string(R"("digest":"sha256:)") + DIGEST_HEX + R"(",)") + "]}]");
  ASSERT_TRUE(parser.foundFirmware());
  EXPECT_STREQ(parser.getTagName(), "2.38.0-rc.1");
  ASSERT_TRUE(parser.hasFirmwareSha256());
  EXPECT_TRUE(sameAsDigestHex(parser.getFirmwareSha256()));
}

TEST(ReleaseJsonParserDigest, ResetForgetsTheDigest) {
  ReleaseJsonParser parser;
  feedAll(parser, R"({"tag_name":"2.37","assets":[)" +
                      asset("firmware.bin", std::string(R"("digest":"sha256:)") + DIGEST_HEX + R"(",)") + "]}");
  ASSERT_TRUE(parser.hasFirmwareSha256());
  parser.reset();
  EXPECT_FALSE(parser.hasFirmwareSha256());
}

std::string jsonString(const std::string& text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  return out + "\"";
}

// GitHub's key order: the title before the assets, the notes after them.
std::string release(const std::string& nameField, const std::string& bodyField) {
  return R"({"tag_name":"2.37",)" + nameField + R"("assets":[)" + asset("firmware.bin", "") +
         R"(],"tarball_url":"https://api.github.com/repos/o/r/tarball/2.37")" + bodyField + "}";
}

TEST(ReleaseJsonParserNotes, ReadsTheReleaseTitle) {
  ReleaseJsonParser parser;
  feedAll(parser, release(R"("name":"2.37 Fixes and improvements",)", ""));
  EXPECT_STREQ(parser.getReleaseName(), "2.37 Fixes and improvements");
}

TEST(ReleaseJsonParserNotes, AnAssetsNameIsNotTheTitle) {
  ReleaseJsonParser parser;
  feedAll(parser, release("", ""));
  ASSERT_TRUE(parser.foundFirmware());
  EXPECT_STREQ(parser.getReleaseName(), "");
}

TEST(ReleaseJsonParserNotes, ALongTitleIsCutOnACodepointBoundary) {
  ReleaseJsonParser parser;
  // 62 ASCII bytes, then a two-byte e-acute that would straddle the 63-byte limit.
  feedAll(parser, release(R"("name":")" + std::string(62, 'a') + "\xC3\xA9 more\",", ""));
  EXPECT_EQ(std::string(parser.getReleaseName()), std::string(62, 'a'));
}

TEST(ReleaseJsonParserNotes, ReadsTheNotesOfAShortBody) {
  ReleaseJsonParser parser;
  feedAll(parser, release("", ",\"body\":" + jsonString("Fixes.\r\n\r\n- One\r\n- Two")));
  EXPECT_TRUE(parser.foundNotes());
  EXPECT_STREQ(parser.getReleaseNotes(), "Fixes.\n- One\n- Two");
}

TEST(ReleaseJsonParserNotes, ALongBodyIsReadFromItsStartBeforeItEnds) {
  std::string body =
      "2.37 improves reading position recovery, chapter page counting, reading statistics, keyboard layouts, and "
      "several navigation issues.\r\n\r\n## Highlights\r\n\r\n- Reading position is now restored correctly after "
      "layout changes.\r\n";
  while (body.size() < 4000) body += "- Fixed font spacing and screen-refresh artefacts.\r\n";
  const std::string json = release("", ",\"body\":" + jsonString(body));

  // Up to well into the notes, but not to their end: the update check stops reading here.
  ReleaseJsonParser parser;
  const size_t cut = json.find("\"body\"") + 1200;
  feedAll(parser, json.substr(0, cut));
  ASSERT_TRUE(parser.foundNotes());
  const std::string notes = parser.getReleaseNotes();
  EXPECT_EQ(notes.rfind(
                "2.37 improves reading position recovery, chapter page counting, reading statistics, keyboard layouts, "
                "and several navigation issues.\n- Reading position is now restored correctly after layout changes.\n",
                0),
            0u)
      << notes;

  // The rest still parses, and the excerpt stands.
  feedAll(parser, json.substr(cut));
  EXPECT_TRUE(parser.foundFirmware());
  EXPECT_EQ(std::string(parser.getReleaseNotes()), notes);
}

TEST(ReleaseJsonParserNotes, ANullBodyCountsAsRead) {
  ReleaseJsonParser parser;
  feedAll(parser, release("", R"(,"body":null)"));
  EXPECT_TRUE(parser.foundNotes());
  EXPECT_STREQ(parser.getReleaseNotes(), "");
}

TEST(ReleaseJsonParserNotes, NoBodyMeansNoNotes) {
  ReleaseJsonParser parser;
  feedAll(parser, release("", ""));
  EXPECT_FALSE(parser.foundNotes());
  EXPECT_STREQ(parser.getReleaseNotes(), "");
}

TEST(ReleaseJsonParserNotes, ABodyInsideANestedObjectIsNotTheNotes) {
  ReleaseJsonParser parser;
  feedAll(parser, release(R"("author":{"name":"someone","body":"not the notes"},)", ""));
  EXPECT_FALSE(parser.foundNotes());
  EXPECT_STREQ(parser.getReleaseName(), "");
}

TEST(ReleaseJsonParserNotes, TheBetaListFormCarriesTitleAndNotes) {
  ReleaseJsonParser parser;
  feedAll(parser, "[" + release(R"("name":"Beta",)", R"(,"body":"Try it.\n")") + "]");
  EXPECT_STREQ(parser.getReleaseName(), "Beta");
  EXPECT_TRUE(parser.foundNotes());
  EXPECT_STREQ(parser.getReleaseNotes(), "Try it.");
}

TEST(ReleaseJsonParserNotes, ResetForgetsTitleAndNotes) {
  ReleaseJsonParser parser;
  feedAll(parser, release(R"("name":"Beta",)", R"(,"body":"Try it.")"));
  ASSERT_TRUE(parser.foundNotes());
  parser.reset();
  EXPECT_STREQ(parser.getReleaseName(), "");
  EXPECT_FALSE(parser.foundNotes());
  EXPECT_STREQ(parser.getReleaseNotes(), "");
}

}  // namespace
