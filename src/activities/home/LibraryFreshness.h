#pragma once

#include <cstdint>

class LibraryIndexReader;

// When the book index has to be built again before New or Authors show it
// (docs/design/library-index.md, "When it rebuilds").
namespace LibraryFreshness {

// True when there is no valid index, no build has finished since boot (a card edited elsewhere while
// the device was off or asleep is invisible otherwise), the card has changed since the last build
// started, or Browse Files' hidden-files setting differs from the one the index was built with.
bool stale(const LibraryIndexReader& index);

// A build that started at `generationAtStart` (HalStorage::contentGeneration) has finished.
void built(uint32_t generationAtStart);

}  // namespace LibraryFreshness
