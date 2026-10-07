// Which side of a KOReader sync is further along (lib/KOReaderSync/ProgressComparison).
//
// The percentages of the two sides are in different units (the other device's rendered pages vs
// our XHTML bytes), so the comparison has to rank evidence: spine, then the paragraph LUT within
// the spine, then the percentages with a tolerance, then give up. The LUT tier's rule: a remote
// p[K] opens on the local page p exactly when K(p-1) < K <= K(p), K(i) being the LUT index at the
// end of page i. The cases below are the inversions that rule prevents and the edges of the rule.
#include <gtest/gtest.h>

#include <limits>

#include "KOReaderSync/ProgressComparison.h"

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// The reader on `page` of `spine`; the page ends inside paragraph `last`, the previous page ended
// inside paragraph `before`.
LocalReadingPosition reader(const int spine, const int page, const uint16_t before, const uint16_t last) {
  LocalReadingPosition local;
  local.spineIndex = spine;
  local.page = page;
  local.paragraphAtPreviousPageEnd = before;
  local.paragraphAtPageEnd = last;
  return local;
}

LocalReadingPosition readerWithoutLut(const int spine, const int page) { return reader(spine, page, 0, 0); }

// A record whose XPath named `spine` and the body-child paragraph `paragraph` (0: no paragraph).
CrossPointPosition record(const int spine, const uint16_t paragraph) {
  CrossPointPosition remote{};
  remote.spineIndex = spine;
  remote.hasResolvedSpineIndex = true;
  remote.paragraphIndex = paragraph;
  remote.hasParagraphIndex = paragraph > 0;
  return remote;
}

// A record with no DocFragment at all: only its percentage says where it is.
CrossPointPosition percentageOnlyRecord() { return CrossPointPosition{}; }

// --- spine ---------------------------------------------------------------------------------------

TEST(CompareProgress, LaterLocalSpineWinsDespiteLowerPercentage) {
  EXPECT_EQ(compareProgress(reader(4, 1, 0, 2), 0.10f, record(3, 40), 0.90f), ProgressComparison::LocalAhead);
}

TEST(CompareProgress, LaterRemoteSpineWinsDespiteLowerPercentage) {
  EXPECT_EQ(compareProgress(reader(3, 18, 30, 33), 0.90f, record(4, 1), 0.10f), ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, Issue268ChapterStartOnBothSidesIsSynchronized) {
  // KOReader pushed at the heading of chapter 13 (26.72%); the reader is on its first page, where
  // our byte percentage reads 25.1%. Neither side is ahead.
  EXPECT_EQ(compareProgress(reader(12, 0, 0, 1), 0.251f, record(12, 1), 0.2672f), ProgressComparison::Synchronized);
}

// --- paragraph LUT within the spine ---------------------------------------------------------------

TEST(CompareProgress, RemoteParagraphInsideTheLocalPageIsSynchronized) {
  // Page 5 ended in p[14], page 4 in p[11]: p[12], p[13] and p[14] all open on page 5.
  EXPECT_EQ(compareProgress(reader(2, 5, 11, 14), 0.30f, record(2, 12), 0.33f), ProgressComparison::Synchronized);
  EXPECT_EQ(compareProgress(reader(2, 5, 11, 14), 0.30f, record(2, 14), 0.33f), ProgressComparison::Synchronized);
}

TEST(CompareProgress, RemoteParagraphBeforeTheLocalPageIsLocalAhead) {
  // p[11] opened on page 4 (or earlier), so the remote is behind even though its percentage is higher.
  EXPECT_EQ(compareProgress(reader(2, 5, 11, 14), 0.30f, record(2, 11), 0.33f), ProgressComparison::LocalAhead);
}

TEST(CompareProgress, RemoteParagraphAfterTheLocalPageIsRemoteAhead) {
  EXPECT_EQ(compareProgress(reader(2, 5, 11, 14), 0.33f, record(2, 15), 0.30f), ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, FirstPageHasNoPreviousPage) {
  EXPECT_EQ(compareProgress(reader(2, 0, 0, 3), 0.1f, record(2, 1), 0.1f), ProgressComparison::Synchronized);
  EXPECT_EQ(compareProgress(reader(2, 0, 0, 3), 0.1f, record(2, 4), 0.1f), ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, PageInsideOneLongParagraphOpensNowhereNew) {
  // Page 7 starts and ends inside p[9]: no paragraph opens on it, so a remote p[9] lands on the page
  // where p[9] began, which is behind.
  EXPECT_EQ(compareProgress(reader(2, 7, 9, 9), 0.4f, record(2, 9), 0.4f), ProgressComparison::LocalAhead);
  EXPECT_EQ(compareProgress(reader(2, 7, 9, 9), 0.4f, record(2, 10), 0.4f), ProgressComparison::RemoteAhead);
}

// --- percentage fallback -----------------------------------------------------------------------------

TEST(CompareProgress, SameSpineWithoutParagraphsFallsBackToPercentage) {
  // A wrapped (Calibre) book: the LUT holds nothing, the record names no body-child paragraph.
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), 0.30f, record(2, 0), 0.33f), ProgressComparison::RemoteAhead);
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), 0.33f, record(2, 0), 0.30f), ProgressComparison::LocalAhead);
}

TEST(CompareProgress, RemoteParagraphWithoutALocalLutFallsBackToPercentage) {
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), 0.30f, record(2, 12), 0.33f), ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, PercentageFallbackToleratesRounding) {
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), 0.5000f, record(2, 0), 0.5009f), ProgressComparison::Synchronized);
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), 0.502f, record(2, 0), 0.500f), ProgressComparison::LocalAhead);
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), 0.500f, record(2, 0), 0.502f), ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, RecordWithoutASpineUsesOnlyThePercentage) {
  EXPECT_EQ(compareProgress(reader(4, 1, 0, 2), 0.10f, percentageOnlyRecord(), 0.90f), ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, UnusablePercentageWithoutOtherEvidenceIsUnknown) {
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), kNaN, record(2, 0), 0.5f), ProgressComparison::Unknown);
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), 0.5f, record(2, 0), std::numeric_limits<float>::infinity()),
            ProgressComparison::Unknown);
}

TEST(CompareProgress, UnusablePercentageDoesNotMatterWhenTheSpinesDecide) {
  EXPECT_EQ(compareProgress(reader(4, 1, 0, 2), kNaN, record(3, 40), kNaN), ProgressComparison::LocalAhead);
}

// --- content offsets within the spine -------------------------------------------------------------
//
// With the chapter laid out, the reader knows its page's start and the end of its window (the first
// later page start greater than its own, UINT32_MAX on the last page) as visible-text offsets, and a mapped record
// carries the offset its XPath resolved to. The offset decides before the paragraph LUT does: it is exact for every
// book, wrapped ones included.

LocalReadingPosition readerAtOffsets(const int spine, const int page, const uint32_t start, const uint32_t next) {
  LocalReadingPosition local = readerWithoutLut(spine, page);
  local.visibleOffsetAtPage = start;
  local.visibleOffsetAtNextPage = next;
  local.hasVisibleOffset = true;
  return local;
}

CrossPointPosition recordAtOffset(const int spine, const uint32_t offset) {
  CrossPointPosition remote = record(spine, 0);
  remote.visibleTextOffset = offset;
  remote.hasVisibleTextOffset = true;
  return remote;
}

TEST(CompareProgress, RemoteOffsetInsideTheLocalPageIsSynchronized) {
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), 0.30f, recordAtOffset(2, 1000), 0.33f),
            ProgressComparison::Synchronized);
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), 0.30f, recordAtOffset(2, 1399), 0.33f),
            ProgressComparison::Synchronized);
}

TEST(CompareProgress, RemoteOffsetBeforeTheLocalPageIsLocalAhead) {
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), 0.30f, recordAtOffset(2, 999), 0.33f),
            ProgressComparison::LocalAhead);
}

TEST(CompareProgress, RemoteBeforeTheFirstPageStartIsOnTheFirstPage) {
  // A chapter that opens with text we hide but KOReader shows: offsets below start(0) resolve to page 0.
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 0, 300, 900), 0.0f, recordAtOffset(2, 120), 0.01f),
            ProgressComparison::Synchronized);
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 1, 300, 900), 0.0f, recordAtOffset(2, 120), 0.01f),
            ProgressComparison::LocalAhead);
}

TEST(CompareProgress, RemoteOffsetAfterTheLocalPageIsRemoteAhead) {
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), 0.33f, recordAtOffset(2, 1400), 0.30f),
            ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, TheLastPageRunsToTheEndOfTheChapter) {
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 9, 8000, UINT32_MAX), 0.9f, recordAtOffset(2, 123456), 0.95f),
            ProgressComparison::Synchronized);
}

TEST(CompareProgress, OffsetsOutrankTheParagraphTier) {
  // The paragraph tier would say "same page" (p[12] opens on page 5); the offsets know better.
  LocalReadingPosition local = reader(2, 5, 11, 14);
  local.visibleOffsetAtPage = 1000;
  local.visibleOffsetAtNextPage = 1400;
  local.hasVisibleOffset = true;
  CrossPointPosition remote = record(2, 12);
  remote.visibleTextOffset = 1500;
  remote.hasVisibleTextOffset = true;
  EXPECT_EQ(compareProgress(local, 0.3f, remote, 0.3f), ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, OneSideWithoutAnOffsetFallsThrough) {
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), 0.30f, record(2, 0), 0.33f),
            ProgressComparison::RemoteAhead);  // percentage decided
  EXPECT_EQ(compareProgress(reader(2, 5, 11, 14), 0.30f, recordAtOffset(2, 999), 0.33f),
            ProgressComparison::RemoteAhead);  // no local offset, no remote paragraph: percentage
}

// --- choosing between two remote records --------------------------------------------------------------

TEST(SelectRemoteRecord, AlternateMustBeStrictlyAhead) {
  EXPECT_EQ(selectRemoteRecord(record(2, 4), 0.8f, record(3, 1), 0.2f), RemoteRecordChoice::Alternate);
  EXPECT_EQ(selectRemoteRecord(record(2, 4), 0.8f, record(2, 9), 0.2f), RemoteRecordChoice::Alternate);
  EXPECT_EQ(selectRemoteRecord(record(2, 4), 0.2f, record(2, 4), 0.8f), RemoteRecordChoice::Primary);
  EXPECT_EQ(selectRemoteRecord(record(3, 1), 0.2f, record(2, 4), 0.8f), RemoteRecordChoice::Primary);
}

TEST(SelectRemoteRecord, WithoutParagraphsThePercentageDecides) {
  EXPECT_EQ(selectRemoteRecord(record(2, 0), 0.2f, record(2, 0), 0.8f), RemoteRecordChoice::Alternate);
  EXPECT_EQ(selectRemoteRecord(record(2, 0), 0.8f, record(2, 0), 0.2f), RemoteRecordChoice::Primary);
}

TEST(SelectRemoteRecord, UnknownKeepsThePrimary) {
  EXPECT_EQ(
      selectRemoteRecord(percentageOnlyRecord(), kNaN, percentageOnlyRecord(), std::numeric_limits<float>::infinity()),
      RemoteRecordChoice::Primary);
}

}  // namespace
