#include "BackgroundSyncWait.h"

#include <Logging.h>

#include "activities/reader/KOReaderAutoSync.h"
#if CROSSPOINT_KOREADER_AUTOSYNC
#include "activities/reader/KOReaderSyncWorker.h"
#endif

namespace background_sync {

void waitBeforeRadioUse(const char* tag, const std::function<void()>& showWaiting) {
#if CROSSPOINT_KOREADER_AUTOSYNC
  if (!KOReaderSyncWorker::isBusy()) return;
  showWaiting();
  constexpr unsigned long kDrainTimeoutMs = 10000;
  if (!KOReaderSyncWorker::drain(kDrainTimeoutMs)) {
    LOG_ERR(tag, "Background sync job still running; proceeding anyway");
  }
#else
  (void)tag;
  (void)showWaiting;
#endif
}

}  // namespace background_sync
