#pragma once

#include <BookCache.h>

#include <functional>
#include <string>

#include "activities/Activity.h"
#include "activities/util/ConfirmationActivity.h"

// WARNING asks whether to clear through a ConfirmationActivity pushed above this one, like every
// other question; the answer starts the clearing, keeping the books' bookmarks and reading
// progress or not, or leaves. The other states are progress/result screens with nothing to answer.
class ClearCacheActivity final : public Activity {
 public:
  explicit ClearCacheActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClearCache", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
  void render(RenderLock&&) override;

  // What an answer other than Cancel to askToKeepReadingState()'s question says the clear keeps.
  static BookCache::Keep keepFor(const ActivityResult& answer);

 private:
  enum State { WARNING, CLEARING, SUCCESS, FAILED };

  // Every clear of a book's cache asks whether the bookmarks and reading progress stay: this puts
  // that question and its answers into `question`.
  static void askToKeepReadingState(ConfirmationActivity::Question& question);

  State state = WARNING;

  void goBack() { finish(); }

  int clearedCount = 0;
  int failedCount = 0;
  void clearCache(BookCache::Keep keep);

  void startClearing(BookCache::Keep keep);
};
