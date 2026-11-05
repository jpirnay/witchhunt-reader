#include "EpubNavigationTarget.h"

#include <Logging.h>

std::optional<int> EpubNavigationTarget::relaidOutRestorePage(const Section& sec, const int spineIndex) const {
  if (restoreParagraph == 0 || cachedSpineIdx != spineIndex || cachedPageCount <= 0 ||
      sec.pageCount == cachedPageCount) {
    return std::nullopt;
  }
  const auto p = sec.getPageForParagraphIndex(restoreParagraph);
  if (!p) return std::nullopt;
  LOG_DBG("ERS", "Chapter relaid out (%d -> %d pages): restoring p[%u] -> page %u", cachedPageCount, sec.pageCount,
          restoreParagraph, *p);
  return *p;
}

void EpubNavigationTarget::resolveInto(Section& sec, int spineIndex) const {
  // Resolve to a baseline page first. Each branch records whether it produced a
  // precise page (LUT/anchor hit, percent jump, explicit page) or only an estimate.
  // The estimate path runs cross-spine rescale + clamp at the end; the precise path
  // skips both because LUT pages are already in the target spine's coordinate system.
  bool isEstimate = false;

  switch (kind) {
    case Kind::LastPage: {
      sec.currentPage = (sec.pageCount > 0) ? sec.pageCount - 1 : 0;
      break;
    }

    case Kind::TocIndex: {
      if (const auto p = sec.getPageForTocIndex(tocIndex)) {
        sec.currentPage = *p;
      }
      break;
    }

    case Kind::Anchor: {
      if (const auto p = sec.getPageForAnchor(anchorStr)) {
        sec.currentPage = *p;
        LOG_DBG("ERS", "Resolved anchor '%s' -> page %d", anchorStr.c_str(), *p);
      } else if (sec.isTruncatedCache()) {
        // The chapter did not finish building, so its anchor map stops where the parse stopped and
        // a miss says nothing about whether the id exists. Distinct log level because the two
        // cases want opposite fixes: a genuine miss is a bad href in the book, this one is the
        // build running out of heap -- and the destination below is then meaningless rather than
        // merely approximate. Device 2026-09-09: a note 46% into a 90-page notes chapter that had
        // built 31 pages landed the reader on page 0.
        LOG_ERR("ERS", "Anchor '%s' missing from a TRUNCATED spine cache (%u pages built); the jump is a guess",
                anchorStr.c_str(), static_cast<uint32_t>(sec.pageCount));
        sec.currentPage = fallbackPage;
        isEstimate = true;
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found; using fallback page %d", anchorStr.c_str(), fallbackPage);
        sec.currentPage = fallbackPage;
        isEstimate = true;
      }
      break;
    }

    case Kind::ListItem: {
      if (const auto p = sec.getPageForListItemIndex(lutIndex)) {
        sec.currentPage = *p;
        LOG_DBG("ERS", "Resolved li[%u] -> page %d", lutIndex, *p);
      } else if (const auto pp = sec.getPageForParagraphIndex(lutIndex)) {
        // Some <li>-anchored XPaths land in books where the LI LUT is empty (no <li>
        // inside <body>'s direct children, or all <li>s skipped). Fall back to the
        // paragraph LUT — the running indices coincide often enough to help, and
        // it's strictly better than dropping back to the estimate.
        sec.currentPage = *pp;
        LOG_DBG("ERS", "Li LUT miss for li[%u]; paragraph LUT -> page %d", lutIndex, *pp);
      } else {
        LOG_DBG("ERS", "Li[%u] not in LUT; using fallback page %d", lutIndex, fallbackPage);
        sec.currentPage = fallbackPage;
        isEstimate = true;
      }
      break;
    }

    case Kind::VisibleOffset: {
      if (const auto p = sec.getPageForVisibleTextOffset(visibleOffset)) {
        sec.currentPage = *p;
        LOG_DBG("ERS", "Resolved offset %u -> page %d", visibleOffset, *p);
      } else {
        LOG_DBG("ERS", "No offset LUT for offset %u; using fallback page %d", visibleOffset, fallbackPage);
        sec.currentPage = fallbackPage;
        isEstimate = true;
      }
      break;
    }

    case Kind::Paragraph: {
      if (const auto p = sec.getPageForParagraphIndex(lutIndex)) {
        sec.currentPage = *p;
        LOG_DBG("ERS", "Resolved p[%u] -> page %d", lutIndex, *p);
      } else {
        LOG_DBG("ERS", "Paragraph LUT miss for p[%u]; using fallback page %d", lutIndex, fallbackPage);
        sec.currentPage = fallbackPage;
        isEstimate = true;
      }
      break;
    }

    case Kind::Percent: {
      if (sec.pageCount > 0) {
        int newPage = static_cast<int>(spineProgress * static_cast<float>(sec.pageCount));
        if (newPage >= sec.pageCount) newPage = sec.pageCount - 1;
        sec.currentPage = newPage;
      }
      break;
    }

    case Kind::Page: {
      sec.currentPage = page;
      isEstimate = true;
      if (const auto p = relaidOutRestorePage(sec, spineIndex)) {
        sec.currentPage = *p;
        isEstimate = false;
      }
      break;
    }
  }

  // Cross-font / cross-spine rescaling: only for estimated pages. cachedPageCount
  // is the page count at the time the estimate was made — when it disagrees with
  // the section's current page count (reflow / different spine entirely), rescale
  // the estimate proportionally before clamping.
  if (isEstimate && cachedPageCount > 0 && cachedSpineIdx == spineIndex && sec.pageCount != cachedPageCount) {
    const float progress = static_cast<float>(sec.currentPage) / static_cast<float>(cachedPageCount);
    sec.currentPage = static_cast<int>(progress * static_cast<float>(sec.pageCount));
  }

  // Safety clamp for all paths — a LUT-derived page is also defensively clamped in
  // case the cache is somehow stale.
  if (sec.currentPage < 0) {
    LOG_DBG("ERS", "Clamping negative page %d to 0 (spine=%d cachedPageCount=%d)", sec.currentPage, spineIndex,
            cachedPageCount);
    sec.currentPage = 0;
  }
  if (sec.pageCount > 0 && sec.currentPage >= sec.pageCount) {
    LOG_DBG("ERS", "Clamping page %d to last page %d", sec.currentPage, sec.pageCount - 1);
    sec.currentPage = sec.pageCount - 1;
  }
}
