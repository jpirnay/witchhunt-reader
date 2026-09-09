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
// Suspend/resume counting without clearing the counters. The dump harness reads every page back
// through the same Section code the build uses, and that read-back is test scaffolding that never
// runs on device -- profiling it made loadPageFromSectionFile the largest "allocation site" in the
// whole book. Bracket anything that is not the build.
void heapTrackPause();
void heapTrackResume();
// Number of allocations since begin(). Peak bytes alone hides allocation CHURN — many
// short-lived blocks can leave peak flat while still fragmenting a no-compaction heap,
// which is the failure mode on the ESP32-C3. Valid after heapTrackEnd().
size_t heapTrackAllocCount();
// Power-of-two size histogram of allocations since begin(): [0]=<=16B, [1]=<=32B, ... [11]=>16KB.
// Fills up to `count` buckets. Reveals which allocations dominate by COUNT, which is what
// fragments a no-compaction heap.
void heapTrackSizeHistogram(size_t* out, int count);
// Top allocation SITES, as raw return addresses (symbolize with addr2line). Writes up to `count`
// entries into out[]; returns how many were written, ordered by peakLive.
//
// `bytes` is CUMULATIVE and answers "who allocates a lot" -- on a churning parser that is almost
// always the per-word string traffic, and almost always the wrong thing to change. `peakLive` is
// what the site was still HOLDING when the run hit its high-water mark, which is what a 380 KB
// device actually runs out of.
struct HeapTrackSite {
  unsigned long long pc;
  size_t count;
  size_t bytes;
  size_t peakLive;
};
int heapTrackTopSites(HeapTrackSite* out, int count);
