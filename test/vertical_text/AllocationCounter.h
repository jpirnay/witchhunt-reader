#pragma once

// Counts of this test binary's heap use, kept by the replacements of the global operator new and delete in
// AllocationCounter.cpp: allocations alive now, and every allocation made.
long liveAllocations();
long allocations();
