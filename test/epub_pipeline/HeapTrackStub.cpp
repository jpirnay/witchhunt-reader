// No-op HeapTrack for the dump variant that must NOT override malloc/free
// process-wide (the override deadlocks under the MinGW runtime).
#include "HeapTrack.h"

void heapTrackBegin() {}
size_t heapTrackEnd() { return 0; }
