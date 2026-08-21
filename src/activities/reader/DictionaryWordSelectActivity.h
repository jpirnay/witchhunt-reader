#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"

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
  // Open the dictionary on first use. Idempotent; false when there is none
  // configured or it will not open.
  bool ensureDictionaryOpen();
  // Resolve the highlighted word's LOCATION once the cursor has been still for
  // SPECULATIVE_DEBOUNCE_MS. Index search only -- no inflate, no definition
  // buffer -- so it costs a handful of SD reads and no large allocation.
  void speculateForSelection();
  // Move to another installed dictionary and look the same word up again.
  // Persists the choice: this IS the dictionary setting, and a reader who
  // switched mid-book means it.
  void switchDictionary(int direction);
  // The dictionaries on the card, discovered once on first need. Empty until
  // then; a single entry means there is nothing to switch between.
  const std::vector<DictionaryEntry>& installedDictionaries();
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
  std::vector<DictionaryEntry> dictionaries;
  bool dictionariesDiscovered = false;
  // Set from the definition viewer's result and acted on in loop(), not in the
  // handler itself: the handler runs while the activity stack is being popped,
  // and starting another activity from inside that is asking for trouble.
  int8_t pendingDictionarySwitch = 0;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

  // What the speculative resolve found for the word the cursor is sitting on.
  // Two things come out of it: Confirm skips straight to reading the definition
  // instead of searching the index again, and the highlight can show, before
  // the user presses anything, that a word has no entry.
  enum class Speculation : uint8_t {
    Unknown,  // not resolved yet (still settling, or speculation is off)
    Found,
    Missing,  // the search reached a verdict and the word is not in the dictionary
  };
  static constexpr unsigned long SPECULATIVE_DEBOUNCE_MS = 300;
  Speculation speculation = Speculation::Unknown;
  int speculationIdx = -1;  // word `speculation` describes; -1 = none
  DictLocation speculationLocation;
  std::string speculationHeadword;
  unsigned long lastMoveMs = 0;
  // Speculation is off for this session: no dictionary, it would not open, or
  // its index still has to be built (a multi-second pass that must stay on the
  // explicit Confirm, behind its popup, rather than firing while idle).
  bool speculationDisabled = false;
  // A verdict arrived for the word the cursor is already on, so the highlight
  // has to be redrawn even though the cursor did not move.
  bool speculationRepaintPending = false;

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
