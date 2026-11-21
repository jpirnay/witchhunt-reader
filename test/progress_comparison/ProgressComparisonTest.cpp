// Which side of a KOReader sync is further along (lib/KOReaderSync/ProgressComparison).
//
// The percentages of the two sides are in different units (the other device's rendered pages vs
// our XHTML bytes), so the comparison has to rank evidence: spine, then the paragraph LUT within
// the spine, then the percentages with a tolerance, then give up. The LUT tier's rule: a remote
// p[K] opens on the local page p exactly when K(p-1) < K <= K(p), K(i) being the LUT index at the
// end of page i. The cases below are the inversions that rule prevents and the edges of the rule.
#include <gtest/gtest.h>

#include <optional>

#include "KOReaderSync/ProgressComparison.h"

namespace {

// A record's percentage that is no usable number (an infinity on the wire; KOReaderJsonNumber.h).
constexpr std::optional<ProgressPpb> kUnusable = std::nullopt;

// A percentage given as a fraction of the book, in the billionths the comparison takes.
constexpr std::optional<ProgressPpb> pct(const double fraction) {
  return ProgressPpb(static_cast<uint32_t>(fraction * ProgressPpb::kWhole + 0.5));
}

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
  EXPECT_EQ(compareProgress(reader(4, 1, 0, 2), pct(0.10), record(3, 40), pct(0.90)), ProgressComparison::LocalAhead);
}

TEST(CompareProgress, LaterRemoteSpineWinsDespiteLowerPercentage) {
  EXPECT_EQ(compareProgress(reader(3, 18, 30, 33), pct(0.90), record(4, 1), pct(0.10)),
            ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, Issue268ChapterStartOnBothSidesIsSynchronized) {
  // KOReader pushed at the heading of chapter 13 (26.72%); the reader is on its first page, where
  // our byte percentage reads 25.1%. Neither side is ahead.
  EXPECT_EQ(compareProgress(reader(12, 0, 0, 1), pct(0.251), record(12, 1), pct(0.2672)),
            ProgressComparison::Synchronized);
}

// --- paragraph LUT within the spine ---------------------------------------------------------------

TEST(CompareProgress, RemoteParagraphInsideTheLocalPageIsSynchronized) {
  // Page 5 ended in p[14], page 4 in p[11]: p[12], p[13] and p[14] all open on page 5.
  EXPECT_EQ(compareProgress(reader(2, 5, 11, 14), pct(0.30), record(2, 12), pct(0.33)),
            ProgressComparison::Synchronized);
  EXPECT_EQ(compareProgress(reader(2, 5, 11, 14), pct(0.30), record(2, 14), pct(0.33)),
            ProgressComparison::Synchronized);
}

TEST(CompareProgress, RemoteParagraphBeforeTheLocalPageIsLocalAhead) {
  // p[11] opened on page 4 (or earlier), so the remote is behind even though its percentage is higher.
  EXPECT_EQ(compareProgress(reader(2, 5, 11, 14), pct(0.30), record(2, 11), pct(0.33)), ProgressComparison::LocalAhead);
}

TEST(CompareProgress, RemoteParagraphAfterTheLocalPageIsRemoteAhead) {
  EXPECT_EQ(compareProgress(reader(2, 5, 11, 14), pct(0.33), record(2, 15), pct(0.30)),
            ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, FirstPageHasNoPreviousPage) {
  EXPECT_EQ(compareProgress(reader(2, 0, 0, 3), pct(0.1), record(2, 1), pct(0.1)), ProgressComparison::Synchronized);
  EXPECT_EQ(compareProgress(reader(2, 0, 0, 3), pct(0.1), record(2, 4), pct(0.1)), ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, PageInsideOneLongParagraphOpensNowhereNew) {
  // Page 7 starts and ends inside p[9]: no paragraph opens on it, so a remote p[9] lands on the page
  // where p[9] began, which is behind.
  EXPECT_EQ(compareProgress(reader(2, 7, 9, 9), pct(0.4), record(2, 9), pct(0.4)), ProgressComparison::LocalAhead);
  EXPECT_EQ(compareProgress(reader(2, 7, 9, 9), pct(0.4), record(2, 10), pct(0.4)), ProgressComparison::RemoteAhead);
}

// --- percentage fallback -----------------------------------------------------------------------------

TEST(CompareProgress, SameSpineWithoutParagraphsFallsBackToPercentage) {
  // A wrapped (Calibre) book: the LUT holds nothing, the record names no body-child paragraph.
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), pct(0.30), record(2, 0), pct(0.33)),
            ProgressComparison::RemoteAhead);
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), pct(0.33), record(2, 0), pct(0.30)),
            ProgressComparison::LocalAhead);
}

TEST(CompareProgress, RemoteParagraphWithoutALocalLutFallsBackToPercentage) {
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), pct(0.30), record(2, 12), pct(0.33)),
            ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, PercentageFallbackToleratesRounding) {
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), pct(0.5000), record(2, 0), pct(0.5009)),
            ProgressComparison::Synchronized);
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), pct(0.502), record(2, 0), pct(0.500)),
            ProgressComparison::LocalAhead);
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), pct(0.500), record(2, 0), pct(0.502)),
            ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, RecordWithoutASpineUsesOnlyThePercentage) {
  EXPECT_EQ(compareProgress(reader(4, 1, 0, 2), pct(0.10), percentageOnlyRecord(), pct(0.90)),
            ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, UnusablePercentageWithoutOtherEvidenceIsUnknown) {
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), kUnusable, record(2, 0), pct(0.5)), ProgressComparison::Unknown);
  EXPECT_EQ(compareProgress(readerWithoutLut(2, 5), pct(0.5), record(2, 0), kUnusable), ProgressComparison::Unknown);
}

TEST(CompareProgress, UnusablePercentageDoesNotMatterWhenTheSpinesDecide) {
  EXPECT_EQ(compareProgress(reader(4, 1, 0, 2), kUnusable, record(3, 40), kUnusable), ProgressComparison::LocalAhead);
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
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), pct(0.30), recordAtOffset(2, 1000), pct(0.33)),
            ProgressComparison::Synchronized);
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), pct(0.30), recordAtOffset(2, 1399), pct(0.33)),
            ProgressComparison::Synchronized);
}

TEST(CompareProgress, RemoteOffsetBeforeTheLocalPageIsLocalAhead) {
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), pct(0.30), recordAtOffset(2, 999), pct(0.33)),
            ProgressComparison::LocalAhead);
}

TEST(CompareProgress, RemoteBeforeTheFirstPageStartIsOnTheFirstPage) {
  // A chapter that opens with text we hide but KOReader shows: offsets below start(0) resolve to page 0.
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 0, 300, 900), pct(0.0), recordAtOffset(2, 120), pct(0.01)),
            ProgressComparison::Synchronized);
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 1, 300, 900), pct(0.0), recordAtOffset(2, 120), pct(0.01)),
            ProgressComparison::LocalAhead);
}

TEST(CompareProgress, RemoteOffsetAfterTheLocalPageIsRemoteAhead) {
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), pct(0.33), recordAtOffset(2, 1400), pct(0.30)),
            ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, TheLastPageRunsToTheEndOfTheChapter) {
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 9, 8000, UINT32_MAX), pct(0.9), recordAtOffset(2, 123456), pct(0.95)),
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
  EXPECT_EQ(compareProgress(local, pct(0.3), remote, pct(0.3)), ProgressComparison::RemoteAhead);
}

TEST(CompareProgress, OneSideWithoutAnOffsetFallsThrough) {
  EXPECT_EQ(compareProgress(readerAtOffsets(2, 5, 1000, 1400), pct(0.30), record(2, 0), pct(0.33)),
            ProgressComparison::RemoteAhead);  // percentage decided
  EXPECT_EQ(compareProgress(reader(2, 5, 11, 14), pct(0.30), recordAtOffset(2, 999), pct(0.33)),
            ProgressComparison::RemoteAhead);  // no local offset, no remote paragraph: percentage
}

// --- choosing between two remote records --------------------------------------------------------------

TEST(SelectRemoteRecord, AlternateMustBeStrictlyAhead) {
  EXPECT_EQ(selectRemoteRecord(record(2, 4), pct(0.8), record(3, 1), pct(0.2)), RemoteRecordChoice::Alternate);
  EXPECT_EQ(selectRemoteRecord(record(2, 4), pct(0.8), record(2, 9), pct(0.2)), RemoteRecordChoice::Alternate);
  EXPECT_EQ(selectRemoteRecord(record(2, 4), pct(0.2), record(2, 4), pct(0.8)), RemoteRecordChoice::Primary);
  EXPECT_EQ(selectRemoteRecord(record(3, 1), pct(0.2), record(2, 4), pct(0.8)), RemoteRecordChoice::Primary);
}

TEST(SelectRemoteRecord, WithoutParagraphsThePercentageDecides) {
  EXPECT_EQ(selectRemoteRecord(record(2, 0), pct(0.2), record(2, 0), pct(0.8)), RemoteRecordChoice::Alternate);
  EXPECT_EQ(selectRemoteRecord(record(2, 0), pct(0.8), record(2, 0), pct(0.2)), RemoteRecordChoice::Primary);
}

TEST(SelectRemoteRecord, UnknownKeepsThePrimary) {
  EXPECT_EQ(selectRemoteRecord(percentageOnlyRecord(), kUnusable, percentageOnlyRecord(), kUnusable),
            RemoteRecordChoice::Primary);
}

}  // namespace
