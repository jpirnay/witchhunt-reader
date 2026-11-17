#pragma once

#include <cstdint>
#include <string>

// How many pages each spine item lays out to under one set of render settings, kept on SD at
// `<book cache>/pages.bin` so the reader can count a chapter split over several spine items
// (#325) without holding a per-spine table in RAM (books with 1700+ spine items exist) and
// without opening each sibling's section cache: every FAT open scans its directory linearly, so a
// chapter of N files would cost N directory walks. Summing a chapter here is one open in the book
// cache root plus one sequential read of 6 bytes per spine item.
//
// One render variant at a time: recording under another (font, margins, a section layout
// version bump) starts the table over, since the old counts describe nothing the reader shows.
// An entry is a fact about a deterministic layout, so it stays right when its section cache is
// evicted: a rebuild under the same variant lays out the same pages.
namespace SpinePageIndex {

struct Variant {
  uint32_t propertyHash = 0;
  uint8_t layoutVersion = 0;
};

// Records one spine item's page count and inflated byte size. The entry is rewritten only when it
// changed, so recording on every section load costs a read, not an SD write.
void record(const std::string& bookCachePath, Variant variant, int spineCount, int spineIndex, uint16_t pages,
            uint32_t bytes);

// A carried spine item's entry: 0 pages and these bytes. Older firmware reads 0 pages as "not recorded" and estimates
// the item, as before; this one counts it as a file with no page.
constexpr uint32_t kCarriedBytes = UINT32_MAX;
// Records that spine item `spineIndex` has no page of its own: a file holding only its chapter's heading, which the
// next item's layout takes in (HeadingCarry).
void recordCarried(const std::string& bookCachePath, Variant variant, int spineCount, int spineIndex);

// The recorded entries of spines [first, last] other than `current`, summed on each side of it. A carried item counts
// as a file of no pages and no bytes.
struct Totals {
  uint32_t pagesBefore = 0;
  uint32_t pagesAfter = 0;
  uint32_t bytesBefore = 0;
  uint32_t bytesAfter = 0;
  uint16_t filesBefore = 0;  // spine items with an entry
  uint16_t filesAfter = 0;
};
// All zero when there is no table for `variant`.
Totals sumRange(const std::string& bookCachePath, Variant variant, int spineCount, int first, int last, int current);

// The last spine item in [first, last] with no entry under `variant` (all of `last` when there is no table for it),
// or -1 when every one has an entry. An entry is unrecorded only while it is all zero: one written with no pages is a
// file known to have none. One open and one sequential read, whatever the range.
int lastUnrecorded(const std::string& bookCachePath, Variant variant, int spineCount, int first, int last);

// Drops the table, for a layout change the variant cannot see: a TOC recovered after the book was
// read without one starts new pages at its chapter anchors.
void discard(const std::string& bookCachePath);

}  // namespace SpinePageIndex
