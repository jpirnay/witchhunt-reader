#include "FontScalingTestActivity.h"

#include <CrossPointSettings.h>
#include <GfxRenderer.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Short, because the panel is 480 px wide in portrait and 26 pt runs about 25 px per character.
// Measured against the shipped advances rather than eyeballed -- the 37-character pangram this
// started with came to 926 px, nearly twice the screen, and would have been silently clipped at
// the right edge, quietly excluding the end of every line from the comparison.
//
// Chosen for the glyphs resampling handles worst rather than for meaning: narrow stems (i, l),
// diagonals (V, x), descenders (j, q, y) and a feature a few pixels across (the dot of the i).
constexpr const char* kSpecimen = "Vex jolly quiz";

struct LadderRow {
  uint8_t pt;
  int fontId;
};

// The reader's ladder, exactly as CrossPointSettings::FONT_SIZE_RUNGS defines it. Which rows are
// real and which are scaled is NOT recorded here -- it is asked of the renderer per row, so this
// table cannot disagree with what ships.
constexpr LadderRow kLadderSerif[] = {
    {10, SERIF_10_FONT_ID}, {12, SERIF_12_FONT_ID}, {14, SERIF_14_FONT_ID},
    {16, SERIF_16_FONT_ID}, {18, SERIF_18_FONT_ID}, {20, SERIF_20_FONT_ID},
    {22, SERIF_22_FONT_ID}, {24, SERIF_24_FONT_ID}, {26, SERIF_26_FONT_ID},
};
constexpr LadderRow kLadderNotoSans[] = {
    {10, NOTOSANS_10_FONT_ID}, {12, NOTOSANS_12_FONT_ID}, {14, NOTOSANS_14_FONT_ID},
    {16, NOTOSANS_16_FONT_ID}, {18, NOTOSANS_18_FONT_ID}, {20, NOTOSANS_20_FONT_ID},
    {22, NOTOSANS_22_FONT_ID}, {24, NOTOSANS_24_FONT_ID}, {26, NOTOSANS_26_FONT_ID},
};
constexpr int kLadderCount = sizeof(kLadderSerif) / sizeof(kLadderSerif[0]);
static_assert(sizeof(kLadderNotoSans) == sizeof(kLadderSerif),
              "the two ladders must cover the same sizes or the comparison is not one");
static_assert(kLadderCount == static_cast<int>(CrossPointSettings::FONT_SIZE_COUNT),
              "this screen must show every size the reader offers; a missing row reads as though "
              "the size does not exist, which is how a Noto artifact once looked like it had gone");

}  // namespace

void FontScalingTestActivity::onEnter() {
  Activity::onEnter();
  notoSans_ = false;
  requestUpdate();
}

void FontScalingTestActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
      mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
    notoSans_ = !notoSans_;
    requestUpdate();
  }
}

void FontScalingTestActivity::renderContent() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/false);

  GUI.drawHeader(renderer,
                 Rect{contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight},
                 notoSans_ ? "Noto Sans 10-26pt (R real / S scaled)" : "Serif 10-26pt (R real / S scaled)", nullptr);

  const int leftX = contentRect.x + metrics.verticalSpacing * 2;
  const int bottom = contentRect.y + contentRect.height;
  // A fixed label column so every specimen starts at the same x: the eye compares the left edges
  // of the text, and a ragged start would read as a size difference that is not there.
  const int textX = leftX + contentRect.width * 15 / 100;
  int y = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

  const LadderRow* const ladder = notoSans_ ? kLadderNotoSans : kLadderSerif;
  for (int i = 0; i < kLadderCount; ++i) {
    const LadderRow& r = ladder[i];
    // Asked of the renderer, not assumed: a synthesised size carries its scale on its font ID, so
    // this marker reports what will actually be drawn.
    const bool real = renderer.fontBaseScale(r.fontId) == 1.0f;
    const int lineH = renderer.getLineHeight(r.fontId);
    if (y + lineH > bottom) break;

    char label[12];
    snprintf(label, sizeof(label), "%upt %c", static_cast<unsigned>(r.pt), real ? 'R' : 'S');
    // Baseline-aligned with the specimen rather than the row top, so the marker does not read as
    // part of the specimen's own line.
    renderer.drawText(UI_10_FONT_ID, leftX, y + lineH, label, true);
    // One call whether the size is real or scaled: drawText() routes a scaled ID to the
    // resampling path itself, which is the point of putting the scale on the ID.
    renderer.drawText(r.fontId, textX, y + lineH, kSpecimen, true);
    y += lineH;
  }
}

void FontScalingTestActivity::render(RenderLock&&) {
  renderer.clearScreen();
  renderContent();
  // FULL, not FAST: see the note on the class. A ghost of the previous page sitting inside a stem
  // is indistinguishable from a resampling artifact, and ~2 s a page is the right price for not
  // having to argue about which one you are looking at.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);

  // The whole point is to judge anti-aliased output, so the grayscale passes run unconditionally
  // rather than behind SETTINGS.textAntiAliasing: a comparison made with AA off would say nothing
  // about how the reader looks for anyone who leaves it on, which is the default.
  renderer.renderGrayscalePlanesSequential([this](GfxRenderer::RenderMode) { renderContent(); }, [] { return false; });
}
