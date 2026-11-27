#pragma once

#include <functional>

// A screen about to use the radio waits here first while the background KOReader sync (KOReaderSyncWorker) holds it: a
// join of the screen's own drops the job's connection, and a teardown under the job's open socket crashes (main.cpp,
// enterDeepSleep). `showWaiting` puts the screen's "Finishing background sync..." up, and runs only while a job is in
// flight. After 10 s the screen goes on anyway, logged under `tag`. Nothing waits on the boards without the worker (the
// C3s).
namespace background_sync {

void waitBeforeRadioUse(const char* tag, const std::function<void()>& showWaiting);

}  // namespace background_sync
