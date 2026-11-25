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

// Past this the message is old news: a chapter laid out again for Bionic Reading can keep the reader busy that long.
constexpr unsigned long kMaxWaitMs = 5000;

constexpr Step step(const Asked& asked, const Now& now) {
  // About the screen it was asked on, not the one that has replaced it.
  if (now.activityChanges != asked.activityChanges) return Step::Drop;
  if (now.ms - asked.atMs > kMaxWaitMs) return Step::Drop;
  if (now.busy) return Step::Wait;
  // Painted before the frame the switch asked for (Dark Mode, a page laid out again), it would be drawn over at once.
  if (asked.afterFrame && now.renderPasses == asked.renderPasses) return Step::Wait;
  return Step::Paint;
}

}  // namespace ToggleNoticeGate
