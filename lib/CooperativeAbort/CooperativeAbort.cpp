#include "CooperativeAbort.h"

namespace CooperativeAbort {
namespace {
bool (*gAbortPredicate)() = nullptr;
bool gAborted = false;
}  // namespace

bool shouldAbortLongTask() { return gAbortPredicate != nullptr && gAbortPredicate(); }

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
