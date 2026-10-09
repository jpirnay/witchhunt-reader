#pragma once

// Text entry on the FreeInkUI keyboard component: the SDK builds the layout
// layers and keyboard() draws the keys and registers their hit rects, the
// InteractionBuffer routes taps and long-presses, and this activity owns the
// text field, cursor editing and the URL layers.
//
// Ported from crosspoint-reader: the SDK keyboard by Justin Mitchell (PR #2481,
// with Julia Nguyen), the layout set and language key by winst0niuss (PR #2858,
// with Uri Tauber), multi-byte cursor and wrapping by winst0niuss (PRs #3094,
// #3095), the key layout and look by Julia Nguyen (PR #3755), Shift in URL
// fields by Ankit (PR #2357). Reworked here to build its layers with
// buildKeyboardLayout() -- one compact copy of the layouts instead of every
// layer expanded in flash -- and for our orientation-aware buttons and content
// rect.

#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <components/keyboard/keyboard.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

enum class InputType { Text, Password, Url };

class KeyboardEntryActivity : public Activity {
 public:
  explicit KeyboardEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string title = "Enter Text", std::string initialText = "",
                                 const size_t maxLength = 0, InputType inputType = InputType::Text)
      : Activity("KeyboardEntry", renderer, mappedInput),
        title(std::move(title)),
        text(std::move(initialText)),
        maxLength(maxLength),
        inputType(inputType) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Half-typed text only exists here, so sleeping would discard it. Same reason as
  // WifiSelectionActivity: this runs as a child activity, so whatever launched it
  // is no longer the one being asked.
  bool preventAutoSleep() override { return true; }

 private:
  std::string title;
  std::string text;
  size_t maxLength;
  InputType inputType;
  bool passwordVisible = false;

  ButtonNavigator buttonNavigator;

  // Which layer is showing. loop() changes these; render() reads them.
  freeink::ui::KeyboardLayoutId layoutId = freeink::ui::KeyboardLayoutId::QwertyEn;
  // Puts the language key in the Latin layers' bottom row; the Cyrillic ones
  // always carry it. Resolved once in onEnter(): the set cannot change while a
  // keyboard is on screen.
  bool showLangKey = false;
  bool shifted = false;
  bool symbols = false;
  bool urlPanel = false;  // URL snippet panel replaces the letter layer

  // The layer on screen, built by buildKeyboardLayout(). One buffer per task:
  // loop() resolves keys against `inputLayer` while render() draws from
  // `renderLayer`, so neither ever reads a layer the other is rebuilding.
  // A shared one would need a lock, and the render task drops its mutex in
  // the middle of a pass. About 1.2 KB each, for as long as the keyboard is up.
  mutable freeink::ui::KeyboardLayoutBuffer inputLayer;
  freeink::ui::KeyboardLayoutBuffer renderLayer;
  // Which layer `inputLayer` holds, so loop() rebuilds it only when that changes.
  mutable uint16_t inputLayerKey = UINT16_MAX;

  // Key hit rects registered by the keyboard component during render(); loop()
  // routes touch against them. A digit row over a Cyrillic layer registers 48
  // keys, so 56 leaves headroom. Two generations of 16-byte entries.
  freeink::ui::InteractionBuffer<56> interactions;

  // Button selection over the current layer (row/col in layer terms; the
  // bottom action row is just the last row).
  int selRow = 0;
  int selCol = 0;
  bool confirmHeld = false;
  bool confirmLongHandled = false;

  bool cursorMode = false;
  bool togglePos = false;
  size_t cursorPos = 0;  // byte offset into text, always on a code point boundary
  bool upHeld = false;
  bool upLongHandled = false;
  bool downHeld = false;
  bool downLongHandled = false;
  bool rightHeld = false;
  bool rightLongHandled = false;
  size_t savedCursorPos = 0;
  size_t rightStartCursorPos = 0;

  // Tap/hold routing (threshold long-press, release swallow, slide re-arm)
  // lives in the SDK; loop() feeds it the level-triggered touch state.
  freeink::ui::TouchHoldRouter touchRouter;
  // render() opts `interactions` into the SDK's double-buffered publish cycle
  // (beginPublishCycle()/publish()), so TouchHoldRouter in loop() always reads
  // a complete, previously published table, never one mid-rebuild. This flag
  // gates only the time before the very first publish. atomic so it also
  // orders that first publication.
  std::atomic<bool> interactionsReady{false};

  bool hintVisible = false;

  void onComplete(std::string text);
  void onCancel();

  // The layer the current state selects, built into `buffer`.
  const freeink::ui::KeyboardLayout& buildLayer(freeink::ui::KeyboardLayoutBuffer& buffer) const;
  // The same, for loop(): `inputLayer`, rebuilt only when the state changed.
  const freeink::ui::KeyboardLayout& currentLayout() const;
  const freeink::ui::KeyboardKey* selectedKey() const;
  int selectedLogicalIndex() const;
  void clampSelection();
  void moveSelectionRow(int delta);
  void moveSelectionCol(int delta);
  bool syncSelectionToValue(int16_t value);
  // Handles one key activation by stable key id. Returns true when the screen
  // needs a repaint; OK and cancel finish the activity instead.
  bool activateValue(int16_t value, bool longPress);
  bool clearAllOrAltOnSelected();
  void insertUtf8(const char* out);
  bool backspaceUtf8();
  static size_t utf8Prev(const std::string& s, size_t pos);
  static size_t utf8Next(const std::string& s, size_t pos);

  bool cursorPositionFromPoint(int x, int y, size_t& position) const;
  std::string displayTextForCurrentState() const;
  // Advance of s[start, end), measured in place by temporarily NUL-terminating
  // at `end` -- no substr temporary per measurement.
  int measureRange(std::string& s, int start, int end) const;
  // Largest line end in (start, s.length()] whose advance fits maxWidth, on a
  // code point boundary; always advances at least one character.
  int lineBreakEnd(std::string& s, int start, int maxWidth) const;
  // Where the keys go for a layer of `rows` rows (the whole keyboard band).
  freeink::ui::Rect keyboardRect(int rows) const;

  static constexpr uint16_t LONG_PRESS_MS = 500;
  static constexpr uint16_t DEL_LONG_PRESS_MS = 1500;
  static constexpr uint16_t TOUCH_LONG_PRESS_MS = 350;
  static constexpr uint16_t TOUCH_DEL_LONG_PRESS_MS = 900;
  // App-specific key id: toggles the URL snippet panel (URL fields only).
  static constexpr int16_t URL_PANEL_KEY = -3;
};
