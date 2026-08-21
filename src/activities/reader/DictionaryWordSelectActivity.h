#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "util/Dictionary.h"

// Word selection over the current reader page: Left/Right step through words in
// reading order, Up/Down jump rows, Confirm looks the word up and opens
// DictionaryDefinitionActivity, Back returns to the reader.
//
// Owns the Page it draws, so the reader can hand over the page it already has
// laid out instead of the overlay reloading it.
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int fontId, int marginLeft, int marginTop)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        fontId(fontId),
        marginLeft(marginLeft),
        marginTop(marginTop) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Screen box of one selectable word. `text` points into the owned Page's
  // TextBlock arena (NUL-terminated), valid for this activity's lifetime.
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
    uint16_t row;
    const char* text;
    // How the page drew this word, so the highlight can redraw it in white and
    // land on the same glyphs -- a heading uses its own font, and a word may be
    // bold, italic or scaled by the publisher's font sizes.
    int fontId;
    EpdFontFamily::Style style;
    float scale;
  };

  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void extractWords();
  int closestInRow(uint16_t row, int centerX) const;
  void moveVertical(int direction);
  void performLookup();
  bool drawHighlightWithSnapshot();
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int fontId;
  const int marginLeft;
  const int marginTop;

  std::vector<WordBox> words;
  // Word boxes are measured once the font cache has been prewarmed for this
  // page, which only happens inside render(); onEnter() has no framebuffer to
  // scan against. Until then there is nothing to highlight.
  bool wordsExtracted = false;
  int selected = 0;
  uint16_t rowCount = 0;

  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint: the pixels under the current highlight box,
  // so a cursor move restores them and repaints only the two affected boxes
  // instead of re-running the full two-pass page render (which also reloads
  // every SD-font glyph on the page). snapshotIdx is the word whose under-pixels
  // are saved; -1 means the framebuffer no longer holds a clean page (popup
  // drawn, sub-activity shown) and the next render must be a full one.
  static constexpr size_t SNAPSHOT_CAPACITY = 2048;  // packed 1bpp: 16384 pixels
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;
};
