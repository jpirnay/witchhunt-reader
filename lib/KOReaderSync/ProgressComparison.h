#pragma once

#include <cstdint>

#include "CrossPointPosition.h"

// Which side of a sync is further along.
//
// A KOSync record's percentage is the other device's rendered page over its page count; ours is
// XHTML bytes. The two routinely differ by a percent or two of the book, so comparing them puts a
// later position behind an earlier one whenever the gap spans a chapter boundary (#268's family).
// The comparison therefore prefers evidence in the same units on both sides, and uses the
// percentage only as the last resort:
//   1. the spine, when the record names one (DocFragment) -- exact;
//   1b. within the same spine, the content offsets when both sides have them: the remote's offset
//      falls on the local page exactly when start(p) <= offset < end(p), where end(p) is the first
//      later page start greater than start(p) (UINT32_MAX on the last page); on page 0 anything
//      below start(0) counts too, as the offset lookup resolves it there -- exact for every book,
//      laid out or not on the other side;
//   2. within the same spine, the paragraph LUT: a remote p[K] opens on the local page p exactly
//      when K(p-1) < K <= K(p), where K(i) is the LUT index at the end of page i -- exact for books
//      whose paragraphs are children of <body>;
//   3. the percentages, with a tolerance for rounding -- an estimate, bounded to one chapter by 1;
//   4. Unknown, when neither side's percentage is usable: the caller asks rather than guesses.
enum class ProgressComparison : uint8_t { LocalAhead, Synchronized, RemoteAhead, Unknown };

// Where the reader is, as the reader itself knows it. The paragraph fields are the section
// cache's paragraph LUT at the end of `page` and at the end of the page before it (0 on the first
// page); 0 means the LUT has no body-child paragraph there, and the paragraph tier is skipped.
struct LocalReadingPosition {
  int spineIndex = 0;
  int page = 0;
  uint16_t paragraphAtPageEnd = 0;
  uint16_t paragraphAtPreviousPageEnd = 0;
  // The page's start and the end of its window as visible-text offsets (Section's LUT);
  // visibleOffsetAtNextPage is UINT32_MAX on the last page. hasVisibleOffset false when the chapter
  // has no LUT, and the paragraph tier takes over.
  uint32_t visibleOffsetAtPage = 0;
  uint32_t visibleOffsetAtNextPage = UINT32_MAX;
  bool hasVisibleOffset = false;
};

ProgressComparison compareProgress(const LocalReadingPosition& local, float localPercentage,
                                   const CrossPointPosition& remote, float remotePercentage);

// Two records for the same book under two document ids (filename and binary hash): which one is
// the live position. Primary unless the alternate is strictly further by the same evidence order
// (spine, then paragraph index, then percentage); a tie or an unknown keeps the primary, which is
// also the id uploads go to.
enum class RemoteRecordChoice : uint8_t { Primary, Alternate };

RemoteRecordChoice selectRemoteRecord(const CrossPointPosition& primary, float primaryPercentage,
                                      const CrossPointPosition& alternate, float alternatePercentage);
