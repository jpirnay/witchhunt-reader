#include "KeyboardLayoutsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"

void KeyboardLayoutsActivity::onEnter() {
  UiListActivity::onEnter();
  workingMask = keyboard_layouts::enabled();
  edited = false;

  // Each layout goes by the language it is for, named in that language.
  for (int i = 0; i < keyboard_layouts::COUNT; ++i) {
    rowItems[i].label = keyboard_layouts::name(static_cast<uint8_t>(i));
    rowItems[i].actionValue = static_cast<int16_t>(i);
  }
}

void KeyboardLayoutsActivity::onExit() {
  if (edited && workingMask != SETTINGS.keyboardLayouts) {
    SETTINGS.keyboardLayouts = workingMask;
    SETTINGS.saveToFile();
  }
  UiListActivity::onExit();
}

const char* KeyboardLayoutsActivity::headerTitle() const { return tr(STR_KEYBOARD_LAYOUTS); }

bool KeyboardLayoutsActivity::isLocked(const uint8_t i) const {
  const uint16_t bit = keyboard_layouts::bitAt(i);
  if (!(workingMask & bit)) return false;
  const uint16_t without = static_cast<uint16_t>(workingMask & ~bit);
  return (without & keyboard_layouts::LATIN_BITS) == 0;
}

void KeyboardLayoutsActivity::activateIndex(const int index) {
  nav.selected = index;
  // The row stays on screen with a new ON/OFF value; a lingering flash would
  // gray an unrelated row on the repaint below.
  app.clearTapFlash();

  if (isLocked(static_cast<uint8_t>(index))) {
    requestUpdate();
    return;
  }

  workingMask = static_cast<uint16_t>(workingMask ^ keyboard_layouts::bitAt(static_cast<uint8_t>(index)));
  edited = true;
  requestUpdate();
}

void KeyboardLayoutsActivity::buildScreen(UiScreen& screen) {
  layoutListArea(screen);

  for (int i = 0; i < keyboard_layouts::COUNT; ++i) {
    const auto row = static_cast<uint8_t>(i);
    if (isLocked(row)) {
      rowItems[i].value = tr(STR_DEFAULT_VALUE);
    } else {
      rowItems[i].value = (workingMask & keyboard_layouts::bitAt(row)) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    }
  }

  auto props = listProps(screen);
  props.items = rowItems;
  props.count = keyboard_layouts::COUNT;
  // Unlike the other lists, this one leaves the label style unset: the screen then takes the theme's
  // body text with the theme's own line limit rather than two lines.
  props.labelText = {};
  addList(screen, props);
}
