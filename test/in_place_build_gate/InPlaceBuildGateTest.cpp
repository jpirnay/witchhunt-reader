// InPlaceBuildGate: the free heap a resident section build asks for at its start.
#include <gtest/gtest.h>

#include "activities/reader/InPlaceBuildGate.h"

namespace {

using InPlaceBuildGate::minFreeHeap;
using InPlaceBuildGate::ttfMissBytesFor;

constexpr uint32_t kCssParseBase = 56 * 1024;  // IN_PLACE_BUILD_CSS_MIN_FREE_HEAP_BYTES
constexpr uint32_t kParseBase = 60 * 1024;     // IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES
constexpr uint32_t kExtractBase = 50 * 1024;   // IN_PLACE_BUILD_EXTRACT_BASE_HEAP_BYTES

// A CJK CFF family read from the card: its 17,408-byte scratch block and the 4,204-byte page cache.
constexpr uint32_t kCffSdMiss = 17408 + 4204;

uint32_t missBytes(const char* language, const uint32_t planBytes) {
  return ttfMissBytesFor(TypographyProfile::forLanguage(language), planBytes);
}

TEST(InPlaceBuildGate, WithoutMissMemoryTheFloorIsTheLargerPhase) {
  EXPECT_EQ(kCssParseBase, minFreeHeap(kCssParseBase, kExtractBase, 1539, 0));  // a small chapter: the parse
  EXPECT_EQ(kParseBase, minFreeHeap(kParseBase, kExtractBase, 9602, 0));
  EXPECT_EQ(kExtractBase + 32768, minFreeHeap(kCssParseBase, kExtractBase, 32768, 0));  // a large one: extraction
}

TEST(InPlaceBuildGate, MissMemoryRaisesTheParsePhase) {
  EXPECT_EQ(kCssParseBase + kCffSdMiss, minFreeHeap(kCssParseBase, kExtractBase, 1539, kCffSdMiss));
  // The X4 (2026-10-23): a 1.5 KB Simplified Chinese chapter admitted at 67,704 B free fell to 8,992 B and aborted.
  EXPECT_GE(67704u, minFreeHeap(kCssParseBase, kExtractBase, 1539, 0));
  EXPECT_LT(67704u, minFreeHeap(kCssParseBase, kExtractBase, 1539, kCffSdMiss));
  // A family in flash mode: the scratch block alone.
  EXPECT_EQ(kParseBase + 13312, minFreeHeap(kParseBase, kExtractBase, 9602, 13312));
}

TEST(InPlaceBuildGate, TheRingAndTheMissMemoryAreNeverLiveTogether) {
  // The ring goes before the parse begins, and a miss happens only while text is laid out.
  EXPECT_EQ(kExtractBase + 32768, minFreeHeap(kCssParseBase, kExtractBase, 32768, kCffSdMiss));
  EXPECT_EQ(kCssParseBase + kCffSdMiss, minFreeHeap(kCssParseBase, kExtractBase, 16384, kCffSdMiss));
}

TEST(InPlaceBuildGate, ACjkBookLeavesRoomForTheMisses) {
  EXPECT_EQ(kCffSdMiss, missBytes("zh-CN", kCffSdMiss));
  EXPECT_EQ(kCffSdMiss, missBytes("zh_TW", kCffSdMiss));
  EXPECT_EQ(13312u, missBytes("ja", 13312));
  EXPECT_EQ(kCffSdMiss, missBytes("ko-KR", kCffSdMiss));
}

TEST(InPlaceBuildGate, ABookSetBetweenSpacesKeepsTheFloorItHad) {
  EXPECT_EQ(0u, missBytes("en", 13312));
  EXPECT_EQ(0u, missBytes("de-DE", kCffSdMiss));
  EXPECT_EQ(0u, missBytes("", kCffSdMiss));  // no language tag: Latin rules
}

TEST(InPlaceBuildGate, WithoutATrueTypeSetThereIsNothingToLeave) {
  EXPECT_EQ(0u, missBytes("ja", 0));
  EXPECT_EQ(0u, missBytes("en", 0));
}

}  // namespace
