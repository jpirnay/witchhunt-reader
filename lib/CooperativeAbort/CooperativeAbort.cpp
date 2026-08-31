#include "CooperativeAbort.h"

#include "LongTaskProgress.h"

namespace CooperativeAbort {
namespace {
bool (*gAbortPredicate)() = nullptr;
bool gAborted = false;
}  // namespace

bool shouldAbortLongTask() {
  // Every long task in the lib layer already polls this at its natural boundary, which makes
  // it the one place that sees them all. Recording liveness here means a new yield point
  // gets it for free, and none of the existing ones had to be touched — see
  // LongTaskProgress.h for why those points can report progress but must not paint.
  LongTaskProgress::noteAlive();
  return gAbortPredicate != nullptr && gAbortPredicate();
}

void setLongTaskAbortPredicate(bool (*predicate)()) { gAbortPredicate = predicate; }

void markAborted() { gAborted = true; }

bool wasAborted() { return gAborted; }

bool consumeAborted() {
  const bool was = gAborted;
  gAborted = false;
  return was;
}

void clearAborted() { gAborted = false; }

}  // namespace CooperativeAbort
