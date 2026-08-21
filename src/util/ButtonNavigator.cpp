#include "ButtonNavigator.h"

#include "ButtonEventManager.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"

const MappedInputManager* ButtonNavigator::mappedInput = nullptr;

void ButtonNavigator::onNext(const Callback& callback) {
  onNextPress(callback);
  onNextContinuous(callback);
}

void ButtonNavigator::onPrevious(const Callback& callback) {
  onPreviousPress(callback);
  onPreviousContinuous(callback);
}

void ButtonNavigator::onPressAndContinuous(const Buttons& buttons, const Callback& callback) {
  onPress(buttons, callback);
  onContinuous(buttons, callback);
}

void ButtonNavigator::onNextPress(const Callback& callback) { onPress(getNextButtons(), callback); }

void ButtonNavigator::onPreviousPress(const Callback& callback) { onPress(getPreviousButtons(), callback); }

void ButtonNavigator::onNextRelease(const Callback& callback) { onRelease(getNextButtons(), callback); }

void ButtonNavigator::onPreviousRelease(const Callback& callback) { onRelease(getPreviousButtons(), callback); }

void ButtonNavigator::onNextContinuous(const Callback& callback) { onContinuous(getNextButtons(), callback); }

void ButtonNavigator::onPreviousContinuous(const Callback& callback) { onContinuous(getPreviousButtons(), callback); }

void ButtonNavigator::onPress(const Buttons& buttons, const Callback& callback) {
  const bool wasPressed = std::any_of(buttons.begin(), buttons.end(), [](const MappedInputManager::Button button) {
    return mappedInput != nullptr && mappedInput->wasPressed(button);
  });

  if (wasPressed) {
    callback();
  }
}

void ButtonNavigator::onRelease(const Buttons& buttons, const Callback& callback) {
  // The double-click FSM in ButtonEventManager delays Short events by DOUBLE_WINDOW_MS
  // (300ms) when a double-press action is configured for that button, so the configured
  // Short and Double actions can be disambiguated. In a reader activity that gating must
  // suppress release-based navigation during the wait, otherwise Left/Right would both
  // turn the page AND fire the configured short action.
  //
  // In non-reader UIs (settings, file browser, etc.), only navigation reacts to release —
  // the configured per-button actions are not dispatched there (see
  // ActivityManager::dispatchButtonAction, which is reader-only). Gating on isShortPending
  // there just makes Left/Right navigation feel sluggish (300ms lag) compared to Up/Down
  // (which have no FSM at all). So skip the gate outside reader activities.
  const bool inReader = activityManager.isReaderActivity();
  const bool wasReleased =
      std::any_of(buttons.begin(), buttons.end(), [inReader](const MappedInputManager::Button button) {
        if (mappedInput == nullptr || !mappedInput->wasReleased(button)) {
          return false;
        }
        return !(inReader && globalButtonEvents().isShortPending(button));
      });

  if (wasReleased) {
    if (lastContinuousNavTime == 0) {
      callback();
    }

    lastContinuousNavTime = 0;
  }
}

void ButtonNavigator::onContinuous(const Buttons& buttons, const Callback& callback) {
  const bool isPressed = std::any_of(buttons.begin(), buttons.end(), [this](const MappedInputManager::Button button) {
    return mappedInput != nullptr && mappedInput->isPressed(button) && shouldNavigateContinuously();
  });

  if (isPressed) {
    callback();
    lastContinuousNavTime = millis();
  }
}

bool ButtonNavigator::shouldNavigateContinuously() const {
  if (!mappedInput) return false;

  const bool buttonHeldLongEnough = mappedInput->getHeldTime() > continuousStartMs;
  const bool navigationIntervalElapsed = (millis() - lastContinuousNavTime) > continuousIntervalMs;

  return buttonHeldLongEnough && navigationIntervalElapsed;
}

void ButtonNavigator::setSelectablePredicate(std::function<bool(int)> selectablePredicate, int totalItems) {
  this->selectablePredicate = std::move(selectablePredicate);
  this->selectableTotalItems = totalItems;
}

void ButtonNavigator::clearSelectablePredicate() {
  selectablePredicate = nullptr;
  selectableTotalItems = 0;
}

int ButtonNavigator::nextIndex(int currentIndex) const {
  if (!selectablePredicate || selectableTotalItems <= 0) return currentIndex;
  return nextIndex(currentIndex, selectableTotalItems, selectablePredicate);
}

int ButtonNavigator::previousIndex(int currentIndex) const {
  if (!selectablePredicate || selectableTotalItems <= 0) return currentIndex;
  return previousIndex(currentIndex, selectableTotalItems, selectablePredicate);
}

int ButtonNavigator::nextIndex(const int currentIndex, const int totalItems) {
  if (totalItems <= 0) return 0;

  // Calculate the next index with wrap-around
  return (currentIndex + 1) % totalItems;
}

int ButtonNavigator::previousIndex(const int currentIndex, const int totalItems) {
  if (totalItems <= 0) return 0;

  // Calculate the previous index with wrap-around
  return (currentIndex + totalItems - 1) % totalItems;
}

int ButtonNavigator::nextIndex(const int currentIndex, const std::vector<bool>& selectable) {
  const int totalItems = static_cast<int>(selectable.size());
  if (totalItems <= 0) return 0;

  int index = nextIndex(currentIndex, totalItems);
  for (int i = 0; i < totalItems; ++i) {
    if (selectable[index]) {
      return index;
    }
    index = nextIndex(index, totalItems);
  }

  return currentIndex;
}

int ButtonNavigator::previousIndex(const int currentIndex, const std::vector<bool>& selectable) {
  const int totalItems = static_cast<int>(selectable.size());
  if (totalItems <= 0) return 0;

  int index = previousIndex(currentIndex, totalItems);
  for (int i = 0; i < totalItems; ++i) {
    if (selectable[index]) {
      return index;
    }
    index = previousIndex(index, totalItems);
  }

  return currentIndex;
}

int ButtonNavigator::nextIndex(const int currentIndex, const int totalItems,
                               const std::function<bool(int index)>& isSelectable) {
  if (totalItems <= 0) return 0;
  if (!isSelectable) return nextIndex(currentIndex, totalItems);

  int index = nextIndex(currentIndex, totalItems);
  for (int i = 0; i < totalItems; ++i) {
    if (isSelectable(index)) {
      return index;
    }
    index = nextIndex(index, totalItems);
  }

  return currentIndex;
}

int ButtonNavigator::previousIndex(const int currentIndex, const int totalItems,
                                   const std::function<bool(int index)>& isSelectable) {
  if (totalItems <= 0) return 0;
  if (!isSelectable) return previousIndex(currentIndex, totalItems);

  int index = previousIndex(currentIndex, totalItems);
  for (int i = 0; i < totalItems; ++i) {
    if (isSelectable(index)) {
      return index;
    }
    index = previousIndex(index, totalItems);
  }

  return currentIndex;
}

int ButtonNavigator::nextPageIndex(const int currentIndex, const int totalItems, const int itemsPerPage) {
  if (totalItems <= 0 || itemsPerPage <= 0) return 0;

  // When items fit on one page, use index navigation instead
  if (totalItems <= itemsPerPage) {
    return nextIndex(currentIndex, totalItems);
  }

  const int lastPageIndex = (totalItems - 1) / itemsPerPage;
  const int currentPageIndex = currentIndex / itemsPerPage;

  if (currentPageIndex < lastPageIndex) {
    return (currentPageIndex + 1) * itemsPerPage;
  }

  return 0;
}

int ButtonNavigator::previousPageIndex(const int currentIndex, const int totalItems, const int itemsPerPage) {
  if (totalItems <= 0 || itemsPerPage <= 0) return 0;

  // When items fit on one page, use index navigation instead
  if (totalItems <= itemsPerPage) {
    return previousIndex(currentIndex, totalItems);
  }

  const int lastPageIndex = (totalItems - 1) / itemsPerPage;
  const int currentPageIndex = currentIndex / itemsPerPage;

  if (currentPageIndex > 0) {
    return (currentPageIndex - 1) * itemsPerPage;
  }

  return lastPageIndex * itemsPerPage;
}

void ButtonNavigator::onNextList(int& selectedIndex, const int totalItems, const Callback& onChange,
                                 const int pageSize) {
  onListNav(getStepNextButtons(), true, selectedIndex, totalItems, pageSize, lastNextPressMs, lastNextPressCount,
            longPressNextFired, onChange);
  onListPageNav(getPageNextButtons(), true, selectedIndex, totalItems, pageSize, onChange);
}

void ButtonNavigator::onNextList(const Buttons& buttons, int& selectedIndex, const int totalItems,
                                 const Callback& onChange, const int pageSize) {
  onListNav(buttons, true, selectedIndex, totalItems, pageSize, lastNextPressMs, lastNextPressCount, longPressNextFired,
            onChange);
}

void ButtonNavigator::onPreviousList(int& selectedIndex, const int totalItems, const Callback& onChange,
                                     const int pageSize) {
  onListNav(getStepPreviousButtons(), false, selectedIndex, totalItems, pageSize, lastPreviousPressMs,
            lastPreviousPressCount, longPressPreviousFired, onChange);
  onListPageNav(getPagePreviousButtons(), false, selectedIndex, totalItems, pageSize, onChange);
}

void ButtonNavigator::onPreviousList(const Buttons& buttons, int& selectedIndex, const int totalItems,
                                     const Callback& onChange, const int pageSize) {
  onListNav(buttons, false, selectedIndex, totalItems, pageSize, lastPreviousPressMs, lastPreviousPressCount,
            longPressPreviousFired, onChange);
}

// Left/Right: one screenful per press, held down for continuous paging. Deliberately none of the
// press-type machinery below — a page jump wants to repeat while held, and there is nothing left
// for a double-click or a long press to mean that a repeat does not already cover.
void ButtonNavigator::onListPageNav(const Buttons& buttons, const bool forward, int& selectedIndex,
                                    const int totalItems, const int pageSize, const Callback& onChange) {
  if (!mappedInput || totalItems <= 0) return;

  const int page = effectivePageSize(pageSize);
  const auto jump = [&] {
    const int target =
        forward ? nextPageIndex(selectedIndex, totalItems, page) : previousPageIndex(selectedIndex, totalItems, page);
    selectedIndex = target;
    if (selectablePredicate && !selectablePredicate(selectedIndex)) {
      // The page boundary can land on a separator row (settings sections). Walk on in the
      // direction of travel — except at the top of the list, where walking further back wraps to
      // the far end, which is the opposite of what Left should ever do.
      const int walked = forward ? nextIndex(selectedIndex) : previousIndex(selectedIndex);
      selectedIndex = (!forward && walked > selectedIndex) ? nextIndex(selectedIndex) : walked;
    }
    onChange();
  };

  onPressAndContinuous(buttons, jump);
}

// Merges the press logs of a button set: counts add up, timestamps come from whichever button was
// pressed last. Sets are normally one button (Up, or Down), and where they are not, the pair is
// two names for one physical button (Up/PageBack) or two buttons that mean the same thing to the
// list — either way a press on any of them is a press for this navigator. A pair of taps split
// across two DIFFERENT buttons of the set is not read as a double-tap, which is the intended
// reading: those are two separate gestures.
ButtonEventManager::PressLog ButtonNavigator::latestPressLog(const Buttons& buttons) {
  ButtonEventManager::PressLog merged;
  uint16_t total = 0;
  for (const MappedInputManager::Button b : buttons) {
    const auto log = globalButtonEvents().pressLog(b);
    total = static_cast<uint16_t>(total + log.count);
    if (log.count > 0 && (merged.count == 0 || log.lastPressMs >= merged.lastPressMs)) {
      merged = log;
    }
  }
  merged.count = total;
  return merged;
}

void ButtonNavigator::onListNav(const Buttons& buttons, const bool forward, int& selectedIndex, const int totalItems,
                                const int pageSize, uint32_t& lastPressMs, uint16_t& lastSeenPressCount,
                                bool& longPressFired, const Callback& onChange) {
  if (!mappedInput || totalItems <= 0) return;

  const bool anyHeld = std::any_of(buttons.begin(), buttons.end(),
                                   [](const MappedInputManager::Button b) { return mappedInput->isPressed(b); });

  if (anyHeld && mappedInput->getHeldTime() > listLongPressMs) {
    if (!longPressFired) {
      longPressFired = true;
      if (forward) {
        selectedIndex = totalItems - 1;
        if (selectablePredicate) {
          while (selectedIndex > 0 && !selectablePredicate(selectedIndex)) --selectedIndex;
        }
      } else {
        selectedIndex = 0;
        if (selectablePredicate) {
          while (selectedIndex < totalItems - 1 && !selectablePredicate(selectedIndex)) ++selectedIndex;
        }
      }
      onChange();
    }
    return;
  }

  const bool wasReleased = std::any_of(buttons.begin(), buttons.end(),
                                       [](const MappedInputManager::Button b) { return mappedInput->wasReleased(b); });
  if (wasReleased) {
    // Long press already fired: reset the guard on release — no navigation.
    longPressFired = false;
  }

  // Presses come from the sampler's log, not from polling wasPressed(): it counts every debounced
  // press-down edge and stamps it with the time the sampler saw it. Polling loses both halves of a
  // double-tap — two taps inside one loop tick collapse into a single flag, and the gap ends up
  // measured between the moments the loop NOTICED each press, which includes whatever redraw
  // happened in between. On e-ink that redraw alone outlasts the window, which is why a
  // double-tap used to register as two single steps almost every time.
  const ButtonEventManager::PressLog press = latestPressLog(buttons);
  if (press.count == lastSeenPressCount) return;
  const bool resumed = press.count < lastSeenPressCount;  // drain() zeroed the log under us
  const uint16_t newPresses = resumed ? press.count : static_cast<uint16_t>(press.count - lastSeenPressCount);
  lastSeenPressCount = press.count;

  // Long press already fired: skip the press navigation — the jump-to-end already happened.
  if (longPressFired) return;

  // Two taps close together are a page jump, whether they arrived in one tick (the pair is in the
  // log) or across two (the second is close to the one we acted on last time).
  const bool doubleInOneTick =
      newPresses >= 2 && press.priorPressMs > 0 && press.lastPressMs - press.priorPressMs < listDoubleClickMs;
  const bool doubleAcrossTicks =
      newPresses == 1 && lastPressMs > 0 && press.lastPressMs - lastPressMs < listDoubleClickMs;
  const bool isDouble = !resumed && (doubleInOneTick || doubleAcrossTicks);
  // Zeroed after a jump so a third tap starts a fresh pair instead of chaining page after page.
  lastPressMs = isDouble ? 0 : press.lastPressMs;

  if (isDouble) {
    // A pair split across two ticks already moved one step for its first tap: undo that, so the
    // total movement is exactly one page. A pair that arrived inside a single tick has not moved
    // anything yet, and its indexBeforePress belongs to some older press — leave the selection be.
    // (The bounds test also guards a stale index if the list shrank since it was stored.)
    if (doubleAcrossTicks && indexBeforePress >= 0 && indexBeforePress < totalItems) {
      selectedIndex = indexBeforePress;
    }
    // A page, not a fixed ten: this is the only jump available on the lists whose Left/Right are
    // taken by their own actions (the file browser), so it should match what a screenful is there.
    const int jumpCount = effectivePageSize(pageSize);
    for (int i = 0; i < jumpCount; ++i) {
      const int next =
          forward ? (selectablePredicate ? nextIndex(selectedIndex) : nextIndex(selectedIndex, totalItems))
                  : (selectablePredicate ? previousIndex(selectedIndex) : previousIndex(selectedIndex, totalItems));
      // Stop before wrapping: forward movement decreases index only on wrap; backward vice versa.
      if (forward && next <= selectedIndex) break;
      if (!forward && next >= selectedIndex) break;
      selectedIndex = next;
    }
  } else {
    indexBeforePress = selectedIndex;
    selectedIndex =
        forward ? (selectablePredicate ? nextIndex(selectedIndex) : nextIndex(selectedIndex, totalItems))
                : (selectablePredicate ? previousIndex(selectedIndex) : previousIndex(selectedIndex, totalItems));
  }
  onChange();
}
