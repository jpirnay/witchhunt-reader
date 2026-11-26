#include "ToggleNotice.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <cstdio>

#include "ToggleNoticeGate.h"
#include "activities/Activity.h"  // complete before ActivityManager.h, which holds activities by unique_ptr
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"

namespace {

struct Pending {
  bool armed = false;
  StrId name = StrId::STR_ON;
  bool on = false;
  ToggleNoticeGate::Asked asked;
};
Pending pending;

// The message painted and not yet drawn over.
bool onPanel = false;
ToggleNoticeGate::Shown shown;

void takeDownWhenDue(const ToggleNoticeGate::Now& now) {
  switch (ToggleNoticeGate::clear(shown, now)) {
    case ToggleNoticeGate::Clear::Keep:
      return;
    case ToggleNoticeGate::Clear::Gone:
      onPanel = false;
      return;
    case ToggleNoticeGate::Clear::Redraw:
      onPanel = false;
      // The screen's own frame takes it down: in a book the page on the panel drawn again, as a clock tick draws it
      // (a fast refresh, not billed to the ghosting budget), with its gray planes.
      activityManager.requestUpdate();
      LOG_DBG("TGL", "Message taken down");
      return;
  }
}

}  // namespace

namespace ToggleNotice {

void show(const StrId name, const bool on, const bool afterFrame) {
  pending.armed = true;
  pending.name = name;
  pending.on = on;
  pending.asked.atMs = millis();
  pending.asked.activityChanges = activityManager.activityChangeCount();
  pending.asked.renderPasses = activityManager.renderPassCount();
  pending.asked.afterFrame = afterFrame;
}

void service(GfxRenderer& renderer) {
  if (!pending.armed && !onPanel) return;
  ToggleNoticeGate::Now now;
  now.ms = millis();
  now.activityChanges = activityManager.activityChangeCount();
  now.renderPasses = activityManager.renderPassCount();
  // Also waits while the secondary buffer is lent out (a chapter build on the C3s): the frame on the panel cannot be
  // put back in the write buffer then, and the box would go up on a stale frame (ActivityManager::showBusyIndicator()).
  now.busy = activityManager.busyReason(/*includeBackground=*/false) != nullptr || !renderer.hasSecondaryBuffer();

  // Nothing to paint: the message on the panel comes down when due. One still to paint goes first and replaces it.
  if (!pending.armed) {
    takeDownWhenDue(now);
    return;
  }

  switch (ToggleNoticeGate::step(pending.asked, now)) {
    case ToggleNoticeGate::Step::Wait:
      return;
    case ToggleNoticeGate::Step::Drop:
      pending.armed = false;
      LOG_DBG("TGL", "Message not shown: the screen changed or stayed busy");
      return;
    case ToggleNoticeGate::Step::Paint:
      break;
  }

  RenderLock lock;
  // A refresh still uses the buffers: a later pass.
  if (renderer.isRefreshPending()) return;
  pending.armed = false;
  char text[96];
  snprintf(text, sizeof(text), "%s: %s", I18N.get(pending.name), pending.on ? tr(STR_ON) : tr(STR_OFF));
  // The frame on the panel, as a screenshot takes it (ScreenshotUtil::service): displayBuffer() ends in a swap, so the
  // write buffer is two frames behind, and the reader may have its next page there (prepareFramebufferForCapture()).
  if (renderer.keepsPreviousFrame()) renderer.syncWriteBufferFromDisplayed();
  activityManager.prepareFramebufferForCapture();
  GUI.drawPopup(renderer, text, /*overlayDisplayedFrame=*/false);
  // Timed from the end of the refresh that put it up. The counts are the ones read before it: no render pass starts
  // while this holds the lock, so a frame asked for meanwhile counts as one that drew over the message.
  onPanel = true;
  shown.atMs = millis();
  shown.activityChanges = now.activityChanges;
  shown.renderPasses = now.renderPasses;
  LOG_DBG("TGL", "Message shown (%s)", pending.on ? "on" : "off");
}

}  // namespace ToggleNotice
