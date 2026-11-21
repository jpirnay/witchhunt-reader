// How a KOSync GET /syncs/progress reply is read (lib/KOReaderSync/KOReaderProgressReply).
//
// The client screens the transport first (status, a captive portal's HTML, a bodiless 2xx); what
// reaches parseProgressReply() is a 2xx body that looks like JSON. Servers disagree on how they
// say "nothing stored", and every one of those shapes has to read as NOT_FOUND, never as a record
// at 0 %: the sync screen would offer it, and smart sync would apply it.
#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

#include "KOReaderSync/KOReaderJsonNumber.h"
#include "KOReaderSync/KOReaderProgressReply.h"
#include "SeriesIndex.h"

namespace {

constexpr char kHash[] = "0123456789abcdef0123456789abcdef";

KOReaderSyncClient::Error parse(const std::string& body, KOReaderProgress& out) {
  return parseProgressReply(body, kHash, out);
}

}  // namespace

// koreader-sync-server's reply for a stored position.
TEST(KOReaderProgressReply, LocatorRecordIsRead) {
  KOReaderProgress out;
  ASSERT_EQ(parse(R"({"percentage":0.4567,"progress":"/body/DocFragment[12]/body/div/p[3]/text().42",)"
                  R"("device":"KOReader","device_id":"A1B2","timestamp":1700000000,"document":"x"})",
                  out),
            KOReaderSyncClient::OK);
  EXPECT_EQ(out.document, kHash);
  EXPECT_EQ(out.progress, "/body/DocFragment[12]/body/div/p[3]/text().42");
  ASSERT_TRUE(out.percentage.has_value());
  EXPECT_EQ(out.percentage->millionths(), 456700u);
  EXPECT_EQ(out.device, "KOReader");
  EXPECT_EQ(out.deviceId, "A1B2");
  EXPECT_EQ(out.timestamp, 1700000000);
}

// "Nothing stored", as the reference server ({}) and others ([], "", null) serialize it.
TEST(KOReaderProgressReply, EmptyPayloadsAreNotFound) {
  for (const char* body : {"{}", "[]", "\"\"", "null"}) {
    KOReaderProgress out;
    EXPECT_EQ(parse(body, out), KOReaderSyncClient::NOT_FOUND) << body;
  }
}

// Calibre-Web NextGen's percentage-only record (its web reader or a Kobo wrote it), served to a
// client that asked for position_kinds=locator,percentage. ProgressMapper places it by percentage.
TEST(KOReaderProgressReply, PercentageOnlyRecordIsRead) {
  KOReaderProgress out;
  ASSERT_EQ(parse(R"({"document":"x","progress":null,"position_kind":"percentage","percentage":0.4567,)"
                  R"("device":"Web reader","device_id":"cwng-web","timestamp":1700000000})",
                  out),
            KOReaderSyncClient::OK);
  EXPECT_EQ(out.progress, "");  // not "null": that would reach the XPath mapper
  ASSERT_TRUE(out.percentage.has_value());
  EXPECT_EQ(out.percentage->millionths(), 456700u);
  EXPECT_EQ(out.device, "Web reader");
  EXPECT_EQ(out.deviceId, "cwng-web");
}

// The same position from a server that leaves the field out, or stores it as "".
TEST(KOReaderProgressReply, PercentageWithoutXPathIsRead) {
  for (const char* body : {R"({"percentage":0.25})", R"({"progress":"","percentage":0.25})"}) {
    KOReaderProgress out;
    ASSERT_EQ(parse(body, out), KOReaderSyncClient::OK) << body;
    EXPECT_EQ(out.progress, "") << body;
    EXPECT_EQ(out.percentage, ProgressPpb(250000000)) << body;  // a quarter is exact in a float
  }
}

// A record at the very start is still a record.
TEST(KOReaderProgressReply, PercentageZeroIsARecord) {
  KOReaderProgress out;
  EXPECT_EQ(parse(R"({"progress":null,"percentage":0})", out), KOReaderSyncClient::OK);
  EXPECT_EQ(out.percentage, ProgressPpb());
}

// KOReader sends its page over its page count in full ("0.45323741007194"); the float ArduinoJson parses it into
// becomes the nearest billionth, and goes back on the wire as the nearest millionth. Past the book's ends is its
// ends; a number too large for a float (an infinity) is no usable percentage, which the comparison treats as unknown
// rather than as 0 %.
TEST(KOReaderProgressReply, PercentageBecomesBillionthsOnce) {
  const struct {
    const char* number;
    std::optional<uint32_t> millionths;
  } cases[] = {
      {"0.45323741007194", 453237},
      {"0.7", 700000},  // 0.699999988 as a float
      {"1", 1000000},
      {"1.5", 1000000},
      {"-0.25", 0},
      {"0.0000004", 0},
      {"0.0000006", 1},
      {"1e39", std::nullopt},
  };
  for (const auto& c : cases) {
    KOReaderProgress out;
    const std::string body = std::string(R"({"progress":"/body/DocFragment[1]/body","percentage":)") + c.number + "}";
    ASSERT_EQ(parse(body, out), KOReaderSyncClient::OK) << c.number;
    ASSERT_EQ(out.percentage.has_value(), c.millionths.has_value()) << c.number;
    if (out.percentage) EXPECT_EQ(out.percentage->millionths(), *c.millionths) << c.number;
  }
}

// Calibre-Web NextGen's answer to a client that did not ask for the kind of position it holds:
// nothing to apply, so no record, and never a position at 0 %.
TEST(KOReaderProgressReply, WithheldPositionIsNotFound) {
  KOReaderProgress out;
  EXPECT_EQ(parse(R"({"position_kinds_available":["percentage"]})", out), KOReaderSyncClient::NOT_FOUND);
}

// Objects that hold neither an XPath nor a percentage.
TEST(KOReaderProgressReply, ObjectWithoutAPositionIsNotFound) {
  for (const char* body : {R"({"document":"x"})", R"({"progress":null})", R"({"progress":""})",
                           R"({"progress":null,"percentage":null,"device":"KOReader"})"}) {
    KOReaderProgress out;
    EXPECT_EQ(parse(body, out), KOReaderSyncClient::NOT_FOUND) << body;
  }
}

// koreader-sync-server leaves out a field it has no value for; that is an empty string here, not
// "null", which the sync screen would print as "from null".
TEST(KOReaderProgressReply, MissingDeviceFieldsReadEmpty) {
  KOReaderProgress out;
  ASSERT_EQ(parse(R"({"progress":"/body/DocFragment[3]/body/p[7]/text().0","percentage":0.1})", out),
            KOReaderSyncClient::OK);
  EXPECT_EQ(out.device, "");
  EXPECT_EQ(out.deviceId, "");
}

TEST(KOReaderProgressReply, TruncatedJsonIsAJsonError) {
  KOReaderProgress out;
  EXPECT_EQ(parse(R"({"progress":"/body/DocFragment[2]/bo)", out), KOReaderSyncClient::JSON_ERROR);
}

// --- The numbers an upload sends (KOReaderJsonNumber.h) -------------------------------------------
//
// updateProgress() used to put the float into the document and let ArduinoJson write it; it now writes the digits
// itself. The server must get the same bytes.

namespace {

std::string arduinoJsonText(const float value) {
  JsonDocument doc;
  doc["n"] = value;
  std::string out;
  serializeJson(doc, out);
  return out.substr(5, out.size() - 6);  // {"n":...}
}

std::string ourText(const int64_t millionths) {
  char text[24];
  formatJsonMillionths(millionths, text);
  return text;
}

}  // namespace

// Every percentage the wire can carry, against what ArduinoJson wrote for the float nearest to it.
TEST(KOReaderJsonNumber, EveryPercentageIsWrittenAsArduinoJsonWroteItsFloat) {
  int mismatches = 0;
  for (uint32_t m = 0; m <= 1000000; ++m) {
    char text[24];
    formatJsonPercentage(ProgressPpb(m * 1000), text);
    const std::string expected = arduinoJsonText(static_cast<float>(m / 1e6));
    if (text != expected && ++mismatches <= 10) {
      ADD_FAILURE() << m << ": ours " << text << ", ArduinoJson " << expected;
    }
  }
  EXPECT_EQ(mismatches, 0);
}

// A percentage finer than the wire rounds to its sixth decimal half up, as ArduinoJson rounded the float. Where the
// float of it lay on the other side of a half millionth (it carries 24 bits, about 3e-8 of error), the last digit
// was the float's rounding error; those are the only differences.
TEST(KOReaderJsonNumber, AFinerPercentageRoundsToTheSixthDecimal) {
  uint32_t state = 7;
  int differing = 0;
  for (int i = 0; i < 200000; ++i) {
    state = state * 1664525u + 1013904223u;
    const uint32_t ppb = state % (ProgressPpb::kWhole + 1);
    char text[24];
    formatJsonPercentage(ProgressPpb(ppb), text);
    const std::string expected = arduinoJsonText(static_cast<float>(ppb / 1e9));
    if (text != expected) {
      ++differing;
      const uint32_t fromHalf = ppb % 1000 > 500 ? ppb % 1000 - 500 : 500 - ppb % 1000;
      EXPECT_LE(fromHalf, 60u) << ppb << ": ours " << text << ", ArduinoJson " << expected;
    }
  }
  EXPECT_LT(differing, 200000 / 50);
  char text[24];
  formatJsonPercentage(ProgressPpb(453236500), text);
  EXPECT_STREQ(text, "0.453237");
  formatJsonPercentage(ProgressPpb(453236499), text);
  EXPECT_STREQ(text, "0.453236");
}

// The first two hundred-thousandths of a book, every billionth: ArduinoJson's exponent form up to 0.00001, then six
// decimals. In the exponent form it printed seven digits of a 24-bit float, the last of them that float's error
// ("9.619999e-7" for 962 billionths); ours are the billionths' own. A text that differs reads back as the same
// number to within the float's precision, and above 0.00001 only an exact half millionth (0.0000105) may differ.
TEST(KOReaderJsonNumber, TheSmallestPercentagesAreWrittenAsArduinoJsonWroteThem) {
  int differing = 0;
  for (uint32_t ppb = 0; ppb <= 20000; ++ppb) {
    char text[24];
    formatJsonPercentage(ProgressPpb(ppb), text);
    const std::string theirs = arduinoJsonText(static_cast<float>(ppb / 1e9));
    if (text == theirs) continue;
    ++differing;
    if (ppb > 10000) {
      EXPECT_EQ(ppb % 1000, 500u) << ppb << ": ours " << text << ", ArduinoJson " << theirs;
    } else {
      const double ours = std::strtod(text, nullptr);
      EXPECT_NEAR(ours, std::strtod(theirs.c_str(), nullptr), ours * 2e-7) << ppb << ": ours " << text;
      EXPECT_DOUBLE_EQ(ours, ppb / 1e9) << text;
    }
  }
  EXPECT_LT(differing, 20000 / 10);
  char text[24];
  formatJsonPercentage(ProgressPpb(4403), text);
  EXPECT_STREQ(text, "4.403e-6");
  formatJsonPercentage(ProgressPpb(10000), text);
  EXPECT_STREQ(text, "1e-5");
  formatJsonPercentage(ProgressPpb(10001), text);
  EXPECT_STREQ(text, "0.00001");
  formatJsonPercentage(ProgressPpb(5), text);
  EXPECT_STREQ(text, "5e-9");
}

TEST(KOReaderJsonNumber, PercentageEdges) {
  EXPECT_EQ(ourText(0), "0");
  EXPECT_EQ(ourText(1), "1e-6");
  EXPECT_EQ(ourText(10), "1e-5");
  EXPECT_EQ(ourText(11), "0.000011");
  EXPECT_EQ(ourText(453200), "0.4532");
  EXPECT_EQ(ourText(999999), "0.999999");
  EXPECT_EQ(ourText(1000000), "1");
}

// Every float a wire percentage turns into comes back as its millionth.
TEST(KOReaderJsonNumber, EveryPercentageFloatComesBackAsItsMillionth) {
  int mismatches = 0;
  for (uint32_t m = 0; m <= 1000000; ++m) {
    const auto back = progressFromJsonFloat(static_cast<float>(m / 1e6));
    if ((!back || back->millionths() != m) && ++mismatches <= 10) ADD_FAILURE() << m;
  }
  EXPECT_EQ(mismatches, 0);
  EXPECT_EQ(progressFromJsonFloat(0.25f), ProgressPpb(250000000));
  EXPECT_EQ(progressFromJsonFloat(std::numeric_limits<float>::infinity()), std::nullopt);
  EXPECT_EQ(progressFromJsonFloat(std::numeric_limits<float>::quiet_NaN()), std::nullopt);
  EXPECT_EQ(progressFromJsonFloat(-std::numeric_limits<float>::infinity()), std::nullopt);
  EXPECT_EQ(progressFromJsonFloat(std::numeric_limits<float>::denorm_min()), ProgressPpb());
}

// A series index, read as strtof read it and written as ArduinoJson wrote that float.
TEST(KOReaderJsonNumber, SeriesIndexIsWrittenAsArduinoJsonWroteItsFloat) {
  for (const char* index :
       {"0",      "1",  "2",   "2.5",    "3.0",      "02",       "10",       "12.5",       "1.25",     "0.5",
        "100.75", "-1", "1e2", "2.5e-1", "1234.567", "123456.7", "999999.5", "1234567.25", "0.000005", " 4"}) {
    const char* end = nullptr;
    const auto parsed = SeriesIndex::parse(index, &end);
    ASSERT_TRUE(parsed.has_value()) << index;
    EXPECT_EQ(*end, '\0') << index;
    EXPECT_EQ(ourText(parsed->millionths), arduinoJsonText(std::strtof(index, nullptr))) << index;
  }
}

// The upload's body, built as updateProgress() builds it, is the body the float made.
TEST(KOReaderJsonNumber, UploadBodyIsByteForByteTheSame) {
  for (const uint32_t m : {0u, 7u, 123456u, 453200u, 500000u, 999999u, 1000000u}) {
    JsonDocument before;
    before["document"] = "x";
    before["percentage"] = static_cast<float>(m / 1e6);
    before["device"] = "d";
    JsonDocument after;
    after["document"] = "x";
    char number[24];
    formatJsonPercentage(ProgressPpb(m * 1000), number);
    after["percentage"] = serialized(static_cast<const char*>(number));
    after["device"] = "d";
    std::string a;
    std::string b;
    serializeJson(before, a);
    serializeJson(after, b);
    EXPECT_EQ(b, a) << m;
  }
}
