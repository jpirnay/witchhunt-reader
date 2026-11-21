#include "ProgressComparison.h"

namespace {
// Rounding noise between two percentages of the same book, not a real difference: a tenth of a percent.
constexpr uint32_t SAME_PROGRESS_EPSILON = ProgressPpb::kWhole / 1000;

ProgressComparison byPercentage(const std::optional<ProgressPpb> local, const std::optional<ProgressPpb> remote) {
  if (!local || !remote) {
    return ProgressComparison::Unknown;
  }
  if (distance(*local, *remote) <= SAME_PROGRESS_EPSILON) return ProgressComparison::Synchronized;
  return *local > *remote ? ProgressComparison::LocalAhead : ProgressComparison::RemoteAhead;
}
}  // namespace

ProgressComparison compareProgress(const LocalReadingPosition& local, const std::optional<ProgressPpb> localPercentage,
                                   const CrossPointPosition& remote,
                                   const std::optional<ProgressPpb> remotePercentage) {
  if (remote.hasResolvedSpineIndex) {
    if (local.spineIndex != remote.spineIndex) {
      return local.spineIndex > remote.spineIndex ? ProgressComparison::LocalAhead : ProgressComparison::RemoteAhead;
    }
    // Same chapter. Content offsets, when both sides have them, are exact for every book.
    if (local.hasVisibleOffset && remote.hasVisibleTextOffset) {
      // Page 0 also owns offsets below its start (hidden text KOReader shows), as the offset lookup does.
      if (local.page != 0 && remote.visibleTextOffset < local.visibleOffsetAtPage) {
        return ProgressComparison::LocalAhead;
      }
      if (remote.visibleTextOffset < local.visibleOffsetAtNextPage) return ProgressComparison::Synchronized;
      return ProgressComparison::RemoteAhead;
    }
    // The paragraph LUT answers exactly where a remote p[K] would open: on the local
    // page when K(p-1) < K <= K(p), before it when K <= K(p-1), after it otherwise.
    if (remote.hasParagraphIndex && local.paragraphAtPageEnd > 0) {
      const uint16_t k = remote.paragraphIndex;
      if (k <= local.paragraphAtPreviousPageEnd) return ProgressComparison::LocalAhead;
      if (k <= local.paragraphAtPageEnd) return ProgressComparison::Synchronized;
      return ProgressComparison::RemoteAhead;
    }
  }
  return byPercentage(localPercentage, remotePercentage);
}

RemoteRecordChoice selectRemoteRecord(const CrossPointPosition& primary,
                                      const std::optional<ProgressPpb> primaryPercentage,
                                      const CrossPointPosition& alternate,
                                      const std::optional<ProgressPpb> alternatePercentage) {
  if (primary.hasResolvedSpineIndex && alternate.hasResolvedSpineIndex) {
    if (primary.spineIndex != alternate.spineIndex) {
      return alternate.spineIndex > primary.spineIndex ? RemoteRecordChoice::Alternate : RemoteRecordChoice::Primary;
    }
    if (primary.hasParagraphIndex && alternate.hasParagraphIndex) {
      return alternate.paragraphIndex > primary.paragraphIndex ? RemoteRecordChoice::Alternate
                                                               : RemoteRecordChoice::Primary;
    }
  }
  return byPercentage(primaryPercentage, alternatePercentage) == ProgressComparison::RemoteAhead
             ? RemoteRecordChoice::Alternate
             : RemoteRecordChoice::Primary;
}
