#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include <cstdint>

#include "MappedInputManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;

// Round-half-up integer division clamped to [0, 100], used as the percent byte appended to
// progress.bin so the home screen can render a per-book badge without re-loading the document.
// All reader types must funnel through this so the displayed value matches across formats.
inline uint8_t pageProgressPercentByte(int currentPage, int totalPages) {
  if (totalPages <= 0 || currentPage < 0) {
    return 0;
  }
  const long numerator = static_cast<long>(currentPage + 1) * 200L + totalPages;
  const long percent = numerator / (2L * totalPages);
  if (percent < 0) return 0;
  if (percent > 100) return 100;
  return static_cast<uint8_t>(percent);
}

// Round-half-up clamp for a pre-computed [0,1] progress fraction (used by EPUB, where progress
// is byte-weighted across spine items rather than a simple page ratio).
inline uint8_t fractionProgressPercentByte(float fraction) {
  const int percent = static_cast<int>(fraction * 100.0f + 0.5f);
  if (percent < 0) return 0;
  if (percent > 100) return 100;
  return static_cast<uint8_t>(percent);
}

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

// Suppresses input processing on activity entry until the user has released all buttons and a
// clean frame (no pending press/release events) has been observed. Without this, the power-button
// hold used to wake the device leaks into the reader's page-turn handling and triggers a page turn
// or chapter skip (the wake-hold easily exceeds skipChapterMs).
// Each reader holds an instance, calls arm() in onEnter(), and calls shouldDrain() at the top
// of loop() — returning early when it returns true.
struct InputDrainGuard {
  bool active = false;

  void arm() { active = true; }

  bool shouldDrain(const MappedInputManager& input) {
    if (!active) {
      return false;
    }
    using B = MappedInputManager::Button;
    const bool anyHeld = input.isPressed(B::Back) || input.isPressed(B::Confirm) || input.isPressed(B::Left) ||
                         input.isPressed(B::Right) || input.isPressed(B::Up) || input.isPressed(B::Down) ||
                         input.isPressed(B::Power) || input.isPressed(B::PageBack) || input.isPressed(B::PageForward);
    if (anyHeld || input.wasAnyPressed() || input.wasAnyReleased()) {
      return true;
    }
    active = false;
    return false;
  }
};

struct PageTurnResult {
  bool prev;
  bool next;
};

// Tilt gestures only. Button page turns come from ButtonEventManager's Short events,
// which every reader handles in its consumeEvent() loop.
//
// Buttons used to be read here from MappedInputManager::wasReleased() instead. That is a
// per-tick bitmask (HalGPIO::accumPressed_/accumReleased_, OR-ed by the sampler and cleared
// on every gpio.update()), so every release landing inside one slow loop iteration collapsed
// into a single page turn and the surplus presses were dropped. The event queue replays each
// debounced edge discretely with its own timestamp, so nothing is lost. Left/Right already
// took the queue path (they carry a double-click action by default, which suppressed the
// wasReleased branch) while Up/Down took the snapshot path — the two behaved visibly
// differently under fast repeated presses. One path for all four keeps them identical.
inline PageTurnResult detectTiltPageTurn() {
  using TA = CrossPointSettings::TILT_GESTURE_ACTION;
  const bool tiltNegative = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPositive = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  bool tiltPrev = false;
  bool tiltNext = false;
  auto applyTiltAction = [&](uint8_t action) {
    if (action == TA::TILT_ACT_NEXT_PAGE) {
      tiltNext = true;
    } else if (action == TA::TILT_ACT_PREV_PAGE) {
      tiltPrev = true;
    }
  };
  if (tiltPositive) {
    applyTiltAction(SETTINGS.tiltPositiveAction);
  }
  if (tiltNegative) {
    applyTiltAction(SETTINGS.tiltNegativeAction);
  }
  return {tiltPrev, tiltNext};
}

// --- Touch page turning ------------------------------------------------------
// Ported from upstream/develop; see docs/touch-input-migration-2026-08-14.md
// phase 3. Header-only and inert on non-touch boards (input.hasTouch() is false
// there), so the readers need no ifdefs.

struct TouchPageTurn {
  bool prev;
  bool next;
  unsigned long heldMs;
};

inline TouchPageTurn detectTouchPageTurn(const GfxRenderer& renderer, const MappedInputManager& input) {
  TouchPageTurn result{false, false, 0};
  if (!SETTINGS.touchReaderControls || !input.hasTouch()) {
    return result;
  }

  if (SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_SWIPE) {
    // Horizontal swipes turn pages; taps stay free for the centred reader-menu
    // zone. A slow swipe never becomes a long-press chapter skip.
    const auto dir = input.wasSwipe();
    if (dir == MappedInputManager::SwipeDir::Left) {
      result.next = true;
    } else if (dir == MappedInputManager::SwipeDir::Right) {
      result.prev = true;
    }
    return result;
  }

  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) {
    return result;
  }

  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  // Outer thirds only: the centre column carries the reader-menu tap target
  // (isTouchMenuTap below), so it must not double as a page turn. The vertical
  // bounds keep the menu zone from swallowing the whole centre column.
  const int zoneWidth = width / 3;
  if (y < 0 || y >= height) return result;
  const bool inverted = SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_INVERTED_TAP;
  if (x < zoneWidth) {
    result.prev = !inverted;
    result.next = inverted;
  } else if (x >= width - zoneWidth) {
    result.prev = inverted;
    result.next = !inverted;
  }
  result.heldMs = input.lastTouchHeldMs();
  return result;
}

// Tap in the centre third: the tap path into the reader menu. The page-turn tap
// zones are the outer horizontal thirds, so the centred rectangle stays free.
inline bool isTouchMenuTap(const GfxRenderer& renderer, const MappedInputManager& input) {
  if (!input.hasTouch() || !SETTINGS.tapForReaderMenu) return false;
  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) return false;
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int zoneWidth = width / 3;
  const int zoneHeight = height / 3;
  return x >= zoneWidth && x < width - zoneWidth && y >= zoneHeight && y < height - zoneHeight;
}

// Reader menu opens on the menu edge-swipe or a centre-third tap. With touch
// reader controls Off the reading surface ignores touch entirely, menu
// included, so a stray brush cannot open it; the menu stays reachable through
// the Confirm button (and, on X4 Pro, the capacitive Home key).
inline bool isTouchMenuGesture(const GfxRenderer& renderer, const MappedInputManager& input) {
  if (!SETTINGS.touchReaderControls) return false;
  return (input.hasTouch() && input.wasMenuGesture()) || isTouchMenuTap(renderer, input);
}

inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  const int freq = SETTINGS.getRefreshFrequency();
  if (freq == 0) {
    renderer.displayBuffer();
    return;
  }
  if (pagesUntilFullRefresh <= 1) {
    LOG_DBG("RCY", "displayWithRefreshCycle: HALF (counter=%d freq=%d)", pagesUntilFullRefresh, freq);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = freq;
  } else {
    LOG_DBG("RCY", "displayWithRefreshCycle: fast (counter=%d freq=%d)", pagesUntilFullRefresh, freq);
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }
}

// Resolve the refresh-cycle mode for the next page and advance the counter.
// Split out of triggerWithRefreshCycle() so callers that fire the refresh
// through a different mechanism (e.g. triggerDisplayAsync for the inline AA
// path) share the exact same cycle policy and logging.
inline HalDisplay::RefreshMode nextRefreshCycleMode(int& pagesUntilFullRefresh) {
  const int freq = SETTINGS.getRefreshFrequency();
  if (freq == 0) {
    return HalDisplay::FAST_REFRESH;
  }
  if (pagesUntilFullRefresh <= 1) {
    LOG_DBG("RCY", "triggerWithRefreshCycle: HALF (counter=%d freq=%d)", pagesUntilFullRefresh, freq);
    pagesUntilFullRefresh = freq;
    return HalDisplay::HALF_REFRESH;
  }
  LOG_DBG("RCY", "triggerWithRefreshCycle: fast (counter=%d freq=%d)", pagesUntilFullRefresh, freq);
  pagesUntilFullRefresh--;
  return HalDisplay::FAST_REFRESH;
}

// Non-blocking variant: trigger the display refresh and return immediately.
// completeDisplay() must be called later (on the same task) to wait for the
// waveform and do post-refresh SPI work.
inline void triggerWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  renderer.triggerDisplay(nextRefreshCycleMode(pagesUntilFullRefresh));
}

inline void enforceExitFullRefresh(const GfxRenderer& renderer) {
  // Reader exits can leave visible ghosting when the next screen is rendered with a fast LUT.
  // Schedule the next displayed screen to use a half refresh, rather than refreshing
  // the current reader screen as it closes.
  renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
}

}  // namespace ReaderUtils
