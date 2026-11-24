#pragma once

#include <Epub/FootnoteEntry.h>

#include <cstdint>
#include <string>

#include "SerialTestApi.h"  // SERIAL_TEST_API
#include "TouchUi.h"        // CP_TOUCH_UI
#include "components/themes/BaseTheme.h"
#include "util/PagedTextBody.h"

class ButtonEventManager;
class GfxRenderer;
class LanguageHyphenator;
class MappedInputManager;
class Page;

// A footnote opened over the page it is called from: a sheet across the screen in the half the note's marker is not in
// (along the bottom for a marker in the upper half, along the top for one in the lower half, never over the marker's
// line), a rule along the edge it turns to the page, as the reader bars' bottom band has, and the page left standing
// beside it. The note is the book's own text (FootnotePreviews::captureNote), laid out in the reader's font, styled
// where the heap allows and wrapped as plain text where it does not (PagedTextBody), and paged inside the sheet when it
// is longer than half the screen.
//
// The reader hosts it as it hosts the live bars, as its live overlay (EpubReaderActivity::liveNote_): painted over the
// frame on the panel when only the sheet changed, drawn over a page the reader draws afresh when the page is owed (the
// sheet opened over another screen, moved, or shrank), and its input taken in the reader's loop. No screen of its own,
// so the page is neither loaded again nor drawn twice to show a note.
//
// Back, or a tap on the page, closes it. Confirm ("Go to note") follows the note's link as the reader always did, with
// a way back. Page keys page through a long note, and past its last page go on to the page's next note (back past its
// first, to the previous note's last page); past the page's last note, or before its first, they close the sheet, so a
// key that turns pages carries on reading.
class FootnoteSheet {
 public:
  // Where this note stands among the page's notes, and where its sheet goes.
  struct Place {
    uint8_t position = 0;          // this note among the page's notes, from 0
    uint8_t count = 1;             // the page's notes
    int markerY = -1;              // the middle of the marker on the screen; -1 when the page has no box for it
    bool startAtLastPage = false;  // stepped back to: open on the note's last page
  };

  // What one input pass asks of the reader.
  enum class Event : uint8_t { None, Redraw, Close, Follow, Next, Previous };

  // `fontId`: the reader's font, the note's. `marker`, the link's text, titles the sheet.
  FootnoteSheet(GfxRenderer& renderer, MappedInputManager& mappedInput, int fontId, const char* marker,
                const LanguageHyphenator* hyphenator, Place place);

  // Lays the note out: `markup`, captureNote's fragment. `page`, drawn with its top at `pageY`, is the page under the
  // sheet: a line of it one of the sheet's edges would cut is covered whole. Under the reader's RenderLock (its font
  // may be a TrueType face). False (no memory for the note's lines) leaves the reader to follow the link instead, so a
  // note that cannot be shown is never an empty sheet.
  bool layOut(std::string markup, const Page& page, int pageY);

  // One input pass: keys as classified events (the footnote button may be a long press, which fires while the key is
  // still down, and its release must not land here as a Confirm), and on a touch board a tap. Loop task.
  Event loop(ButtonEventManager& events);

  // The sheet, the hint strip's band and the hints into the frame being drawn; no refresh. Under RenderLock.
  void draw() const;

  // Set by layOut().
  bool atTop() const { return sheetAtTop; }
  // This sheet covers every pixel `other` covered: painted over the frame on the panel, it leaves nothing of `other`.
  bool covers(const FootnoteSheet& other) const {
    return sheetAtTop == other.sheetAtTop && coverTop <= other.coverTop && coverBottom >= other.coverBottom;
  }
#if SERIAL_TEST_API
  // For the serial CMD:STATE: which note, which page of it, where.
  void appendDebugState(std::string& out) const;
#endif

 private:
  bool hasNextNote() const { return place.position + 1 < place.count; }
  bool hasPreviousNote() const { return place.position > 0; }
  // A page on, or back; past the note's end the page's next or previous note, past the page's notes the sheet closes.
  Event nextPage();
  Event previousPage();
#if CP_TOUCH_UI
  // The tap, if one came that is not the hint strip's: closes on the page beside the sheet, pages (and steps) in the
  // note's halves. Not compiled without a digitiser.
  Event handleTap();
#endif

  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  const int fontId;
  char marker[FOOTNOTE_NUMBER_LEN] = {};
  const LanguageHyphenator* const hyphenator;
  const Place place;

  PagedTextBody body;
  // Set by layOut(): the screen's content area (the hint strip outside it), the half of it the sheet takes, the sheet
  // within it, the rule along its page-side edge, the note's box, and the rows the sheet's fill covers (its own, grown
  // over a cut page line).
  Rect area{};
  bool sheetAtTop = false;
  Rect sheet{};
  Rect bodyBox{};
  int ruleY = 0;
  int headerY = 0;
  int coverTop = 0;
  int coverBottom = 0;
  int currentPage = 0;
};
