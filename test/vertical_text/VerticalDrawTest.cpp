#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <string>

#include "AllocationCounter.h"
#include "Epub/TurnedFrame.h"
#include "Epub/VerticalDraw.h"

namespace {

constexpr int kFontId = 1;

void expectCall(const GfxRenderer::DrawCall& call, const std::string& text, const GfxRenderer::Orientation frame,
                const int x, const int y) {
  EXPECT_EQ(text, call.text);
  EXPECT_EQ(frame, call.frame) << text;
  EXPECT_EQ(x, call.x) << text;
  EXPECT_EQ(y, call.y) << text;
}

}  // namespace

// em 10, ascender 16: baseline 50 + 16 = 66, axis 66 - (380 + 50) / 100 = 62, upright x = 479 - 62 - 10 / 2 = 412,
// upright y = pen + (880 + 50) / 100 - 16 = pen - 7. Pens: 日 100, 本 110, Latin 120, 語 170.
TEST(VerticalDraw, UprightPiecesDrawInTheReadersFrameRunsInTheTurnedFrame) {
  GfxRenderer r;
  r.setOrientation(GfxRenderer::Portrait);
  r.setDrawOrientation(GfxRenderer::LandscapeCounterClockwise);
  drawTextVertical(r, kFontId, 100, 50, "日本Latin語", EpdFontFamily::REGULAR, 1.0f);
  ASSERT_EQ(4u, r.drawCalls.size());
  expectCall(r.drawCalls[0], "日", GfxRenderer::Portrait, 412, 93);
  expectCall(r.drawCalls[1], "本", GfxRenderer::Portrait, 412, 103);
  expectCall(r.drawCalls[2], "Latin", GfxRenderer::LandscapeCounterClockwise, 120, 50);
  expectCall(r.drawCalls[3], "語", GfxRenderer::Portrait, 412, 163);
  EXPECT_EQ(GfxRenderer::LandscapeCounterClockwise, r.getOrientation());
  EXPECT_EQ(80, verticalTextAdvance(r, kFontId, "日本Latin語", EpdFontFamily::REGULAR, 1.0f));
}

// A scan pass draws the text as is, and must also ask for 一 in the word's style: the draw pass measures the column
// with it, and an unprewarmed glyph is a miss on an SD or TTF font.
TEST(VerticalDraw, AScanPassRecordsTheEmGlyphInTheWordsStyle) {
  GfxRenderer r;
  r.scanning = true;
  drawTextVertical(r, kFontId, 100, 50, "日本", EpdFontFamily::BOLD, 1.0f);
  bool askedForOneInBold = false;
  for (const auto& q : r.hasGlyphQueries) askedForOneInBold |= q.first == 0x4E00 && q.second == EpdFontFamily::BOLD;
  EXPECT_TRUE(askedForOneInBold);
  ASSERT_EQ(1u, r.drawCalls.size());
  EXPECT_EQ("日本", r.drawCalls[0].text);
}

TEST(VerticalDraw, AScaledColumnRoundsOnceAsLayoutDoes) {
  GfxRenderer r;
  r.setOrientation(GfxRenderer::Portrait);
  r.setDrawOrientation(GfxRenderer::LandscapeCounterClockwise);
  EXPECT_EQ(15, columnEm(r, kFontId, EpdFontFamily::REGULAR, 1.5f));
  drawTextVertical(r, kFontId, 100, 50, "日本Latin語", EpdFontFamily::REGULAR, 1.5f);
  ASSERT_EQ(4u, r.drawCalls.size());
  expectCall(r.drawCalls[0], "日", GfxRenderer::Portrait, 404, 89);
  expectCall(r.drawCalls[1], "本", GfxRenderer::Portrait, 404, 104);
  expectCall(r.drawCalls[2], "Latin", GfxRenderer::LandscapeCounterClockwise, 130, 50);
  expectCall(r.drawCalls[3], "語", GfxRenderer::Portrait, 404, 194);
  for (const auto& call : r.drawCalls) EXPECT_EQ(1.5f, call.scale);
  EXPECT_EQ(120, verticalTextAdvance(r, kFontId, "日本Latin語", EpdFontFamily::REGULAR, 1.5f));
}

TEST(VerticalDraw, ALongSidewaysRunIsDrawnInPiecesAtTheirPens) {
  GfxRenderer r;
  r.setOrientation(GfxRenderer::Portrait);
  r.setDrawOrientation(GfxRenderer::LandscapeCounterClockwise);
  const std::string text(70, 'a');
  drawTextVertical(r, kFontId, 100, 50, text.c_str(), EpdFontFamily::REGULAR, 1.0f);
  ASSERT_EQ(2u, r.drawCalls.size());
  EXPECT_EQ(64u, r.drawCalls[0].text.size());
  EXPECT_EQ(100, r.drawCalls[0].x);
  EXPECT_EQ(6u, r.drawCalls[1].text.size());
  EXPECT_EQ(740, r.drawCalls[1].x);
  EXPECT_EQ(700, verticalTextAdvance(r, kFontId, text.c_str(), EpdFontFamily::REGULAR, 1.0f));
}

TEST(VerticalDraw, ACombiningMarkStaysOnItsBase) {
  GfxRenderer r;
  r.setOrientation(GfxRenderer::Portrait);
  r.setDrawOrientation(GfxRenderer::LandscapeCounterClockwise);
  const std::string text = "か\u3099";
  drawTextVertical(r, kFontId, 100, 50, text.c_str(), EpdFontFamily::REGULAR, 1.0f);
  ASSERT_EQ(1u, r.drawCalls.size());
  EXPECT_EQ(6u, r.drawCalls[0].text.size());
  EXPECT_EQ(10, verticalTextAdvance(r, kFontId, text.c_str(), EpdFontFamily::REGULAR, 1.0f));
}

TEST(VerticalDraw, TheReadersFrameIsTheHeldOne) {
  for (int held = 0; held < 4; ++held) {
    GfxRenderer r;
    r.setOrientation(static_cast<GfxRenderer::Orientation>(held));
    r.setDrawOrientation(static_cast<GfxRenderer::Orientation>(turnedframe::contentOf(held)));
    drawTextVertical(r, kFontId, 100, 50, "日", EpdFontFamily::REGULAR, 1.0f);
    ASSERT_EQ(1u, r.drawCalls.size());
    const int width = held == turnedframe::Portrait || held == turnedframe::PortraitInverted ? 480 : 800;
    expectCall(r.drawCalls[0], "日", static_cast<GfxRenderer::Orientation>(held), width - 1 - 62 - 5, 93);
    EXPECT_EQ(turnedframe::contentOf(held), static_cast<int>(r.getOrientation()));
  }
}

TEST(VerticalDraw, DrawingAllocatesNothing) {
  GfxRenderer r;
  r.setOrientation(GfxRenderer::Portrait);
  r.setDrawOrientation(GfxRenderer::LandscapeCounterClockwise);
  r.drawCalls.reserve(16);
  const long live = liveAllocations();
  const long all = allocations();
  drawTextVertical(r, kFontId, 100, 50, "日本Latin語", EpdFontFamily::REGULAR, 1.0f);
  EXPECT_EQ(all, allocations());
  EXPECT_EQ(live, liveAllocations());
}

// A sideways run past std::string's small-string buffer: the recorder's copy of the 64 B piece allocates (the 6 B
// rest fits the buffer). That cost is measured with the same string in a direct drawText; the draw path adds nothing
// to it, where a std::string built per piece would.
TEST(VerticalDraw, ALongSidewaysRunAllocatesOnlyWhatTheRecorderDoes) {
  const std::string run(70, 'a');
  const std::string piece(64, 'a');

  GfxRenderer baseline;
  baseline.drawCalls.reserve(16);
  const long baseLive = liveAllocations();
  const long baseAll = allocations();
  baseline.drawText(kFontId, 100, 50, piece.c_str());
  const long recorderAll = allocations() - baseAll;
  const long recorderLive = liveAllocations() - baseLive;
  ASSERT_GT(recorderAll, 0) << "the recorder's copy of a 64 B string must allocate, or this measures nothing";

  GfxRenderer r;
  r.setOrientation(GfxRenderer::Portrait);
  r.setDrawOrientation(GfxRenderer::LandscapeCounterClockwise);
  r.drawCalls.reserve(16);
  const long live = liveAllocations();
  const long all = allocations();
  drawTextVertical(r, kFontId, 100, 50, run.c_str(), EpdFontFamily::REGULAR, 1.0f);
  ASSERT_EQ(2u, r.drawCalls.size());
  EXPECT_EQ(64u, r.drawCalls[0].text.size());
  EXPECT_EQ(6u, r.drawCalls[1].text.size());
  EXPECT_EQ(all + recorderAll, allocations());
  EXPECT_EQ(live + recorderLive, liveAllocations());
}
