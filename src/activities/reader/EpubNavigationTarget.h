#pragma once

#include <Epub/Section.h>

#include <cstdint>
#include <optional>
#include <string>

// Encodes pending navigation intent — where to land once the target section is loaded.
// Replaces the scattered nextPageNumber / pendingTocIndex / pendingAnchor /
// cachedSpineIndex / cachedChapterTotalPageCount / pendingPercent* / pendingParagraph* fields.
struct EpubNavigationTarget {
  enum class Kind : uint8_t {
    Page,           // go to page n (0-based)
    LastPage,       // go to last page of section (was UINT16_MAX sentinel)
    Anchor,         // href fragment (e.g. "note1")
    TocIndex,       // TOC entry index
    Percent,        // normalised 0.0–1.0 within spine
    Paragraph,      // KOReader paragraph LUT index
    ListItem,       // KOReader li-anchored LUT index
    VisibleOffset,  // KOReader content offset (Section::getPageForVisibleTextOffset)
  };
  Kind kind = Kind::Page;
  union {
    int page;                // Kind::Page
    int tocIndex;            // Kind::TocIndex
    float spineProgress;     // Kind::Percent
    uint16_t lutIndex;       // Kind::Paragraph / Kind::ListItem
    uint32_t visibleOffset;  // Kind::VisibleOffset
  };
  std::string anchorStr;  // Kind::Anchor; empty for all others
  // Cross-font rescaling: page count of this spine at save time.
  // Non-zero for Kind::Page when loaded from progress.bin or written during reflow.
  // Also set for Kind::Paragraph / Kind::ListItem / Kind::VisibleOffset / Kind::Anchor so
  // a LUT miss still rescales the estimated fallbackPage instead of stranding at 0.
  int cachedPageCount = 0;
  int cachedSpineIdx = 0;
  // Estimated page used as a baseline before LUT/anchor lookup, and as a fallback
  // when the lookup misses. Only meaningful for Kind::Paragraph / Kind::ListItem /
  // Kind::VisibleOffset / Kind::Anchor — for Kind::Page the `page` field is the baseline.
  int fallbackPage = 0;
  // Kind::Page restored from a saved position (makeSavedPage): the paragraph LUT index of the saved
  // page (0 = none).
  // Used only once the chapter turns out to have been laid out anew (its page count differs from
  // cachedPageCount), in place of rescaling the page number. Otherwise the saved page stands: it
  // is exact, while the paragraph resolves to the page where that paragraph STARTS, a page early
  // whenever the reader had turned into a paragraph running over a page break.
  uint16_t restoreParagraph = 0;
  // Kind::Page restored from a saved position: where the saved page started, in visible text
  // (VisibleText.h). Unlike the page and the paragraph it means the same under any font, size or
  // margin, and every chapter has one: a Calibre book's <body><div><p> counts no paragraphs at all.
  // kNoOffset: not saved, the record is older or the position is not the reader's own.
  static constexpr uint32_t kNoOffset = UINT32_MAX;
  uint32_t restoreOffset = kNoOffset;
  // Kind::Anchor from a followed link that pushed a Back entry: where it lands is checked once
  // against the Back stack (settleFollowedLink). Any other navigation assigns a new target, which
  // clears it.
  bool followedLink = false;

  EpubNavigationTarget() : kind(Kind::Page), page(0) {}

  static EpubNavigationTarget makePage(int n) {
    EpubNavigationTarget t;
    t.kind = Kind::Page;
    t.page = n;
    return t;
  }
  static EpubNavigationTarget makeLastPage() {
    EpubNavigationTarget t;
    t.kind = Kind::LastPage;
    t.page = 0;
    return t;
  }
  static EpubNavigationTarget makeAnchor(std::string a, int fallback = 0) {
    EpubNavigationTarget t;
    t.kind = Kind::Anchor;
    t.page = 0;
    t.anchorStr = std::move(a);
    t.fallbackPage = fallback;
    return t;
  }
  static EpubNavigationTarget makeTocIndex(int idx) {
    EpubNavigationTarget t;
    t.kind = Kind::TocIndex;
    t.tocIndex = idx;
    return t;
  }
  static EpubNavigationTarget makePercent(float sp) {
    EpubNavigationTarget t;
    t.kind = Kind::Percent;
    t.spineProgress = sp;
    return t;
  }
  static EpubNavigationTarget makeParagraph(uint16_t i, int fallback = 0) {
    EpubNavigationTarget t;
    t.kind = Kind::Paragraph;
    t.lutIndex = i;
    t.fallbackPage = fallback;
    return t;
  }
  static EpubNavigationTarget makeListItem(uint16_t i, int fallback = 0) {
    EpubNavigationTarget t;
    t.kind = Kind::ListItem;
    t.lutIndex = i;
    t.fallbackPage = fallback;
    return t;
  }
  static EpubNavigationTarget makeVisibleOffset(uint32_t offset, int fallback = 0) {
    EpubNavigationTarget t;
    t.kind = Kind::VisibleOffset;
    t.visibleOffset = offset;
    t.fallbackPage = fallback;
    return t;
  }
  // A position the reader saved to come back to: progress.bin on a reopen, a link Back entry on
  // Back. The page stands while the chapter still has `pageCount` pages; only a chapter laid out
  // anew lands on `paragraph` (relaidOutRestorePage), or on the page rescaled by page count when
  // there is none. `paragraph` 0 = none, `pageCount` 0 = unknown.
  //
  // One factory for both, because they had drifted apart: the Back stack went straight to the
  // paragraph, which maps to the first page that paragraph index covers -- the page where a long
  // paragraph starts, or, where only some of a chapter's paragraphs are counted (body-child <p>s,
  // see Section::getParagraphIndexForPage), the first of a run of pages that share one index. Back
  // from a note landed pages before the one the reader had left (issue #604).
  static EpubNavigationTarget makeSavedPage(const int spineIndex, const int page, const int pageCount,
                                            const uint16_t paragraph, const uint32_t offset = kNoOffset) {
    EpubNavigationTarget t = makePage(page);
    t.restoreParagraph = paragraph;
    t.restoreOffset = offset;
    t.cachedSpineIdx = spineIndex;
    t.cachedPageCount = pageCount;
    return t;
  }

  // Resolves the target into section.currentPage. Must be called on the render task
  // after the section has been loaded (pageCount is known).
  void resolveInto(Section& section, int spineIndex) const;
  // The page a saved position lands on when `section` has been laid out anew since it was saved:
  // from restoreOffset, else restoreParagraph. nullopt when the saved page stands, or nothing says
  // otherwise. The offset also sees through a new layout that happens to have the same page count.
  std::optional<int> relaidOutRestorePage(const Section& section, int spineIndex) const;
  // A saved position with an offset, as a target that waits for the text rather than the page number: the one a build
  // of its chapter needs, since it draws a Page target as soon as a page of that number exists, and that number
  // belongs to the old layout. The page stays as the fallback.
  [[nodiscard]] bool awaitsItsText() const { return kind == Kind::Page && restoreOffset != kNoOffset; }
  [[nodiscard]] EpubNavigationTarget atItsText() const {
    EpubNavigationTarget t = makeVisibleOffset(restoreOffset, page);
    t.cachedSpineIdx = cachedSpineIdx;
    t.cachedPageCount = cachedPageCount;
    return t;
  }
};
