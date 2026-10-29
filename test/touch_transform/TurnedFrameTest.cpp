#include <TouchTransform.h>
#include <gtest/gtest.h>

#include "Epub/TurnedFrame.h"

TEST(TurnedFrame, TheTurnedFrameIsOneStepFromTheReaders) {
  EXPECT_EQ(turnedframe::LandscapeCounterClockwise, turnedframe::contentOf(turnedframe::Portrait));
  EXPECT_EQ(turnedframe::Portrait, turnedframe::contentOf(turnedframe::LandscapeClockwise));
  EXPECT_EQ(turnedframe::LandscapeClockwise, turnedframe::contentOf(turnedframe::PortraitInverted));
  EXPECT_EQ(turnedframe::PortraitInverted, turnedframe::contentOf(turnedframe::LandscapeCounterClockwise));
  for (int o = 0; o < 4; ++o) {
    EXPECT_EQ(o, turnedframe::readerOf(turnedframe::contentOf(o)));
    EXPECT_EQ(o, turnedframe::contentOf(turnedframe::readerOf(o)));
  }
}

// The same panel touch, read in the reader's frame (rx, ry) and in the turned one (cx, cy): the content point is the
// reader point turned, rx == W - 1 - cy and ry == cx. This ties the table to the panel transform
// GfxRenderer::rotateCoordinates inverts.
TEST(TurnedFrame, AgreesWithThePanel) {
  const float touches[][2] = {{0.1f, 0.2f}, {0.5f, 0.5f}, {0.9f, 0.7f}};
  for (int o = 0; o < 4; ++o) {
    const int width = o == turnedframe::Portrait || o == turnedframe::PortraitInverted ? 480 : 800;
    for (const auto& touch : touches) {
      int rx = 0;
      int ry = 0;
      int cx = 0;
      int cy = 0;
      touchtransform::tapToLogical(o, 800, 480, touch[0], touch[1], rx, ry);
      touchtransform::tapToLogical(turnedframe::contentOf(o), 800, 480, touch[0], touch[1], cx, cy);
      EXPECT_EQ(width - 1 - cy, rx) << "orientation " << o;
      EXPECT_EQ(cx, ry) << "orientation " << o;
    }
  }
}

TEST(TurnedFrame, ARectAndAPageOrigin) {
  const turnedframe::Rect rect = turnedframe::toReader(480, {10, 20, 30, 40});
  EXPECT_EQ(420, rect.x);
  EXPECT_EQ(10, rect.y);
  EXPECT_EQ(40, rect.width);
  EXPECT_EQ(30, rect.height);
  const turnedframe::Offsets offsets = turnedframe::contentOffsets(480, 10, 20, 460);
  EXPECT_EQ(20, offsets.x);
  EXPECT_EQ(10, offsets.y);
}
