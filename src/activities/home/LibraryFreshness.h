#pragma once

#include <cstdint>

class LibraryIndexReader;

// When the book index has to be built again before New or Authors show it
// (docs/design/library-index.md, "When it rebuilds").
namespace LibraryFreshness {

// True when there is no valid index, the card changed since the last finished build -- in this boot or
// an earlier one (HalStorage::contentChangedSinceMark) -- or the index was built under the other
// hidden-files setting. A boot or a wake on its own is no reason: the index lives on its card.
bool stale(const LibraryIndexReader& index);

// A build that started at `generationAtStart` (HalStorage::contentGeneration) has finished. With no
// change since it started, the card counts as seen.
void built(uint32_t generationAtStart);

// A build published a new index (its newest book date may have moved).
void published();

// The Books tab lists a book or folder dated `entryDate` (FAT, the later of modified and created).
// One newer than every indexed book was put on the card where the firmware did not see it -- on a
// computer -- and is noted as a change, so New and Authors build again.
void checkListedEntry(uint32_t entryDate);

}  // namespace LibraryFreshness
