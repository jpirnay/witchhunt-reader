#pragma once
// Whole-process heap tracking for the epub_pipeline_dump benchmark mode.
// Linked ONLY into the CLI tool — it overrides malloc/free process-wide
// (same header-word technique as test/epub_benchmark), which is unwanted
// inside the gtest binary.
#include <cstddef>

// Zero the live/peak counters and start tracking.
void heapTrackBegin();
// Stop tracking and return the peak live bytes observed since begin().
size_t heapTrackEnd();
// Number of allocations since begin(). Peak bytes alone hides allocation CHURN — many
// short-lived blocks can leave peak flat while still fragmenting a no-compaction heap,
// which is the failure mode on the ESP32-C3. Valid after heapTrackEnd().
size_t heapTrackAllocCount();
// Power-of-two size histogram of allocations since begin(): [0]=<=16B, [1]=<=32B, ... [11]=>16KB.
// Fills up to `count` buckets. Reveals which allocations dominate by COUNT, which is what
// fragments a no-compaction heap.
void heapTrackSizeHistogram(size_t* out, int count);
// Top allocation SITES by count, as raw return addresses (symbolize with addr2line).
// Writes up to `count` pairs into out[]; returns how many were written.
struct HeapTrackSite {
  unsigned long long pc;
  size_t count;
  size_t bytes;
};
int heapTrackTopSites(HeapTrackSite* out, int count);
