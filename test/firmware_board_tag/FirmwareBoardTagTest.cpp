// Host tests for the board-identity guard that keeps a firmware image built for
// one board from being flashed onto another (src/network/FirmwareBoardTag.h).
//
// This binary is compiled with -DFREEINK_DEVICE_LILYGO=1, so the "running
// firmware" under test is the lilygo build.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "FirmwareBoardTag.h"

namespace {

constexpr const char* MAGIC = "CROSSPOINT-BOARD-V1:";

std::string selfName() { return std::string(board_tag::boardName(), board_tag::boardNameLen()); }

// Run a buffer through the scanner in fixed-size chunks.
bool scanChunked(const std::string& data, size_t chunk) {
  board_tag::Scanner s;
  for (size_t i = 0; i < data.size(); i += chunk) {
    const size_t n = std::min(chunk, data.size() - i);
    s.feed(reinterpret_cast<const uint8_t*>(data.data() + i), n);
  }
  return s.mismatch();
}

std::string tagFor(const std::string& board) { return std::string(MAGIC) + board + ";"; }

}  // namespace

TEST(FirmwareBoardTag, TagNamesThisBuild) {
  EXPECT_EQ(selfName(), "lilygo");
  EXPECT_STREQ(board_tag::TAG, "CROSSPOINT-BOARD-V1:lilygo;");
  // boardName() points into TAG rather than duplicating the literal, so an
  // image contains exactly one copy of the needle.
  EXPECT_EQ(board_tag::boardName(), board_tag::TAG + std::strlen(MAGIC));
}

// The scanner restarts a failed magic match with a single-byte lookback, which
// is only correct while the magic's first character appears nowhere else in it.
// If someone edits the magic, this is the test that catches it.
TEST(FirmwareBoardTag, MagicFirstCharIsUnique) {
  const std::string magic(MAGIC);
  EXPECT_EQ(magic.find(magic[0], 1), std::string::npos);
}

TEST(FirmwareBoardTag, OwnTagIsNotAMismatch) {
  EXPECT_FALSE(scanChunked("padding" + tagFor("lilygo") + "trailing", 4096));
}

TEST(FirmwareBoardTag, ForeignTagIsAMismatch) {
  board_tag::Scanner s;
  const std::string img = "padding" + tagFor("x4") + "trailing";
  s.feed(reinterpret_cast<const uint8_t*>(img.data()), img.size());
  EXPECT_TRUE(s.mismatch());
  EXPECT_STREQ(s.foundName(), "x4");
}

TEST(FirmwareBoardTag, SiblingS3BoardIsAMismatch) {
  // x4pro and lilygo share an esp_image_header chip_id, so the tag is the only
  // thing that can tell these two images apart.
  board_tag::Scanner s;
  const std::string img = tagFor("x4pro");
  s.feed(reinterpret_cast<const uint8_t*>(img.data()), img.size());
  EXPECT_TRUE(s.mismatch());
  EXPECT_STREQ(s.foundName(), "x4pro");
}

TEST(FirmwareBoardTag, UntaggedImagePasses) {
  // Other projects and older releases carry no tag; they must not be rejected.
  std::string img(10000, '\x00');
  for (size_t i = 0; i < img.size(); i++) img[i] = static_cast<char>(i * 7);
  EXPECT_FALSE(scanChunked(img, 1024));
}

TEST(FirmwareBoardTag, DetectedAtEveryChunkBoundary) {
  // A real download splits the stream at arbitrary offsets; the tag must still
  // be found when a chunk boundary falls anywhere inside it.
  const std::string img = std::string(37, 'x') + tagFor("x4") + std::string(41, 'y');
  for (size_t chunk = 1; chunk <= img.size(); chunk++) {
    EXPECT_TRUE(scanChunked(img, chunk)) << "chunk size " << chunk;
  }
}

TEST(FirmwareBoardTag, RestartsAfterAPartialMagic) {
  // A truncated magic immediately followed by the real one: the single-byte
  // lookback has to pick the second occurrence up.
  const std::string img = "CROSSPOINT-BOARD-V" + tagFor("x4");
  board_tag::Scanner s;
  s.feed(reinterpret_cast<const uint8_t*>(img.data()), img.size());
  EXPECT_TRUE(s.mismatch());
  EXPECT_STREQ(s.foundName(), "x4");
}

TEST(FirmwareBoardTag, OverlongNameIsIgnored) {
  // A chance byte-collision with the magic followed by unbounded garbage must
  // not be treated as a board name.
  const std::string img = std::string(MAGIC) + std::string(64, 'z') + ";";
  EXPECT_FALSE(scanChunked(img, 8));
}

TEST(FirmwareBoardTag, NonPrintableNameIsIgnored) {
  const std::string img = std::string(MAGIC) + "x4\x01pro;";
  EXPECT_FALSE(scanChunked(img, 8));
}

TEST(FirmwareBoardTag, MismatchIsSticky) {
  // Callers abort a transfer on mismatch(); later bytes must not clear it.
  board_tag::Scanner s;
  const std::string bad = tagFor("x4");
  s.feed(reinterpret_cast<const uint8_t*>(bad.data()), bad.size());
  ASSERT_TRUE(s.mismatch());
  const std::string good = tagFor("lilygo");
  s.feed(reinterpret_cast<const uint8_t*>(good.data()), good.size());
  EXPECT_TRUE(s.mismatch());
  EXPECT_STREQ(s.foundName(), "x4");
}
