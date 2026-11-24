// AsciiParse (src/util/AsciiParse.h) took the place of three sscanf formats: "%2x" for the Wi-Fi hint's BSSID,
// "%u.%u.%u.%u%n" for its cached addresses, and the hex loops of the reading-stats document id and the serial lab.
// These compare it with those formats on what the firmware writes, and pin down where it is stricter.
#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "AsciiParse.h"

namespace {

// The dotted-quad check JsonSettingsIO::loadWifi used before.
bool sscanfQuad(const std::string& s, uint8_t out[4]) {
  unsigned int a = 0, b = 0, c = 0, d = 0;
  int consumed = 0;
  if (sscanf(s.c_str(), "%u.%u.%u.%u%n", &a, &b, &c, &d, &consumed) != 4) return false;
  if (consumed < 0 || static_cast<size_t>(consumed) != s.size()) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  out[0] = a;
  out[1] = b;
  out[2] = c;
  out[3] = d;
  return true;
}

bool quad(const std::string& s, uint8_t out[4]) { return ascii_parse::ipv4(s.data(), s.size(), out); }

}  // namespace

TEST(AsciiParse, HexNibbleMatchesIsxdigitForEveryByte) {
  for (int c = 0; c < 256; ++c) {
    const char ch = static_cast<char>(c);
    if (std::isxdigit(c)) {
      const char text[2] = {ch, '\0'};
      EXPECT_EQ(ascii_parse::hexNibble(ch), static_cast<int>(strtol(text, nullptr, 16))) << c;
    } else {
      EXPECT_EQ(ascii_parse::hexNibble(ch), -1) << c;
    }
  }
}

TEST(AsciiParse, HexBytesMatchesSscanfOnWhatTheSaverWrites) {
  // loadWifi's saver writes the BSSID with "%02x" six times.
  std::mt19937 rng(7);
  for (int round = 0; round < 2000; ++round) {
    uint8_t bssid[6];
    for (auto& b : bssid) b = static_cast<uint8_t>(rng());
    char text[13];
    snprintf(text, sizeof(text), "%02x%02x%02x%02x%02x%02x", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4],
             bssid[5]);
    uint8_t parsed[6] = {};
    ASSERT_TRUE(ascii_parse::hexBytes(text, parsed, 6)) << text;
    for (int i = 0; i < 6; ++i) {
      unsigned int byte = 0;
      ASSERT_EQ(sscanf(text + i * 2, "%2x", &byte), 1);
      EXPECT_EQ(parsed[i], byte) << text << " byte " << i;
    }
  }
}

TEST(AsciiParse, HexBytesTakesEitherCase) {
  uint8_t out[3] = {};
  ASSERT_TRUE(ascii_parse::hexBytes("aB0f9C", out, 3));
  EXPECT_EQ(out[0], 0xAB);
  EXPECT_EQ(out[1], 0x0F);
  EXPECT_EQ(out[2], 0x9C);
}

TEST(AsciiParse, HexBytesRefusesANonDigitAndLeavesTheOutputAlone) {
  for (const char* bad : {"0011g2", "00 122", "0011-2", "00112", "x01122"}) {
    uint8_t out[3] = {0xEE, 0xEE, 0xEE};
    EXPECT_FALSE(ascii_parse::hexBytes(bad, out, 3)) << bad;
    EXPECT_EQ(out[0], 0xEE) << bad;
    EXPECT_EQ(out[1], 0xEE) << bad;
    EXPECT_EQ(out[2], 0xEE) << bad;
  }
}

TEST(AsciiParse, HexBytesReadsOnlyTheBytesAsked) {
  // A string_view's hex is not terminated: nothing past 2 * count may be read or matter.
  const char text[] = {'0', '1', 'f', 'F', 'z', 'z'};
  uint8_t out[2] = {};
  ASSERT_TRUE(ascii_parse::hexBytes(text, out, 2));
  EXPECT_EQ(out[0], 0x01);
  EXPECT_EQ(out[1], 0xFF);
  EXPECT_TRUE(ascii_parse::hexBytes("", out, 0));
}

TEST(AsciiParse, Ipv4MatchesSscanfOnWhatTheSaverWrites) {
  // loadWifi's saver writes each address with "%d.%d.%d.%d" of four bytes.
  std::mt19937 rng(11);
  for (int round = 0; round < 5000; ++round) {
    uint8_t bytes[4];
    for (auto& b : bytes) b = static_cast<uint8_t>(rng() % (round % 3 == 0 ? 10 : 256));
    char text[16];
    snprintf(text, sizeof(text), "%d.%d.%d.%d", bytes[0], bytes[1], bytes[2], bytes[3]);
    uint8_t mine[4] = {};
    uint8_t old[4] = {};
    ASSERT_TRUE(quad(text, mine)) << text;
    ASSERT_TRUE(sscanfQuad(text, old)) << text;
    EXPECT_EQ(memcmp(mine, old, 4), 0) << text;
    EXPECT_EQ(memcmp(mine, bytes, 4), 0) << text;
  }
}

TEST(AsciiParse, Ipv4RefusesWhatSscanfRefused) {
  for (const char* bad : {"", "1", "1.2.3", "1.2.3.", "1.2.3.4.", "1.2.3.4x", "256.1.1.1", "1.2.3.256", "1..3.4",
                          ".1.2.3.4", "1.2.3.4 ", "a.b.c.d", "1.2.3.99999999999", "1,2,3,4"}) {
    uint8_t mine[4] = {9, 9, 9, 9};
    uint8_t old[4] = {};
    EXPECT_FALSE(sscanfQuad(bad, old)) << '"' << bad << '"';
    EXPECT_FALSE(quad(bad, mine)) << '"' << bad << '"';
    EXPECT_EQ(mine[0], 9) << bad;
  }
}

TEST(AsciiParse, Ipv4AcceptsLeadingZerosAsSscanfDid) {
  uint8_t mine[4] = {};
  uint8_t old[4] = {};
  ASSERT_TRUE(quad("010.001.000.009", mine));
  ASSERT_TRUE(sscanfQuad("010.001.000.009", old));
  EXPECT_EQ(memcmp(mine, old, 4), 0);
  EXPECT_EQ(mine[0], 10);
}

TEST(AsciiParse, Ipv4IsStricterThanSscanfAboutBlanksAndSigns) {
  // %u skips blanks before a number and takes a sign; the saver never writes either, so they are refused now.
  for (const char* loose : {" 1.2.3.4", "1. 2.3.4", "+1.2.3.4", "1.2.3.+4"}) {
    uint8_t old[4] = {};
    uint8_t mine[4] = {};
    EXPECT_TRUE(sscanfQuad(loose, old)) << '"' << loose << '"';
    EXPECT_FALSE(quad(loose, mine)) << '"' << loose << '"';
  }
}

TEST(AsciiParse, Ipv4ReadsOnlyTheGivenLength) {
  const std::string withNul("1.2.3.4\0junk", 12);
  uint8_t out[4] = {};
  EXPECT_FALSE(quad(withNul, out));
  EXPECT_TRUE(ascii_parse::ipv4(withNul.data(), 7, out));
  EXPECT_EQ(out[3], 4);
}
