#pragma once

#include <string>

#include "ReaderBarsLayout.h"
#include "ReaderFontSizes.h"
#include "ReaderLayoutValues.h"
#include "activities/ActivityResult.h"
#include "components/UiAppHost.h"

class GfxRenderer;
class MappedInputManager;

// The reader bars, whoever shows them: a top bar (back out of the book, its title, the page's star) and a tab bar
// (Navigation, Progress, Layout, Orientation, More, an icon each) painted over the page the reader left on the panel,
// which stays readable between them. Opened by Confirm, a tap in the middle of the page, or a tap on the status bar
// (EpubReaderActivity::openReaderBars).
//
// More hands back to the reader, which opens its full menu: every reader feature stays one step past More. The other
// tabs open in place, above the tab bar. Navigation: the chapter list, the starred pages, a printed page where the
// book has them, and leaving the book, each handed back to the reader. Progress: where the book and the chapter stand,
// the time spent in the book, previous chapter / Go to % / next chapter, and KOReader sync's pull and push where it is
// set up. Orientation: the four ways to hold the device, the current one marked. Layout opens the layout sheet there
// (ReaderLayoutSheet): the font, size, line spacing, margin, darkness, paragraph spacing, alignment, hyphenation and
// the book's style, and whether they are this book's own or the global settings. The reader lays the book out with
// them when the bars close, or at each change where it keeps running under them (`live`, below).
//
// The bars draw over the page and keep no copy of it: a C3 has no 48 KB to spare for one. So anything that would have
// to bring a covered part of the page back closes them instead, and the reader draws the page afresh: Back, a tap on
// the page, and a panel closing (its tab again, or Back while it is open).
//
// Keys work on every board, so the X3 and X4 reach all of it: Left/Right along a row, Up/Down between the rows as they
// lie on screen, Confirm to act, Back to close. On a touch board the keys' cursor stays hidden until a key is pressed.
//
// The state, input and drawing, for a host to run: ReaderBarsActivity, pushed over the reader, which lays the book out
// with the Layout tab's values when the bars close; or the reader itself (EpubReaderActivity::liveBars_), which
// keeps running under them and lays the page out again at each change (`live`).
class ReaderBarsController final : private UiAppHost {
 public:
  // What the reader fills in. The values are formatted; an empty timeValue leaves the time row out.
  struct Info {
    std::string title;
    bool starred = false;
    std::string bookValue;
    std::string chapterValue;
    std::string timeValue;
    // The Layout tab: the values the book is read with and the global ones, whether the book has settings of its own,
    // and the sizes its font offers.
    ReaderLayoutValues bookValues;
    ReaderLayoutValues globalValues;
    bool thisBookOnly = false;
    ReaderSizeList sizes;
    // Navigation offers a printed page (the book has page numbers of its print edition); Progress offers KOReader
    // sync (its account is set up); Orientation marks the one the book is read in.
    bool hasPrintedPages = false;
    bool hasSync = false;
    uint8_t orientation = 0;
  };

  // What one input pass asks of the host: nothing, a redraw, the Layout tab's values applied (live only), or the bars
  // gone, result() saying what the reader does next.
  enum class Event : uint8_t { None, Redraw, LayoutChanged, Closed };

  ReaderBarsController(GfxRenderer& renderer, Info info);

  // `touch`: the cursor waits for a key. `live`: a Layout change is reported at once (Event::LayoutChanged) rather
  // than carried in the result. `fontWaits` (live): a new font is not; it is carried in the result, for the reader to
  // lay the book out with once the bars close (HalCapabilities::defersLiveFontChange).
  void begin(bool touch, bool live, bool fontWaits = false);
  void end();
  Event loop(MappedInputManager& mappedInput);
  // Paints the bars into the frame being drawn. No refresh: the host's.
  void draw();
  const ReaderBarsResult& result() const { return result_; }
  // Live: the values to lay the book out with now. The Layout tab's, but for a font that waits for the bars to close:
  // the book keeps the one it is laid out in.
  ReaderLayoutValues liveLayoutValues() const;
  bool layoutIsThisBooks() const { return thisBookOnly; }
  // Live: the values as the reader lays the book out after a change, for "This book only" to go back to.
  void resync(const ReaderLayoutValues& book, const ReaderLayoutValues& global, const ReaderSizeList& sizes);
  // For the serial CMD:STATE: where the keys' cursor is, the open panel, the page's star, the Layout tab's values.
  void appendDebugState(std::string& out) const;

 private:
  using Tab = ReaderBarsLayout::Tab;
  using Focus = ReaderBarsLayout::Focus;
  using Panel = ReaderBarsLayout::Panel;
  using Button = ReaderBarsLayout::Button;

  Info info;
  bool starred = false;
  bool starToggled = false;
  Panel panel = Panel::None;
  Focus focus = Focus::None;
  Tab focusedTab = Tab::Navigation;
  // The Navigation or Orientation list's row under the cursor while focus is List.
  uint8_t listFocus = 0;
  // The Layout tab's values as the reader is changing them, and the target its cursor is on while focus is Layout.
  ReaderLayoutValues layout{};
  bool thisBookOnly = false;
  int16_t layoutFocus = 0;
  // Set once in begin(): the faces do not change while the bars are open, and the loop task's tap test reads them.
  ReaderBarsLayout::Metrics metrics{};
  freeink::ui::Rect area{};
  freeink::ui::TabItem tabs[ReaderBarsLayout::kTabCount]{};
  // The Layout cells' values, formatted for the pass that draws them (buildBars, on the render task), and the font's
  // name, set where the font changes.
  char cellValues[ReaderLayout::kControlCount][24]{};
  std::string fontLabel;

  ReaderBarsLayout::Model model() const;
  // The rows of list `which` (Navigation, Orientation; none for another panel) into `model`.
  void fillList(ReaderBarsLayout::Model& model, Panel which) const;
  // Panel `wanted` fits the screen, the top bar given up if need be (ReaderBarsLayout::panelFits).
  bool fits(Panel wanted) const;
  ReaderLayoutSheet::Model sheetModel();
  const char* cellValue(ReaderLayout::Control control);
  static void barsScreen(UiScreen& screen, void* user);
  void buildBars(UiScreen& screen);

  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void pressButton(ReaderBarsLayout::Button button);
  // An ACTION_SHEET target's value (ReaderLayoutSheet): the font, "This book only", a control stepped.
  void pressSheet(int16_t value);
  void nextFont();
  static std::string fontOptionLabel(uint8_t font);

  GfxRenderer& renderer;
  bool live_ = false;
  bool fontWaits_ = false;
  // A font chosen on the Layout tab that waits for the bars to close: not the one the book is laid out in.
  bool fontPending() const { return fontWaits_ && layout.font != info.bookValues.font; }
  // What the last input pass did, for loop() to report; and the result once the bars closed.
  bool redraw_ = false;
  bool layoutChanged_ = false;
  bool closed_ = false;
  ReaderBarsResult result_{};

  void handleInput(MappedInputManager& mappedInput);
  void close(ReaderBarsResult::Action action);
  void toggleStar();
  void openTab(Tab tab);
  void pressControl(ReaderLayout::Control control, int delta);
  void flipThisBookOnly();
  void activateFocus();
  void moveAlongRow(int step);
  void moveBetweenRows(int step);
  // The Layout sheet's key targets, as it lies now.
  uint8_t layoutTargets(ReaderLayoutSheet::Target (&out)[ReaderLayoutSheet::kMaxTargets]) const;
};
