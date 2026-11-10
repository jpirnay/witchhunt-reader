#pragma once
// The shaping engine's heap check (ot::setHeapCheck, lib/OtShaper) for the span of a layout build. While one of these
// lives, every block the engine asks it about (a plan's lists, its lookups' digests and filters, the plan builder's
// scratch, and ShapingFace::plan's Plan itself) must leave heap_growth::kShapedFreeFloor free, as the shaped store's
// growths do (heap_growth::shapedHeapAllows). A plan refused so is not built: its runs are laid out as their code
// points and the section latches shaping degraded, rebuilt once (docs/text-shaping.md, "What fails how"); refused
// filters only leave the plan's subtables unfiltered.
//
// Section::BuildState and buildDictionaryHtmlPages hold one for their layout. The reader's build and Background-B's may
// be in flight at once, so the check is counted: the first one in installs it, the last one out takes it away. It is
// counted where a build starts and ends, as the build's other global state is (TextShaping::releaseBuildMemory,
// GfxRenderer::releaseShapingFaces).
//
// Outside every build no check is installed, and the engine's blocks are guarded by nothrow alone: UI text
// (UiShapedText) shapes over plans its pass releases at its end, with no build's working set to keep, and draws a
// title whose plan is refused reordered. A UI pass that falls between two slices of a build shapes under the build's
// floor.

#include <OtBuffer.h>

#include "HeapGrowth.h"

class ShapingHeapCheck {
 public:
  ShapingHeapCheck() {
    // cppcheck-suppress knownConditionTrueFalse ; live_ counts builds in flight, not always 0
    if (live_++ == 0) ot::setHeapCheck(&heap_growth::shapedHeapAllows);
  }
  ~ShapingHeapCheck() {
    // cppcheck-suppress knownConditionTrueFalse ; the last build out removes the check
    if (--live_ == 0) ot::setHeapCheck(nullptr);
  }
  ShapingHeapCheck(const ShapingHeapCheck&) = delete;
  ShapingHeapCheck& operator=(const ShapingHeapCheck&) = delete;

 private:
  static inline int live_ = 0;
};
