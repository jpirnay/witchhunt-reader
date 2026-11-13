#pragma once

// The box a highlighted run of words is drawn in: its glyphs' box grown by 2 px for a little air around them, kept on
// the panel. The dictionary's selection (DictionaryWordSelectActivity) and the book search's matches
// (EpubReaderActivity::drawBookSearchOverlay) draw the same box. False when nothing of it is on the panel.
struct WordOutlineBox {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

inline bool wordOutlineBox(const int x, const int y, const int width, const int height, const int screenWidth,
                           const int screenHeight, WordOutlineBox& out) {
  out = {x - 2, y - 2, width + 4, height + 4};
  if (out.x < 0) {
    out.width += out.x;
    out.x = 0;
  }
  if (out.y < 0) {
    out.height += out.y;
    out.y = 0;
  }
  if (out.x + out.width > screenWidth) out.width = screenWidth - out.x;
  if (out.y + out.height > screenHeight) out.height = screenHeight - out.y;
  return out.width > 0 && out.height > 0;
}
