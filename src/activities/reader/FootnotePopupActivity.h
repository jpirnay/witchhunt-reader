#pragma once

#include <Epub/Page.h>

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/PagedTextBody.h"

class LanguageHyphenator;

// A footnote opened over the page it is called from: a sheet along the bottom of the screen, a rule along its top edge
// as the reader bars' bottom band has, the page left standing above it. The note is the book's own text
// (FootnotePreviews::captureNote), laid out in the reader's font, styled where the heap allows and wrapped as plain
// text where it does not (PagedTextBody), and paged inside the sheet when it is longer than half the screen.
//
// Back, or a tap on the page, closes it. Confirm ("Go to note") hands the link back to the reader, which follows it as
// it always did, with a way back. Page keys page through a long note; past either end they close the sheet, so a key
// that turns pages carries on reading.
//
// Draws the page itself, from the Page object the reader hands over, rather than painting over whatever the frame
// buffer holds: the reader's buffers may hold a pre-rendered next page or be lent to a build, and a sheet composited
// onto either would show the wrong page under it. That is the word-select overlay's approach, and like it this leaves
// the status bars off while the sheet is open.
class FootnotePopupActivity final : public Activity {
 public:
  // `page` is drawn in `pageFontId` with its top-left at (pageX, pageY), as the reader drew it. `marker` titles the
  // sheet; `href` is what Confirm hands back. `markup`: the note (captureNote's fragment).
  FootnotePopupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                        int pageFontId, int pageX, int pageY, std::string marker, std::string href, std::string markup,
                        const LanguageHyphenator* hyphenator);

  // Lays the note out for the sheet. Called by the reader before it pushes this, under its RenderLock: false (no memory
  // for the note's lines) leaves the reader to follow the link instead, so a note that cannot be shown is never an
  // empty sheet.
  bool layOut();

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool takesButtonAction(CrossPointSettings::BUTTON_ACTION action, MappedInputManager::Button button) const override;
  void onButtonAction(CrossPointSettings::BUTTON_ACTION action) override;
  // It opens and closes over the reader in one refresh each; an indicator would only flash between them.
  bool suppressesBusyIndicator() const override { return true; }

 private:
  // A page on, or back; past either end the sheet closes, and these return false.
  bool nextPage();
  bool previousPage();
  void close();
  void goToNote();
  // The tap, if one came that is not the hint strip's: closes on the page above the sheet, pages in the note's halves.
  bool handleTap();

  std::unique_ptr<Page> page;
  const int pageFontId;
  const int pageX;
  const int pageY;
  const std::string marker;
  const std::string href;
  // The note until layOut() hands it to the body.
  std::string markup;
  const LanguageHyphenator* const hyphenator;

  PagedTextBody body;
  // Set by layOut(): the screen's content area (the hint strip outside it), the sheet within it, and the note's box.
  Rect area{};
  Rect sheet{};
  Rect bodyBox{};
  int headerY = 0;
  int currentPage = 0;
};
