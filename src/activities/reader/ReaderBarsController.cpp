#include "ReaderBarsController.h"

#include <Epub/LineSpacing.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "ReaderLayoutControls.h"
#include "SdCardFontGlobals.h"
#include "SerialTestApi.h"
#include "activities/ListTabBar.h"
#include "activities/RenderLock.h"
#include "components/UIThemeTokens.h"
#include "util/SerialCommandGrammar.h"

namespace fui = freeink::ui;
using namespace ReaderBarsLayout;
using ReaderLayout::Control;

namespace {

// The labels as the settings store the values: TEXT_DARKNESS and PARAGRAPH_ALIGNMENT, in their stored order.
constexpr StrId kDarknessLabels[] = {StrId::STR_NORMAL, StrId::STR_DARK, StrId::STR_EXTRA_DARK, StrId::STR_MAX_DARK,
                                     StrId::STR_LIGHT};
static_assert(sizeof(kDarknessLabels) / sizeof(kDarknessLabels[0]) == CrossPointSettings::TEXT_DARKNESS_COUNT);
constexpr StrId kAlignmentLabels[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER,
                                      StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE};
static_assert(sizeof(kAlignmentLabels) / sizeof(kAlignmentLabels[0]) == CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT);
// The controls' labels, in ReaderLayout::Control order.
constexpr StrId kControlLabels[] = {
    StrId::STR_LAYOUT_SIZE,          StrId::STR_LAYOUT_LINES, StrId::STR_LAYOUT_MARGIN,  StrId::STR_LAYOUT_DARKNESS,
    StrId::STR_LAYOUT_PARAGRAPH_GAP, StrId::STR_LAYOUT_ALIGN, StrId::STR_LAYOUT_HYPHENS, StrId::STR_LAYOUT_BOOK_STYLE};
static_assert(sizeof(kControlLabels) / sizeof(kControlLabels[0]) == ReaderLayout::kControlCount);

const char* text(const StrId id) { return I18n::getInstance().get(id); }
const char* onOff(const bool on) { return text(on ? StrId::STR_ON : StrId::STR_OFF); }

}  // namespace

ReaderBarsController::ReaderBarsController(GfxRenderer& renderer, Info info)
    : UiAppHost(renderer), renderer(renderer), info(std::move(info)) {}

void ReaderBarsController::begin(const bool touch, const bool live) {
  live_ = live;
  starred = info.starred;
  thisBookOnly = info.thisBookOnly;
  layout = thisBookOnly ? info.bookValues : info.globalValues;
  // Keys show their cursor from the start where they are the only way in; a touch board waits for a key.
  focus = touch ? Focus::None : Focus::Tabs;

  resetUi();
  area = fui::Rect{0, 0, static_cast<int16_t>(renderer.getScreenWidth()),
                   static_cast<int16_t>(renderer.getScreenHeight())};
  metrics = ReaderBarsLayout::metricsFor(uiTarget, uiThemeTokens(uiTarget), ListTabBar::HEIGHT, touch, area.width,
                                         area.height);
  fontLabel = fontOptionLabel(layout.font);
  // Three actions, one handler: FreeInkApp keeps 8 a screen and drops any past that without a sign.
  app.on(ACTION_BAR, &ReaderBarsController::onAction, this);
  app.on(ACTION_TAB, &ReaderBarsController::onAction, this);
  app.on(ReaderLayoutSheet::ACTION_SHEET, &ReaderBarsController::onAction, this);
  app.setScreen(&ReaderBarsController::barsScreen, this);
}

void ReaderBarsController::appendDebugState(std::string& out) const {
  static constexpr const char* kFocus[] = {"none", "exit", "star", "tabs", "prev", "goto", "next", "layout"};
  static constexpr const char* kTab[] = {"contents", "progress", "layout", "more"};
  static constexpr const char* kPanel[] = {"none", "progress", "layout"};
  SerialCommandGrammar::appendField(out, "focus", kFocus[static_cast<uint8_t>(focus)]);
  SerialCommandGrammar::appendField(out, "tab", kTab[static_cast<uint8_t>(focusedTab)]);
  SerialCommandGrammar::appendField(out, "panel", kPanel[static_cast<uint8_t>(panel)]);
  SerialCommandGrammar::appendField(out, "starred", starred ? 1L : 0L);
#if SERIAL_TEST_API
  // The Layout tab's values: what a device test checks a press against. Release builds keep the short reply.
  if (panel != Panel::Layout) return;
  SerialCommandGrammar::appendField(out, "target", static_cast<long>(layoutFocus));
  SerialCommandGrammar::appendField(out, "own", thisBookOnly ? 1L : 0L);
  SerialCommandGrammar::appendField(out, "size", static_cast<long>(layout.fontSize));
  SerialCommandGrammar::appendField(out, "lines", static_cast<long>(layout.lineSpacing));
  SerialCommandGrammar::appendField(out, "margin", static_cast<long>(layout.marginHorizontal));
  SerialCommandGrammar::appendField(out, "darkness", static_cast<long>(layout.textDarkness));
  SerialCommandGrammar::appendField(out, "paragraphs", layout.extraParagraphSpacing ? 1L : 0L);
  SerialCommandGrammar::appendField(out, "align", static_cast<long>(layout.paragraphAlignment));
  SerialCommandGrammar::appendField(out, "hyphens", layout.hyphenation ? 1L : 0L);
  SerialCommandGrammar::appendField(out, "bookstyle", layout.embeddedStyle ? 1L : 0L);
#endif
}

void ReaderBarsController::end() { closeRouting(); }

ReaderBarsLayout::Model ReaderBarsController::model() const {
  ReaderBarsLayout::Model model;
  model.title = info.title.c_str();
  model.starred = starred;
  model.panel = panel;
  model.focus = focus;
  model.focusedTab = focusedTab;
  model.bookLabel = tr(STR_BOOK);
  model.bookValue = info.bookValue.c_str();
  model.chapterLabel = tr(STR_CHAPTER);
  model.chapterValue = info.chapterValue.empty() ? nullptr : info.chapterValue.c_str();
  model.timeLabel = tr(STR_READING_STATS_TOTAL_TIME);
  model.timeValue = info.timeValue.empty() ? nullptr : info.timeValue.c_str();
  model.goToLabel = tr(STR_GO_TO_PERCENT);
  return model;
}

ReaderLayoutSheet::Model ReaderBarsController::sheetModel() {
  ReaderLayoutSheet::Model sheet;
  sheet.font = {text(StrId::STR_LAYOUT_FONT), fontLabel.c_str()};
  sheet.thisBook = {text(StrId::STR_THIS_BOOK_ONLY), onOff(thisBookOnly)};
  sheet.focus = focus == Focus::Layout ? layoutFocus : int16_t{-1};
  for (uint8_t c = 0; c < ReaderLayout::kControlCount; ++c) {
    sheet.controls[c] = {text(kControlLabels[c]), cellValue(static_cast<Control>(c))};
  }
  return sheet;
}

// A control's value as its cell shows it. Numbers go into cellValues, kept for the pass that draws them.
const char* ReaderBarsController::cellValue(const Control control) {
  char* number = cellValues[static_cast<uint8_t>(control)];
  switch (control) {
    case Control::Size:
      snprintf(number, sizeof(cellValues[0]), "%u", static_cast<unsigned>(info.sizes.snap(layout.fontSize)));
      return number;
    case Control::Line: {
      const char* percent = lineSpacingStepLabel(layout.lineSpacing);
      return percent ? percent : text(StrId::STR_BOOK_S_STYLE);
    }
    case Control::Margin:
      snprintf(number, sizeof(cellValues[0]), "%u", static_cast<unsigned>(layout.marginHorizontal));
      return number;
    case Control::Darkness:
      return text(
          kDarknessLabels[layout.textDarkness < CrossPointSettings::TEXT_DARKNESS_COUNT ? layout.textDarkness : 0]);
    case Control::Paragraph:
      return onOff(layout.extraParagraphSpacing);
    case Control::Align:
      return text(kAlignmentLabels[layout.paragraphAlignment < CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT
                                       ? layout.paragraphAlignment
                                       : 0]);
    case Control::Hyphenation:
      return onOff(layout.hyphenation);
    case Control::BookStyle:
      return onOff(layout.embeddedStyle);
    case Control::Count:
      break;
  }
  return "";
}

void ReaderBarsController::barsScreen(UiScreen& screen, void* user) {
  static_cast<ReaderBarsController*>(user)->buildBars(screen);
}

void ReaderBarsController::buildBars(UiScreen& screen) {
  const char* labels[kTabCount] = {text(StrId::STR_CONTENTS), text(StrId::STR_READING_STATS_PROGRESS),
                                   text(StrId::STR_LAYOUT), text(StrId::STR_MORE)};
  for (uint8_t i = 0; i < kTabCount; ++i) {
    tabs[i].label = labels[i];
    tabs[i].value = static_cast<int16_t>(i);
    // The open panel's tab, or the one under the keys' cursor.
    const auto tab = static_cast<Tab>(i);
    tabs[i].selected = (panel == Panel::Progress && tab == Tab::Progress) ||
                       (panel == Panel::Layout && tab == Tab::Layout) || (focus == Focus::Tabs && tab == focusedTab);
  }
  const fui::TabBarProps props = ListTabBar::props(screen, tabs, kTabCount, ACTION_TAB, focus == Focus::Tabs);
  const ReaderLayoutSheet::Model sheet = sheetModel();
  ReaderBarsLayout::Model bars = model();
  bars.layout = &sheet;
  ReaderBarsLayout::build(screen, bars, metrics, props);
}

void ReaderBarsController::draw() { renderUi(); }

void ReaderBarsController::close(const ReaderBarsResult::Action action) {
  ReaderBarsResult closing;
  closing.action = action;
  closing.starToggled = starToggled;
  const ReaderLayoutValues& opened = info.thisBookOnly ? info.bookValues : info.globalValues;
  closing.layoutChanged = thisBookOnly != info.thisBookOnly || !(layout == opened);
  closing.thisBookOnly = thisBookOnly;
  closing.layout = layout;
  // Live, the reader applied every change as it was made: nothing is left to apply.
  if (live_) closing.layoutChanged = false;
  result_ = closing;
  closed_ = true;
}

void ReaderBarsController::toggleStar() {
  {
    RenderLock lock;
    starred = !starred;
    starToggled = !starToggled;
  }
  redraw_ = true;
}

void ReaderBarsController::openTab(const Tab tab) {
  switch (tab) {
    case Tab::Contents:
      close(ReaderBarsResult::Action::Contents);
      return;
    case Tab::More:
      close(ReaderBarsResult::Action::More);
      return;
    case Tab::Progress:
    case Tab::Layout: {
      const Panel wanted = tab == Tab::Progress ? Panel::Progress : Panel::Layout;
      // A screen too short for the Layout sheet (the Murphy held sideways) has the settings in the reader menu.
      if (wanted == Panel::Layout && !ReaderBarsLayout::layoutFits(metrics, area.height)) {
        close(ReaderBarsResult::Action::More);
        return;
      }
      // A panel closing, or giving way to the other, would have to bring back the page it covered: the reader draws
      // it instead. The other panel's tab closes the bars too: the next open shows it.
      if (panel != Panel::None) {
        close(ReaderBarsResult::Action::None);
        return;
      }
      {
        RenderLock lock;
        panel = wanted;
        layoutFocus = 0;
        if (focus != Focus::None) focus = wanted == Panel::Progress ? Focus::GoTo : Focus::Layout;
      }
      redraw_ = true;
      return;
    }
    case Tab::Count:
      return;
  }
}

void ReaderBarsController::pressControl(const Control control, const int delta) {
  {
    RenderLock lock;
    ReaderLayout::press(layout, control, delta, info.sizes);
  }
  redraw_ = true;
  layoutChanged_ = live_;
}

// Off: the book follows the global settings, which the sheet shows from then on and changes for every book. On: the
// values on show become the book's own.
void ReaderBarsController::flipThisBookOnly() {
  {
    RenderLock lock;
    thisBookOnly = !thisBookOnly;
    if (!thisBookOnly) layout = info.globalValues;
  }
  redraw_ = true;
  layoutChanged_ = live_;
}

void ReaderBarsController::onAction(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ReaderBarsController*>(user);
  switch (event.action) {
    case ACTION_BAR:
      self->pressButton(static_cast<Button>(event.value));
      return;
    case ACTION_TAB:
      if (event.value >= 0 && event.value < kTabCount) self->openTab(static_cast<Tab>(event.value));
      return;
    case ReaderLayoutSheet::ACTION_SHEET:
      self->pressSheet(event.value);
      return;
    default:
      return;
  }
}

void ReaderBarsController::pressButton(const Button button) {
  switch (button) {
    case Button::Exit:
      close(ReaderBarsResult::Action::Exit);
      return;
    case Button::Star:
      toggleStar();
      return;
    case Button::PrevChapter:
      close(ReaderBarsResult::Action::PrevChapter);
      return;
    case Button::GoTo:
      close(ReaderBarsResult::Action::GoToPercent);
      return;
    case Button::NextChapter:
      close(ReaderBarsResult::Action::NextChapter);
      return;
  }
}

void ReaderBarsController::pressSheet(const int16_t value) {
  if (value == ReaderLayoutSheet::kFontValue) {
    nextFont();
    return;
  }
  if (value == ReaderLayoutSheet::kThisBookValue) {
    flipThisBookOnly();
    return;
  }
  const int16_t control = static_cast<int16_t>(value & ~ReaderLayoutSheet::kUpFlag);
  if (control < 0 || control >= ReaderLayout::kControlCount) return;
  pressControl(static_cast<Control>(control), (value & ReaderLayoutSheet::kUpFlag) != 0 ? +1 : -1);
}

std::string ReaderBarsController::fontOptionLabel(const uint8_t font) {
  return font == 0 ? std::string(tr(STR_BOOK_FONTS)) : fontFamilyOptionLabel(static_cast<uint8_t>(font - 1));
}

// The next font round the list, the book's own first: few enough to go round rather than open a list for. The size
// steps through what the new family offers.
void ReaderBarsController::nextFont() {
  const uint8_t options = static_cast<uint8_t>(1 + fontFamilyOptionCount());
  const uint8_t next = static_cast<uint8_t>((layout.font + 1) % options);
  const ReaderSizeList sizes = sdFontSystem.sizeListFor(
      next == 0 ? SETTINGS.sdFontFamilyName : fontFamilyOptionSdName(static_cast<uint8_t>(next - 1)));
  std::string label = fontOptionLabel(next);
  {
    RenderLock lock;
    layout.font = next;
    info.sizes = sizes;
    fontLabel = std::move(label);
  }
  redraw_ = true;
  layoutChanged_ = live_;
}

uint8_t ReaderBarsController::layoutTargets(ReaderLayoutSheet::Target (&out)[ReaderLayoutSheet::kMaxTargets]) const {
  return ReaderLayoutSheet::targets(ReaderBarsLayout::sheetRect(area, model(), metrics), metrics.sheet, out);
}

void ReaderBarsController::activateFocus() {
  switch (focus) {
    case Focus::None:
      return;
    case Focus::Exit:
      pressButton(Button::Exit);
      return;
    case Focus::Star:
      pressButton(Button::Star);
      return;
    case Focus::Tabs:
      openTab(focusedTab);
      return;
    case Focus::PrevChapter:
      pressButton(Button::PrevChapter);
      return;
    case Focus::GoTo:
      pressButton(Button::GoTo);
      return;
    case Focus::NextChapter:
      pressButton(Button::NextChapter);
      return;
    case Focus::Layout: {
      ReaderLayoutSheet::Target all[ReaderLayoutSheet::kMaxTargets];
      const uint8_t count = layoutTargets(all);
      if (layoutFocus >= 0 && layoutFocus < count) pressSheet(all[layoutFocus].value);
      return;
    }
  }
}

// Left/Right: along the row the cursor is on, stopping at its ends.
void ReaderBarsController::moveAlongRow(const int step) {
  ReaderLayoutSheet::Target all[ReaderLayoutSheet::kMaxTargets];
  const uint8_t count = focus == Focus::Layout ? layoutTargets(all) : 0;
  RenderLock lock;
  switch (focus) {
    case Focus::None:
      focus = Focus::Tabs;
      return;
    case Focus::Exit:
    case Focus::Star:
      if (step != 0) focus = step < 0 ? Focus::Exit : Focus::Star;
      return;
    case Focus::Tabs: {
      const int next = static_cast<int>(focusedTab) + step;
      if (next >= 0 && next < kTabCount) focusedTab = static_cast<Tab>(next);
      return;
    }
    case Focus::PrevChapter:
      if (step > 0) focus = Focus::GoTo;
      return;
    case Focus::GoTo:
      if (step != 0) focus = step < 0 ? Focus::PrevChapter : Focus::NextChapter;
      return;
    case Focus::NextChapter:
      if (step < 0) focus = Focus::GoTo;
      return;
    case Focus::Layout:
      if (step != 0) layoutFocus = ReaderLayoutSheet::targetAlong(all, count, layoutFocus, step);
      return;
  }
}

// Up/Down: between the rows as they lie on screen, the top bar, the open panel's rows, and the tabs. The Progress
// buttons are entered on Go to %, the top bar on the star, the Layout sheet on its nearest target.
void ReaderBarsController::moveBetweenRows(const int step) {
  ReaderLayoutSheet::Target all[ReaderLayoutSheet::kMaxTargets];
  const uint8_t count = panel == Panel::Layout ? layoutTargets(all) : 0;
  const bool topBar = ReaderBarsLayout::topRect(area, model(), metrics).height > 0;
  RenderLock lock;
  if (focus == Focus::None || step == 0) {
    if (focus == Focus::None) focus = Focus::Tabs;
    return;
  }
  const auto enterTop = [&] {
    if (topBar) focus = Focus::Star;
  };
  const auto enterPanelFromBelow = [&] {
    if (panel == Panel::Progress) {
      focus = Focus::GoTo;
    } else if (panel == Panel::Layout && count > 0) {
      // The last row's target nearest the middle.
      int16_t best = static_cast<int16_t>(count - 1);
      for (int16_t i = 0; i < count; ++i) {
        if (all[i].row == all[count - 1].row && std::abs(all[i].centre - 500) < std::abs(all[best].centre - 500)) {
          best = i;
        }
      }
      focus = Focus::Layout;
      layoutFocus = best;
    } else {
      enterTop();
    }
  };
  switch (focus) {
    case Focus::None:
      return;
    case Focus::Exit:
    case Focus::Star:
      if (step < 0) return;
      if (panel == Panel::Progress) {
        focus = Focus::GoTo;
      } else if (panel == Panel::Layout) {
        focus = Focus::Layout;
        layoutFocus = 0;
      } else {
        focus = Focus::Tabs;
      }
      return;
    case Focus::PrevChapter:
    case Focus::GoTo:
    case Focus::NextChapter:
      if (step < 0) {
        enterTop();
      } else {
        focus = Focus::Tabs;
      }
      return;
    case Focus::Tabs:
      if (step < 0) enterPanelFromBelow();
      return;
    case Focus::Layout: {
      const int16_t next = ReaderLayoutSheet::targetAcross(all, count, layoutFocus, step);
      if (next >= 0) {
        layoutFocus = next;
      } else if (step < 0) {
        enterTop();
      } else {
        focus = Focus::Tabs;
      }
      return;
    }
  }
}

void ReaderBarsController::handleInput(MappedInputManager& mappedInput) {
  const auto touch = routeTouch(mappedInput);
  if (touch.routed) {
    if (app.invalidated()) redraw_ = true;
    // A handled event may have closed the bars already.
    if (touch) return;
  }
  // A tap on the page between the bars puts the reader back; one on a bar but off its controls does nothing.
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    bool onBars = false;
    {
      RenderLock lock;
      onBars = ReaderBarsLayout::onBars(area, model(), metrics, x, y);
    }
    if (!onBars) {
      close(ReaderBarsResult::Action::None);
      return;
    }
  }
  if (mappedInput.wasHomeGesture() || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close(ReaderBarsResult::Action::None);
    return;
  }

  // The first key on a touch board only shows the cursor.
  const bool cursorHidden = focus == Focus::None;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (cursorHidden) {
      moveAlongRow(0);
      redraw_ = true;
      return;
    }
    activateFocus();
    return;
  }
  int along = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) along = -1;
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) along = 1;
  if (along != 0) {
    moveAlongRow(cursorHidden ? 0 : along);
    redraw_ = true;
    return;
  }
  int between = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) between = -1;
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) between = 1;
  if (between != 0) {
    moveBetweenRows(cursorHidden ? 0 : between);
    redraw_ = true;
  }
}

ReaderBarsController::Event ReaderBarsController::loop(MappedInputManager& mappedInput) {
  handleInput(mappedInput);
  if (closed_) return Event::Closed;
  const Event event = layoutChanged_ ? Event::LayoutChanged : redraw_ ? Event::Redraw : Event::None;
  layoutChanged_ = false;
  redraw_ = false;
  return event;
}

void ReaderBarsController::resync(const ReaderLayoutValues& book, const ReaderLayoutValues& global,
                                  const ReaderSizeList& sizes) {
  RenderLock lock;
  info.bookValues = book;
  info.globalValues = global;
  info.sizes = sizes;
}
