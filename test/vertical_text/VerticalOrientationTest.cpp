// UAX #50 classes (Unicode 16.0) and the pieces a vertical column is drawn in.
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "Epub/VerticalOrientation.h"
#include "Epub/VerticalOrientationTable.h"

TEST(VerticalOrientation, TheTableIsUnicode16sInRuns) {
  EXPECT_EQ(264u, kVerticalOrientationRunCount);
  EXPECT_EQ(0u, kVerticalOrientationRuns[0] >> 2);
  for (size_t i = 1; i < kVerticalOrientationRunCount; ++i) {
    EXPECT_LT(kVerticalOrientationRuns[i - 1] >> 2, kVerticalOrientationRuns[i] >> 2);
    EXPECT_NE(kVerticalOrientationRuns[i - 1] & 3, kVerticalOrientationRuns[i] & 3) << "adjacent runs are merged";
  }
}

TEST(VerticalOrientation, ClassesFollowUax50) {
  using V = VerticalOrientation;
  for (const uint32_t cp : {0x4E00u, 0x3005u, 0x3000u, 0x30FBu, 0xAC00u, 0x1100u, 0xA7u, 0xA9u, 0xD7u, 0xF7u, 0x2160u,
                            0x2460u, 0x3099u, 0xFE45u, 0x20000u, 0x1F600u, 0xE000u, 0xFFFDu, 0x25CFu})
    EXPECT_EQ(V::U, verticalOrientationOf(cp)) << std::hex << cp;
  for (const uint32_t cp : {0x3001u, 0x3002u, 0x3041u, 0x3063u, 0x30A1u, 0x30C3u, 0x31F0u, 0xFF01u, 0xFF0Cu, 0xFF0Eu,
                            0xFF1Fu, 0x309Bu, 0x309Cu})
    EXPECT_EQ(V::Tu, verticalOrientationOf(cp)) << std::hex << cp;
  for (const uint32_t cp : {0x3008u, 0x300Cu, 0x300Du, 0x3014u, 0x301Cu, 0x301Du, 0x30FCu, 0x30A0u, 0xFF08u, 0xFF09u,
                            0xFF1Au, 0xFF1Bu, 0xFF5Eu, 0xFF5Bu, 0xFF5Du, 0xFF5Fu, 0xFF60u, 0xFF3Fu, 0xFF5Cu, 0x3030u})
    EXPECT_EQ(V::Tr, verticalOrientationOf(cp)) << std::hex << cp;
  for (const uint32_t cp : {0x41u, 0x31u, 0x20u, 0x2014u, 0x2015u, 0x2025u, 0x2026u, 0x2E3Au, 0xB7u, 0x2010u, 0x2013u,
                            0x2190u, 0xFF0Du, 0xFF61u, 0xFF65u, 0xFF71u, 0xFF9Eu, 0x2022u, 0x10FFFFu})
    EXPECT_EQ(V::R, verticalOrientationOf(cp)) << std::hex << cp;
}

TEST(VerticalOrientation, UprightIsUAndTuSidewaysIsRAndTr) {
  EXPECT_TRUE(drawsUpright(0x4E00));   // 一 U
  EXPECT_TRUE(drawsUpright(0x3002));   // 。 Tu: upright, unplaced
  EXPECT_FALSE(drawsUpright(0x300C));  // 「 Tr: sideways
  EXPECT_FALSE(drawsUpright(0x30FC));  // ー Tr
  EXPECT_FALSE(drawsUpright('A'));
}

namespace {
using Pieces = std::vector<std::pair<size_t, bool>>;

Pieces piecesOf(const std::string& text) {
  Pieces out;
  const char* p = text.c_str();
  bool upright = false;
  while (const size_t n = nextColumnPiece(p, upright)) {
    out.emplace_back(n, upright);
    p += n;
  }
  return out;
}
}  // namespace

TEST(VerticalOrientation, APieceKeepsItsMarksAndNeverSplitsACodepoint) {
  EXPECT_EQ((Pieces{{3, true}, {3, true}}), piecesOf("日本"));
  EXPECT_EQ((Pieces{{5, false}, {3, true}}), piecesOf("Latin語"));
  EXPECT_EQ((Pieces{{6, true}}), piecesOf("か\u3099"));
  EXPECT_EQ((Pieces{{4, false}}), piecesOf("e\u0301a"));
  EXPECT_EQ((Pieces{{3, true}}), piecesOf("\u3099"));
  EXPECT_EQ((Pieces{{64, false}, {6, false}}), piecesOf(std::string(70, 'a')));
  std::string accented;
  for (int i = 0; i < 40; ++i) accented += "\u00E9";
  EXPECT_EQ((Pieces{{64, false}, {16, false}}), piecesOf(accented));
  EXPECT_EQ((Pieces{{63, false}, {3, false}}), piecesOf(std::string(63, 'a') + "e\u0301"));
  EXPECT_EQ((Pieces{{7, true}}), piecesOf("😀️"));
  EXPECT_TRUE(piecesOf("").empty());
}

TEST(VerticalOrientation, AClusterLongerThanTheCapIsCutAtACodepoint) {
  std::string text = "a";
  for (int i = 0; i < 40; ++i) text += "\u0301";
  ASSERT_EQ(81u, text.size());
  const Pieces pieces = piecesOf(text);
  size_t total = 0;
  for (const auto& piece : pieces) {
    EXPECT_LE(piece.first, kMaxColumnRunBytes);
    total += piece.first;
    // A boundary is never a UTF-8 continuation byte.
    if (total < text.size()) {
      EXPECT_NE(0x80, static_cast<unsigned char>(text[total]) & 0xC0);
    }
  }
  EXPECT_EQ(81u, total);
}

TEST(VerticalOrientation, PlacementClasses) {
  for (const uint32_t cp : {0x3001u, 0x3002u, 0xFF0Cu, 0xFF0Eu}) {
    EXPECT_EQ(UprightPlacement::Reflect, uprightPlacementOf(cp)) << std::hex << cp;
  }
  std::vector<uint32_t> small = {0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085,
                                 0x3087, 0x308E, 0x3095, 0x3096, 0x30A1, 0x30A3, 0x30A5, 0x30A7,
                                 0x30A9, 0x30C3, 0x30E3, 0x30E5, 0x30E7, 0x30EE, 0x30F5, 0x30F6};
  for (uint32_t cp = 0x31F0; cp <= 0x31FF; cp++) small.push_back(cp);
  for (const uint32_t cp : small) {
    EXPECT_EQ(UprightPlacement::SmallKana, uprightPlacementOf(cp)) << std::hex << cp;
  }
  for (const uint32_t cp :
       {0x4E00u, 0x3042u, 0x300Cu, 0x30FCu, 0xFF01u, static_cast<uint32_t>('a'), 0x3040u, 0x3200u}) {
    EXPECT_EQ(UprightPlacement::None, uprightPlacementOf(cp)) << std::hex << cp;
  }
}
