#include "Activity.h"

#include "ActivityManager.h"
#include "components/themes/ButtonHintStrip.h"

// The recorded button-hint strip belongs to the screen that painted it. Drop it on every
// transition, in both directions: a screen that draws no hints would otherwise inherit the
// previous one's boxes, and taps near the bottom edge would fire phantom button presses.
// Each screen re-records on its next render, so the only gap is between the transition and
// that render -- during which there is correctly no strip.
void Activity::onEnter() {
  ButtonHintStrip::invalidate();
  LOG_DBG("ACT", "Entering activity: %s", name.c_str());
}

void Activity::onExit() {
  ButtonHintStrip::invalidate();
  LOG_DBG("ACT", "Exiting activity: %s", name.c_str());
}

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

void Activity::requestUpdateAndWait() { activityManager.requestUpdateAndWait(); }

bool Activity::isUpdateSuperseded() const { return activityManager.isUpdateSuperseded(); }

// "Up and out" — return to whichever parent launched this flow. If no return hint
// is set (typical for activities launched via a plain goTo*()), falls back to Home.
void Activity::onGoHome() { activityManager.returnFromChild(); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }
