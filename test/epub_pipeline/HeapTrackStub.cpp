// No-op HeapTrack for the dump variant that must NOT override malloc/free
// process-wide (the override deadlocks under the MinGW runtime).
#include "HeapTrack.h"

void heapTrackBegin() {}
size_t heapTrackEnd() { return 0; }
size_t heapTrackAllocCount() { return 0; }
void heapTrackSizeHistogram(size_t* out, const int count) {
  for (int i = 0; i < count; i++) out[i] = 0;
}
