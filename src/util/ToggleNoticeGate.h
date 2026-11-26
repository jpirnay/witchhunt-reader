#pragma once

#include <cstdint>

// When the message a toggle action asked for (ToggleNotice) goes on the panel. Apart from the code that paints it, so
// the host can test it (test/toggle_notice).
namespace ToggleNoticeGate {

enum class Step : uint8_t { Wait, Paint, Drop };

// The moment the message was asked for.
struct Asked {
  unsigned long atMs = 0;
  uint32_t activityChanges = 0;  // ActivityManager::activityChangeCount()
  uint32_t renderPasses = 0;     // ActivityManager::renderPassCount()
  bool afterFrame = false;       // the switch asked for a frame of its own as well
};

// The loop pass that decides.
struct Now {
  unsigned long ms = 0;
  uint32_t activityChanges = 0;
  uint32_t renderPasses = 0;
  bool busy = false;  // a frame queued, drawn or still going onto the panel, a transition, a press not taken in
};

// Past this the message is old news: a chapter laid out again for Auto Page Turn's status band can keep the reader
// busy that long.
constexpr unsigned long kMaxWaitMs = 5000;

constexpr Step step(const Asked& asked, const Now& now) {
  // About the screen it was asked on, not the one that has replaced it.
  if (now.activityChanges != asked.activityChanges) return Step::Drop;
  if (now.ms - asked.atMs > kMaxWaitMs) return Step::Drop;
  if (now.busy) return Step::Wait;
  // Painted before the frame the switch asked for (a page laid out again), it would be drawn over at once.
  if (asked.afterFrame && now.renderPasses == asked.renderPasses) return Step::Wait;
  return Step::Paint;
}

// The message on the panel: when it went up, and the screen and frame it went up over.
struct Shown {
  unsigned long atMs = 0;
  uint32_t activityChanges = 0;
  uint32_t renderPasses = 0;
};

enum class Clear : uint8_t { Keep, Redraw, Gone };

// How long it stays up when nothing else draws over it. In a book nothing does until the next page turn, and the
// message stayed over the page for as long as it was read (issue #930).
constexpr unsigned long kShowMs = 2000;

constexpr Clear clear(const Shown& shown, const Now& now) {
  // A frame of the screen's own, or another screen, has already drawn over it.
  if (now.activityChanges != shown.activityChanges || now.renderPasses != shown.renderPasses) return Clear::Gone;
  if (now.ms - shown.atMs < kShowMs) return Clear::Keep;
  // A frame is coming anyway, or a press is about to bring one, or the frame on the panel is lent out (a chapter
  // build on the C3s).
  if (now.busy) return Clear::Keep;
  return Clear::Redraw;
}

}  // namespace ToggleNoticeGate
