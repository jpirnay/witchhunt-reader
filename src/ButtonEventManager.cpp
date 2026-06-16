#include "ButtonEventManager.h"

#include "CrossPointSettings.h"

// Required for constexpr array out-of-class definition (C++14).
constexpr ButtonEventManager::Button ButtonEventManager::ALL_BUTTONS[ButtonEventManager::NUM_BUTTONS];

int ButtonEventManager::buttonToIndex(const Button button) {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (ALL_BUTTONS[i] == button) {
      return i;
    }
  }
  return -1;
}

bool ButtonEventManager::hasDoubleAction(const Button button) const {
  if (forcedDoubleMask & (1 << static_cast<int>(button))) {
    return true;
  }
  using BA = CrossPointSettings::BUTTON_ACTION;
  switch (button) {
    case Button::Back:
      return SETTINGS.btnDoubleBack != BA::BTN_DEFAULT;
    case Button::Confirm:
      return SETTINGS.btnDoubleConfirm != BA::BTN_DEFAULT;
    case Button::Left:
      return SETTINGS.btnDoubleLeft != BA::BTN_DEFAULT;
    case Button::Right:
      return SETTINGS.btnDoubleRight != BA::BTN_DEFAULT;
    case Button::PageBack:
      return SETTINGS.btnDoublePageBack != BA::BTN_DEFAULT;
    case Button::PageForward:
      return SETTINGS.btnDoublePageForward != BA::BTN_DEFAULT;
    case Button::Power:
      return SETTINGS.btnDoublePower != BA::BTN_DEFAULT;
  }
  return false;
}

void ButtonEventManager::pushEvent(const Button button, const PressType type) {
  const int next = (eventTail + 1) % EVENT_BUF;
  if (next == eventHead) return;  // buffer full, drop oldest not possible — just drop newest
  eventBuf[eventTail] = {button, type};
  eventTail = next;
}

void ButtonEventManager::pushEventFront(const Button button, const PressType type) {
  const int prev = (eventHead - 1 + EVENT_BUF) % EVENT_BUF;
  if (prev == eventTail) return;  // buffer full
  eventHead = prev;
  eventBuf[eventHead] = {button, type};
}

bool ButtonEventManager::isShortPending(const Button button) const {
  const int idx = buttonToIndex(button);
  if (idx < 0) return false;
  return buttons[idx].state == State::ReleasedOnce;
}

bool ButtonEventManager::consumeEvent(ButtonEvent& out) {
  if (eventHead == eventTail) return false;
  out = eventBuf[eventHead];
  eventHead = (eventHead + 1) % EVENT_BUF;
  return true;
}

void ButtonEventManager::drain() {
  for (auto& b : buttons) {
    b.state = State::Idle;
    b.pressDownTime = 0;
    b.releaseTime = 0;
  }
  eventHead = eventTail = 0;
  longPressDispatchedMask = 0;
  // Drop edges the sampler queued for the outgoing activity so they don't bleed in.
  input.flushRawEdges();
}

void ButtonEventManager::applyEdge(const int idx, const Button btn, const bool pressed, const unsigned long t) {
  PerButton& s = buttons[idx];
  switch (s.state) {
    case State::Idle:
      if (pressed) {
        s.state = State::Pressed;
        s.pressDownTime = t;
      }
      break;

    case State::Pressed:
      if (!pressed) {
        const unsigned long heldMs = t - s.pressDownTime;
        if (heldMs >= LONG_PRESS_MS) {
          pushEvent(btn, PressType::Long);
          s.state = State::Idle;
        } else if (hasDoubleAction(btn)) {
          // Delay short-press decision until double-click window expires.
          s.releaseTime = t;
          s.state = State::ReleasedOnce;
        } else {
          // No double action configured — fire immediately.
          pushEvent(btn, PressType::Short);
          s.state = State::Idle;
        }
      }
      break;

    case State::ReleasedOnce:
      if (pressed) {
        if (t - s.releaseTime >= DOUBLE_WINDOW_MS) {
          // Window already elapsed before this press arrived (e.g. the loop task
          // was blocked past it): the first press was a Short, this starts fresh.
          pushEvent(btn, PressType::Short);
          s.state = State::Pressed;
          s.pressDownTime = t;
        } else {
          // Second press within the window — start tracking the double.
          s.state = State::DoublePressed;
          s.pressDownTime = t;
        }
      }
      break;

    case State::DoublePressed:
      if (!pressed) {
        pushEvent(btn, PressType::Double);
        s.state = State::Idle;
      }
      break;
  }
}

void ButtonEventManager::applyTimeout(const int idx, const Button btn, const unsigned long now, const bool heldNow) {
  PerButton& s = buttons[idx];
  switch (s.state) {
    case State::Pressed:
      if (heldNow && now - s.pressDownTime >= LONG_PRESS_MS) {
        // Fire Long as soon as the hold threshold passes, without waiting for
        // release. The eventual release edge lands in Idle and is ignored.
        pushEvent(btn, PressType::Long);
        s.state = State::Idle;
      }
      break;

    case State::ReleasedOnce:
      if (now - s.releaseTime >= DOUBLE_WINDOW_MS) {
        // Window expired without a second press — it was a Short.
        pushEvent(btn, PressType::Short);
        s.state = State::Idle;
      }
      break;

    default:
      break;
  }
}

void ButtonEventManager::update() {
  // 1) Replay every debounced edge the sampler captured since the last tick, in
  //    order, each with its own timestamp. Processing edges discretely (rather
  //    than sampling instantaneous wasPressed/wasReleased) means a press+release
  //    that both landed inside one slow loop iteration is still classified.
  HalGPIO::ButtonEdge edge;
  while (input.popRawEdge(edge)) {
    for (int i = 0; i < NUM_BUTTONS; i++) {
      const Button btn = ALL_BUTTONS[i];
      if (input.rawIndex(btn) != edge.button) {
        continue;
      }
      // A fresh press clears the long-press-dispatched suppression for this button
      // (it survives through the release so detectPageTurn can skip the release
      // page-turn, then resets on the next press).
      if (edge.pressed) {
        longPressDispatchedMask &= ~(1u << static_cast<int>(btn));
      }
      applyEdge(i, btn, edge.pressed, edge.timeMs);
    }
  }

  // 2) Time-based transitions that no edge will deliver: long-press-while-held and
  //    double-click-window expiry.
  const unsigned long now = millis();
  for (int i = 0; i < NUM_BUTTONS; i++) {
    applyTimeout(i, ALL_BUTTONS[i], now, input.isPressed(ALL_BUTTONS[i]));
  }
}
