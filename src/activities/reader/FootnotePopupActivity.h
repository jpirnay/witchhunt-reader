#pragma once

#include <Epub/FootnoteEntry.h>
#include <Epub/Page.h>

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/PagedTextBody.h"

class LanguageHyphenator;

// A footnote opened over the page it is called from: a sheet across the screen in the half the note's marker is not in
// (along the bottom for a marker in the upper half, along the top for one in the lower half), a rule along the edge it
// turns to the page, as the reader bars' bottom band has, and the page left standing beside it. The note is the book's
// own text (FootnotePreviews::captureNote), laid out in the reader's font, styled where the heap allows and wrapped as
// plain text where it does not (PagedTextBody), and paged inside the sheet when it is longer than half the screen.
//
// Back, or a tap on the page, closes it. Confirm ("Go to note") hands the link back to the reader, which follows it as
// it always did, with a way back. Page keys page through a long note, and past its last page go on to the page's next
// note (back past its first, to the previous note's last page); past the page's last note, or before its first, they
// close the sheet, so a key that turns pages carries on reading. A step is handed back to the reader, which reads the
// next note and opens a sheet for it.
//
// Draws the page itself, from the Page object the reader hands over, rather than painting over whatever the frame
// buffer holds: the reader's buffers may hold a pre-rendered next page or be lent to a build, and a sheet composited
// onto either would show the wrong page under it. That is the word-select overlay's approach, and like it this leaves
// the status bars off while the sheet is open.
class FootnotePopupActivity final : public Activity {
 public:
  // Where this note stands among the page's notes, and where its sheet goes.
  struct Place {
    uint8_t position = 0;          // this note among the page's notes, from 0
    uint8_t count = 1;             // the page's notes
    bool sheetAtTop = false;       // the marker stands in the lower half of the screen
    bool startAtLastPage = false;  // stepped back to: open on the note's last page
  };

  // `page` is drawn in `pageFontId` with its top-left at (pageX, pageY), as the reader drew it. `marker`, the link's
  // text, titles the sheet.
  FootnotePopupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                        int pageFontId, int pageX, int pageY, const char* marker, const LanguageHyphenator* hyphenator,
                        Place place);

  // Lays the note out for the sheet: `markup`, captureNote's fragment. Called by the reader before it pushes this,
  // under its RenderLock: false (no memory for the note's lines) leaves the reader to follow the link instead, so a
  // note that cannot be shown is never an empty sheet.
  bool layOut(std::string markup);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool takesButtonAction(CrossPointSettings::BUTTON_ACTION action, MappedInputManager::Button button) const override;
  void onButtonAction(CrossPointSettings::BUTTON_ACTION action) override;
  // It opens and closes over the reader in one refresh each; an indicator would only flash between them.
  bool suppressesBusyIndicator() const override { return true; }

 private:
  // A page on, or back; past the note's end the page's next or previous note, past the page's notes the sheet closes.
  // False when this sheet is done (stepped or closed).
  bool nextPage();
  bool previousPage();
  bool hasNextNote() const { return place.position + 1 < place.count; }
  bool hasPreviousNote() const { return place.position > 0; }
  void close();
  // Hands the reader a FootnoteResult: `delta` 0 follows the note's link ("Go to note"), +1 / -1 open the page's next
  // or previous note.
  void step(int delta);
  // The tap, if one came that is not the hint strip's: closes on the page beside the sheet, pages (and steps) in the
  // note's halves.
  bool handleTap();

  std::unique_ptr<Page> page;
  const int pageFontId;
  const int pageX;
  const int pageY;
  char marker[FOOTNOTE_NUMBER_LEN] = {};
  const LanguageHyphenator* const hyphenator;
  const Place place;

  PagedTextBody body;
  // Set by layOut(): the screen's content area (the hint strip outside it), the sheet within it, the rule along its
  // page-side edge, and the note's box.
  Rect area{};
  Rect sheet{};
  Rect bodyBox{};
  int ruleY = 0;
  int headerY = 0;
  int currentPage = 0;
};
