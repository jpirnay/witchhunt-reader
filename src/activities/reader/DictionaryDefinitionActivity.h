#pragma once

// Ported from crosspoint-reader (PR #2583 by Uri Tauber, HTML layout #2836 by
// Uri Tauber). Adapted to this device: a pinned font, and dictionary switching.

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/PagedTextBody.h"

// The font a Latin lookup's definition is rendered in -- fixed, and deliberately
// NOT the reader's font.
//
// A definition is the only text in the firmware that needs the phonetic
// alphabet, and the phonetic alphabet is not free: shipping it in all twenty
// Noto Sans faces cost 124 KB of flash. Pinning the viewer to one face means
// only that face has to carry it. Noto Sans also reads well for reference text
// and looks distinct from the book, which is the right signal for an overlay.
//
// The cost is that a reader using Extra Large text still gets a 14pt
// definition.
//
// A lookup that started on CJK text uses the reader's font instead: Noto Sans has
// no Han, kana or Hangul, and the font that drew the page has the characters
// (DictionaryWordSelectActivity::definitionFontId() chooses).
#define DICTIONARY_FONT_ID NOTOSANS_14_FONT_ID

class LanguageHyphenator;

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain text,
// or HTML too damaged or too large to lay out) is word-wrapped once on entry and
// each page renders spans of the original string, so no per-line copies are held
// (PagedTextBody, which the footnote popup shares).
class DictionaryDefinitionActivity final : public Activity {
 public:
  // `dictionaryName` is shown alongside the page counter and is empty when only
  // one dictionary is installed -- which is also when Confirm does nothing, so
  // the name and the ability to switch appear together or not at all.
  // An empty `headword` draws the header with no title: a CJK definition carries its
  // headwords in the body, which may be in a script the UI font cannot draw.
  // `fontId` draws the body: DICTIONARY_FONT_ID, or the reader's font for a CJK lookup.
  // `hyphenator`: the open book's language's patterns (null for none), which a styled definition hyphenates by.
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, const LanguageHyphenator* hyphenator,
                                        bool htmlDefinition = false, std::string dictionaryName = {},
                                        int fontId = DICTIONARY_FONT_ID)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(std::move(headword)),
        definition(std::move(definition)),
        hyphenator(hyphenator),
        htmlDefinition(htmlDefinition),
        dictionaryName(std::move(dictionaryName)),
        bodyFontId(fontId) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool takesButtonAction(CrossPointSettings::BUTTON_ACTION action, MappedInputManager::Button button) const override;
  void onButtonAction(CrossPointSettings::BUTTON_ACTION action) override;

 private:
  void nextPage();
  void previousPage();

  // Usable body-text rectangle between the header and the button hints. Carries its origin as
  // well as its size because the tap-to-page split has to land on exactly the rectangle the text
  // was drawn in -- one statement of it, read by both.
  struct BodyArea {
    int x;
    int y;
    int width;
    int height;
  };

  BodyArea bodyArea() const;

  const std::string headword;
  // Not const: onEnter() normalizes embedded NULs (StarDict multi-type
  // separators) to newlines so C-string APIs see the whole text, then hands it
  // to the body, which lays it out and keeps what it draws from.
  std::string definition;
  const LanguageHyphenator* const hyphenator;
  const bool htmlDefinition;
  // Empty when there is nothing to switch to.
  const std::string dictionaryName;
  // What the body is laid out and drawn in.
  const int bodyFontId;
  bool canSwitchDictionary() const { return !dictionaryName.empty(); }
  PagedTextBody body;
  // The body could not be laid out (no heap for its line table): the screen
  // shows a low-memory message instead.
  bool layoutFailed = false;
  int currentPage = 0;
  ButtonNavigator buttonNavigator;
};
