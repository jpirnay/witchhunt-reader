#pragma once

#include <cstdint>

// The button scheme every list screen follows, as pure logic: which command a button event means,
// and what the front hint boxes say, both derived from one declaration so they cannot disagree.
// No hardware and no Arduino here, so every rule is pinned by host tests (test/list_grammar).
// The rules are docs/design/list-input-harmonization.md §1.
namespace ListGrammar {

// Up/Down/Left/Right are screen directions (MappedInputManager::Direction), already resolved for
// the orientation by the caller.
enum class Key : uint8_t { Up, Down, Left, Right, Confirm, Back };
enum class Press : uint8_t { Short, Double, Long };
enum class Side : uint8_t { Left, Right };

enum class Command : uint8_t {
  None,
  StepPrev,
  StepNext,
  PagePrev,
  PageNext,
  First,
  Last,
  TabPrev,
  TabNext,
  Activate,
  ActivateLong,
  Back,
  Home,
  LeftAction,
  RightAction,
};

// What the screen declared, reduced to what the rules read.
struct Shape {
  bool leftDeclared = false;   // short Left runs a screen action instead of stepping
  bool rightDeclared = false;  // short Right runs a screen action instead of stepping
  bool confirmLong = false;    // long Confirm has an action of its own
  bool tabbed = false;         // long Up/Down switch tabs instead of jumping to the ends
};

// Whether each declared action applies to the selected row right now.
struct Availability {
  bool left = false;
  bool right = false;
};

// A command and how often to apply it: a Double press of a step is two steps.
struct Result {
  Command command = Command::None;
  uint8_t times = 0;
};

Result commandFor(Key key, Press press, const Shape& shape, const Availability& available);

// What a front Left/Right box shows after its page glyph: the default step, the declared action,
// or nothing (the glyph alone: the pair is overloaded but this side does not apply right now).
enum class FrontLabel : uint8_t { Step, Action, PageOnly };
struct Labels {
  FrontLabel left = FrontLabel::Step;
  FrontLabel right = FrontLabel::Step;
};

Labels labelsFor(const Shape& shape, const Availability& available);

// Row arithmetic. `selectable` may be null (every row is selectable); `ctx` is handed back to it.
// A `from` outside [0, count) is clamped into it first; `direction` is -1 or +1.
using Selectable = bool (*)(const void* ctx, int row);
struct Rows {
  int count = 0;
  int top = 0;    // the first row the screen drew
  int drawn = 1;  // how many rows it drew from there: its window, the size of a page
  Selectable selectable = nullptr;
  const void* ctx = nullptr;
  // Positions before the first row that stay on screen above the rows, such as a tab bar: paging
  // moves the rows below them alone. The window counts them only when it starts at 0.
  int lead = 0;
  // The screen lays rows out a whole page at a time (each page starts at a multiple of `drawn`),
  // so its last screen starts at the last page boundary, not at count - drawn.
  bool pageAligned = false;
};

// One selectable row back (-1) or forward (+1), wrapping round the ends.
int step(const Rows& rows, int from, int direction);
// A page turn: the row to select, and the row the new screen starts from.
struct PageTurn {
  int row = 0;
  int top = 0;
};
// A screenful back (-1) or forward (+1). The screen moves by the window the last render drew
// (forward: the new screen starts with the first row below it; back: one window above it), never
// past the ends of the list, and the selection keeps its line on the screen. Where the screen
// cannot move any further, the selection goes to the first / last row. From a selection outside
// the window (the window is a render old) the page is measured from the selection, which lands on
// the new top row. A header the selection lands on is passed over to the next selectable row,
// else the nearest one before it. A header directly above the new top opens the new screen.
PageTurn page(const Rows& rows, int from, int direction);
int first(const Rows& rows);
int last(const Rows& rows);

// Whether every row fits in the drawn window, so there is no page to jump to.
bool fitsOnePage(const Rows& rows);

// Two Up/Down taps whose presses land this close together are a double-tap, which jumps a page.
// The same window as ButtonEventManager::DOUBLE_WINDOW_MS (ListController asserts it).
constexpr unsigned long kDoubleTapMs = 300;

// Whether an Up/Down tap pressed at `pressMs` completes a double-tap with the tap before it: the
// same key, pressed within kDoubleTapMs, on a list longer than one page. A list that fits one page
// has no page to jump to, so two quick taps there stay two steps. `previousPressMs` 0 means none.
bool completesDoubleTap(Key key, unsigned long pressMs, Key previousKey, unsigned long previousPressMs,
                        const Rows& rows);

}  // namespace ListGrammar
