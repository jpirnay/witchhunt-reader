#define DEBUG_MEMORY_CONSUMPTION 1
#define DEBUG_BACKGROUND_WORK 1
#define DEBUG_BACKGROUND_OVERLAY 0

#ifndef DEBUG_MEMORY_CONSUMPTION
#define DEBUG_MEMORY_CONSUMPTION 0
#endif

// When 1, draws a small overlay in the status bar showing background-work progress:
//   A<.|x>  next-page pre-render: '.' running/scheduled, 'x' ready
//   B<nn%>  next-section background build percent (omitted when inactive)
// and dumps A/B run+complete counters to serial every ~5s. Diagnostic aid for the
// Background A/B work; compiled out (zero cost) when 0.
#ifndef DEBUG_BACKGROUND_WORK
#define DEBUG_BACKGROUND_WORK 0
#endif

#include "EpubReaderActivity.h"

#include <Epub/FootnotePreviews.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderPrintedPageInputActivity.h"
#include "FinishedBookActivity.h"
#include "GlobalBookmarkIndex.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "QrDisplayActivity.h"
#include "QuickOverridesActivity.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "ReadingSessionTracker.h"
#include "RecentBooksStore.h"
#include "SdCardFontGlobals.h"
#include "SilentRestart.h"
#include "StarredPagesActivity.h"
#include "activities/home/BookInfoActivity.h"
#include "activities/settings/ReadingStatsBookDetailActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

// Defined further down (near the other font helpers); declared here because
// buildRenderParams() above it needs the ladder.
static FontSizeLadder buildReaderFontSizeLadder(int bodyFontId);

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()

// Human-readable effective refresh mode for the page-summary diagnostic log.
const char* refreshModeName(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return "full";
    case HalDisplay::HALF_REFRESH:
      return "half";
    case HalDisplay::FAST_REFRESH:
      return "fast";
    default:
      return "?";
  }
}

// Parse a printed-page label as a non-negative integer. Returns nullopt for empty strings,
// strings with non-digit characters (e.g. roman "iv"), and overflow. Used both to gate the
// "go to printed page" menu item and to compute min/max for the numeric input.
std::optional<int> parsePrintedPageLabel(const std::string& label) {
  if (label.empty()) return std::nullopt;
  int value = 0;
  for (char c : label) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + (c - '0');
    if (value > 999999) return std::nullopt;  // sanity
  }
  return value;
}
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_LABELS[] = {1, 1, 3, 6, 12};

// Pre-render of the next page within the current chapter only runs when heap is healthy.
// 56 KB, evidence-based (X3, 2026-06-11): normal foreground renders complete fine from
// ~52 KB free, and post-render free heap sits at ~57-65 KB — the previous 64 KB floor
// silently disabled Background A whenever steady free dipped to ~65 KB (e.g. right
// after a cache rebuild), which also starved Background B behind it.
constexpr uint32_t PRE_RENDER_MIN_FREE_HEAP_BYTES = 56 * 1024;

// Background B (next-section pre-build) heap gates. Unlike the foreground indexing path,
// B runs with the secondary framebuffer live (~52 KB less headroom). Refuse rather than
// risk OOM — the foreground blocking path remains the fallback. Overridable for tuning.
//
// The sliced build runs in two phases with disjoint peaks (see Section::runBuildParse):
//   extract — holds the inflate ring (sized to the entry, ≤32 KB) + ~2 KB scratch, but
//             no layout working set yet;
//   parse   — holds the parser's layout working set (~20 KB), with no ZIP state.
// Floors derived from measured X3 numbers (2026-06-11 serial logs): setup ≈ 12 KB (CSS
// index + visitor), observed safe min-free ≈ 15 KB → ~16 KB reserve.
// Required free heap = max(BG_BUILD_PARSE_MIN_FREE, BG_BUILD_EXTRACT_BASE + ring).
#ifndef BG_BUILD_PARSE_MIN_FREE_HEAP_BYTES
#define BG_BUILD_PARSE_MIN_FREE_HEAP_BYTES (48 * 1024)  // setup + working set + reserve
#endif
#ifndef BG_BUILD_EXTRACT_BASE_HEAP_BYTES
#define BG_BUILD_EXTRACT_BASE_HEAP_BYTES (30 * 1024)  // setup + scratch + reserve (ring added per target)
#endif
#ifndef BG_BUILD_MIN_CONTIG_HEAP_BYTES
#define BG_BUILD_MIN_CONTIG_HEAP_BYTES (24 * 1024)  // parse-phase floor; raised to ring+8 KB while extracting
#endif
// Extra free-heap floor for a CSS section built with the secondary buffer RESIDENT (which B
// always is — it can't release while displaying). The runtime CSS resolver self-protects below
// ~40 KB free (MIN_FREE_HEAP_FOR_CSS) by skipping disk lookups, producing a css-degraded cache
// the foreground must rebuild — so B grinds for seconds then discards. The parse working set
// peaks at ~25-28 KB, so B must start a CSS build with ≥ ~68 KB free to stay above the resolver
// floor mid-parse. Below this, B refuses (stays in WaitHeap) and lets Background-C build the
// section released — with ~120 KB free — when the reader navigates into it. (X3 docs note CSS
// builds are "impossible" resident below ~68 KB free; this is that line, with a small margin.)
#ifndef BG_BUILD_CSS_MIN_FREE_HEAP_BYTES
#define BG_BUILD_CSS_MIN_FREE_HEAP_BYTES (72 * 1024)
#endif
// Per-slice time budget for a Background-B parse step. Conservative start (handoff plan
// suggests 30–50 ms); tune from the DEBUG_BACKGROUND_WORK serial counters.
constexpr uint32_t BG_BUILD_BUDGET_MS = 40;

// Background-B keeps a page-budgeted window of layout ready ahead of the reading position rather
// than a fixed number of sections: it pre-builds whole subsequent spines until roughly this many
// pages of runway exist (counting the current section's unread tail plus the subsequent sections
// built so far), then idles until the reader advances and the window re-anchors. A page budget
// spans spine boundaries naturally — front matter of many tiny one-page spines gets several built,
// while one big chapter already covers the budget on its own. Bounds continuous CPU/SD cost on a
// battery e-reader (vs. indexing the whole book up front).
//
// Adapted from crosspoint-reader's BUILD_WINDOW_AHEAD (PR #2452 by GitHub user itsthisjustin,
// "Lazy incremental EPUB section indexing"); here the window is a page budget that spans spines
// rather than a per-section count.
#ifndef BG_BUILD_LOOKAHEAD_PAGES
#define BG_BUILD_LOOKAHEAD_PAGES 50
#endif

// Foreground in-place section build (the "keep the secondary buffer" path). When heap is
// ample we build the new section WITHOUT releasing the secondary framebuffer, so the chapter's
// first page keeps a valid fast-refresh baseline and avoids the baseline-resetting half-
// refresh. Conservative floors: the foreground build runs at page-turn time with the secondary
// buffer (and possibly a pre-rendered page) resident, so pin above Background-B's idle gate.
// Failure is recoverable — the build retries with the buffer released — so these gate "try in
// place" rather than guaranteeing success; tune from the "Index start mem" serial logs.
#ifndef IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES
#define IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES (60 * 1024)
#endif
#ifndef IN_PLACE_BUILD_MIN_CONTIG_HEAP_BYTES
#define IN_PLACE_BUILD_MIN_CONTIG_HEAP_BYTES (28 * 1024)
#endif
// CSS books need more margin to build in place: the parse resolves embedded styles, which
// self-degrade below the runtime CSS-resolve floor (CSS_MIN_FREE_HEAP_FOR_CSS ≈ 40 KB). Since
// every build is now two-phase (the inflate ring is released BEFORE the CSS-resolving parse),
// the resolve runs with the ring gone, so a higher free floor keeps it clear of 40 KB; contig is
// pinned at the inflate-ring size (≤32 KB) for the extraction phase. A miss is still caught by
// isCssLowHeapDegraded() and rebuilt with the buffer released.
#ifndef IN_PLACE_BUILD_CSS_MIN_FREE_HEAP_BYTES
#define IN_PLACE_BUILD_CSS_MIN_FREE_HEAP_BYTES (66 * 1024)
#endif
#ifndef IN_PLACE_BUILD_CSS_MIN_CONTIG_HEAP_BYTES
#define IN_PLACE_BUILD_CSS_MIN_CONTIG_HEAP_BYTES (32 * 1024)
#endif
// Proactive low-heap guard for a resident (AA-buffer-kept) Background-C build. The in-place start
// floors can't bound the parse's transient working set, which can ride free heap well down on a
// big chapter (a 123 KB CSS section was observed dipping to ~24 KB). If the between-slice baseline
// falls below these, abandon the resident build and rebuild on the released path (frees the
// ~48 KB buffer) before an allocation fails. Pinned below typical mid-build baselines so a build
// that is coping is not aborted, but above the ~13-15 KB zone where heap-pressure faults appear.
#ifndef RESIDENT_BUILD_ABORT_FREE_HEAP_BYTES
#define RESIDENT_BUILD_ABORT_FREE_HEAP_BYTES (30 * 1024)
#endif
#ifndef RESIDENT_BUILD_ABORT_CONTIG_HEAP_BYTES
#define RESIDENT_BUILD_ABORT_CONTIG_HEAP_BYTES (16 * 1024)
#endif

constexpr uint8_t TRUNCATED_SECTION_HINT_RENDER_COUNT = 2;
constexpr const char* TRUNCATED_SECTION_HINT_LINE_1 = "Chapter may be truncated (low memory).";
constexpr const char* TRUNCATED_SECTION_HINT_LINE_2 = "Try: No embedded style | No images | AA Off";

#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
// Temporary corruption tripwire: walks the entire heap and names the checkpoint that
// sees damage. DANGEROUS on this platform: the ESP32-C3 startup-stack heap region
// ([SOC_ROM_STACK_START - SOC_ROM_STACK_SIZE, SOC_ROM_STACK_START)) carries trampled
// TLSF metadata from boot, and tlsf_check chasing its garbage pointers crashes the
// walker (observed: Load access fault inside block_is_free on X4, decoded via
// addr2line). Only enable for dedicated diagnostics sessions, never in normal builds —
// gated on the same flag as the BootHeapProbe static-init probes.
void checkHeapIntegrity(const char* checkpoint) {
  static bool corruptSeen = false;
  if (heap_caps_check_integrity_all(true)) {
    return;
  }
  LOG_ERR("ERS", "HEAP CORRUPT at checkpoint: %s%s", checkpoint, corruptSeen ? " (repeat)" : " <-- FIRST");
  corruptSeen = true;
}
#else
inline void checkHeapIntegrity(const char*) {}
#endif

#if DEBUG_MEMORY_CONSUMPTION
void logReaderMemSnapshot(const char* stage) {
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  LOG_DBG("ERS", "Reader mem[%s]: free=%lu contig=%lu", stage, freeHeap, contigHeap);
}
#else
inline void logReaderMemSnapshot(const char*) {}
#endif

// Computes the [0..100] EPUB progress percent. Returns 0 when pageCount is unknown (sync/bookmark
// pre-render writes), in which case the next saveProgress() will overwrite progress.bin with the
// real value before the user can leave the reader.
uint8_t epubProgressPercentByte(const Epub& epub, const int spineIndex, const int currentPage, const int pageCount) {
  if (pageCount <= 0) {
    return 0;
  }
  const float chapterProgress = static_cast<float>(currentPage) / static_cast<float>(pageCount);
  return ReaderUtils::fractionProgressPercentByte(epub.calculateProgress(spineIndex, chapterProgress));
}

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

int getImageOnlyPageYOffset(const Page& page, const int viewportHeight) {
  if (viewportHeight <= 0 || page.elements.empty()) {
    return 0;
  }

  const bool imageOnlyPage = std::all_of(
      page.elements.begin(), page.elements.end(),
      [](const std::shared_ptr<PageElement>& element) { return element && element->getTag() == TAG_PageImage; });
  if (!imageOnlyPage) {
    return 0;
  }

  int16_t imgX, imgY, imgW, imgH;
  if (!page.getImageBoundingBox(imgX, imgY, imgW, imgH) || imgH >= viewportHeight) {
    return 0;
  }

  const int centeredTop = (viewportHeight - imgH) / 2;
  return std::max(0, centeredTop - static_cast<int>(imgY));
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();
  logReaderMemSnapshot("onEnter_begin");
  // Bisect anchor: corruption already present HERE means the writer ran before the
  // reader (Home sidecar JPEG conversion / thumb generation are prime suspects — see
  // the long-standing "heap may be corrupt after image decode failures" note below).
  checkHeapIntegrity("reader_onEnter");
  secondaryBufferDegraded_ = !renderer.hasSecondaryBuffer();
  // Cold open: arm the dramatic-transition HALF for the first section entry only (cleared by any
  // non-incremental entry in buildSection). Also clear any stale post-popup HALF left armed if the
  // previous reader session was abandoned mid-build.
  coldOpenHalfRefreshArmed_ = true;
  forceHalfRefreshAfterPopup_ = false;
  // Start the refresh cadence at the configured frequency so the first page uses a fast
  // differential. RED RAM is valid: the previous activity's last displayBuffer() called
  // syncRedRamFromFrameBuffer(). If the previous activity set a HALF_REFRESH override via
  // enforceExitFullRefresh(), consumeRefreshOverride() will honour it on the first display call.
  pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();

  // Drop any input events that arrived from the activity that launched us (e.g. a wake-up power
  // button hold) before they reach detectPageTurn() — see ReaderUtils::InputDrainGuard.
  inputDrainGuard.arm();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  {
    RenderLock lock(*this);
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  }
  logReaderMemSnapshot("onEnter_after_orientation");

  epub->setupCacheDir();
  logReaderMemSnapshot("onEnter_after_setupCacheDir");

  if (getEffectiveImageRendering() != CrossPointSettings::IMAGES_SUPPRESS) {
    // Just loads images.bin (or starts an empty cache) — no whole-ZIP scan. Image
    // dimensions are resolved + cached lazily as section indexing first hits each image.
    epub->loadImageManifest();
    logReaderMemSnapshot("onEnter_after_image_manifest");
  }

  // Load the persistent baseline (progress.bin) first. Pending session state
  // (sync result, bookmark jump) is then overlaid on top — this is the only order
  // that lets a Kind::Paragraph / Kind::ListItem navTarget set by applyPendingSyncSession
  // survive into render(). The previous order (apply then load) clobbered the LUT
  // target with Kind::Page from progress.bin, which is why XPath-precision sync
  // silently degraded to the rough page estimate.
  FsFile f;
  bool hadSavedProgress = false;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      navTarget = NavigationTarget::makePage(data[2] + (data[3] << 8));
      navTarget.cachedSpineIdx = currentSpineIndex;
      hadSavedProgress = true;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, navTarget.page);
    }
    if (dataSize == 6) {
      navTarget.cachedPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }
  if (currentSpineIndex < 0 || currentSpineIndex >= epub->getSpineItemsCount()) {
    LOG_ERR("ERS", "Invalid saved spine index %d (valid 0..%d), resetting to start", currentSpineIndex,
            epub->getSpineItemsCount() > 0 ? epub->getSpineItemsCount() - 1 : 0);
    currentSpineIndex = 0;
    navTarget = NavigationTarget::makePage(0);
  }

  applyPendingSyncSession();
  applyPendingBookmarkJump();
  logReaderMemSnapshot("onEnter_after_pending_sync");

  // True first open only: no progress.bin record. Skip the front matter to the text reference.
  // A saved position of spine 0 (reading the cover/chapter 0) must NOT be overridden — the old
  // `currentSpineIndex == 0` test couldn't tell "never opened" from "saved at chapter 0" and
  // bounced the reader to the text start on every reopen at the cover.
  if (!hadSavedProgress && currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }
  logReaderMemSnapshot("onEnter_after_progress_load");

  // Load bookmarks for this book
  bookmarkStore.load(epub->getCachePath());
  logReaderMemSnapshot("onEnter_after_bookmarks_loaded");

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  std::string series = epub->getSeries();
  if (!series.empty() && !epub->getSeriesIndex().empty()) {
    series += " #" + epub->getSeriesIndex();
  }
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), series,
                       ReaderActivity::coverThumbPlaceholder(epub->getPath()));
  const RecentBook currentBook = RECENT_BOOKS.getBookByPath(epub->getPath());
  bookEmbeddedStyleOverride = currentBook.embeddedStyleOverride;
  bookImageRenderingOverride = currentBook.imageRenderingOverride;
  bookFontFamilyOverride = currentBook.fontFamilyOverride;
  bookSdFontFamilyOverride = currentBook.sdFontFamilyOverride;
  bookFontSizeOverride = currentBook.fontSizeOverride;
  bookBionicReadingOverride = currentBook.bionicReadingOverride;
  bookParagraphAlignmentOverride = currentBook.paragraphAlignmentOverride;
  bookTextAntiAliasingOverride = currentBook.textAntiAliasingOverride;
  bookHyphenationOverride = currentBook.hyphenationOverride;
  bookGuideDotsOverride = currentBook.guideDotsOverride;
  bookInlineFootnotePreviewsOverride = currentBook.inlineFootnotePreviewsOverride;
  // Prime the footnote-cache flag with one existence probe so Background-B is not
  // needlessly gated on an already-gathered book (the gather itself runs lazily at
  // the first preview-enabled foreground build).
  footnotePreviewCacheReady_ = FootnotePreviews::cacheExists(epub->getCachePath());
  logReaderMemSnapshot("onEnter_after_recent_books");

  // Start a reading-stats session. We use the cheap filename-based hash here:
  // computing the content hash would re-read the file on every reader open,
  // and a renamed book getting a new stats entry is acceptable — it'll still
  // accumulate going forward.
  globalReadingSessionTracker().begin(KOReaderDocumentId::calculateFromFilename(epub->getPath()), epub->getTitle(),
                                      epub->getAuthor());

  // Trigger first update
  logReaderMemSnapshot("onEnter_before_request_update");
  requestUpdate();
  logReaderMemSnapshot("onEnter_ready");
  checkHeapIntegrity("reader_onEnter_ready");
}

void EpubReaderActivity::onExit() {
  Activity::onExit();
  logReaderMemSnapshot("onExit_before_release");

  // Flush the reading-stats session before tearing down the epub: end() needs
  // no live epub reference and persists the JSON. Sleep paths that bypass
  // onExit() still end up here on resume because the activity is recreated.
  globalReadingSessionTracker().end();
  // If a pre-render left the next page in the frame buffer, redraw the current page so the
  // next activity (notably SleepActivity's OVERLAY mode) sees what the user was looking at.
  // Must run before section.reset() and the orientation reset below.
  restoreCurrentPageToBufferIfPreRendered();

  // Save bookmarks before exit
  bookmarkStore.save();
  if (epub) {
    GLOBAL_BOOKMARKS.syncFromStore(bookmarkStore, epub->getPath(), epub->getCachePath(), epub->getTitle(), false);
  }

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  // Release any deferred AA page before tearing down the section/epub.
  pendingGrayscale_ = {};
  // Abort any in-flight Background-B build (deletes its partial cache file) before the
  // epub it references goes away.
  resetBackgroundBuild();
  section.reset();  // also aborts an in-flight Background-C build of the current section
  // Background-C may have BORROWED the secondary buffer for headroom (lent, not freed); the
  // build is now aborted (section.reset above released into the arena), so hand the block back.
  // The return cannot fail — the region never entered the heap.
  if (secondaryBorrowed_) {
    buildScratch_.reset();
    renderer.returnSecondaryBuffer();
    secondaryBorrowed_ = false;
    secondaryBufferDegraded_ = false;
    LOG_INF("ERS", "onExit: returned secondary buffer borrowed by Background-C");
  }
  // Background-C may instead have RELEASED the secondary buffer for headroom; the build is now
  // aborted, so restore the global "buffer resident" invariant before the next activity renders.
  if (secondaryBufferDegraded_ && !renderer.hasSecondaryBuffer()) {
    if (reallocSecondaryEvictingCaches()) {
      LOG_INF("ERS", "onExit: restored secondary buffer released by Background-C");
    }
    secondaryBufferDegraded_ = false;
  }
  // Restore the display-global single-buffer fast-diff flag unconditionally. Background-C
  // (buildSection) sets it true alongside its release; the two designed restore sites
  // (recoverSecondaryBufferIfNeeded, compileSectionCache) clear it, but exiting mid-build
  // reaches neither. The flag lives on the shared EInkDisplay and outlives this activity,
  // so a stale true would make the next activity's first FAST refresh diff against the
  // controller's retained RED RAM instead of its host baseline (ghosting). No-op on X3.
  renderer.setSingleBufferFastDiff(false);
  UITheme::getInstance().getMutableTheme().onBookWillClose(epub ? epub->getPath() : "", epub.get(), nullptr, nullptr);
  epub.reset();
  currentPageFootnotes.clear();
  currentPageFootnotes.shrink_to_fit();
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // Debug-only: periodic serial dump of background-work counters (no-op in release).
  serviceBackgroundDebugLog();

  if (pendingProgressSave.pending.load(std::memory_order_acquire)) {
    pendingProgressSave.pending.store(false, std::memory_order_relaxed);
    saveProgress(pendingProgressSave.spineIndex, pendingProgressSave.page, pendingProgressSave.pageCount);
  }

  if (inputDrainGuard.shouldDrain(mappedInput)) {
    buttonEvents.drain();
    return;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      buttonEvents.drain();
      stopAutomaticPageTurn();
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  bool delayedPrevTurn = false;
  bool delayedNextTurn = false;
  using BA = CrossPointSettings::BUTTON_ACTION;

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Confirm) {
      if (ev.type == ButtonEventManager::PressType::Long && KOREADER_STORE.hasCredentials()) {
        launchKOReaderSync(SyncLaunchMode::COMPARE);
        return;
      }
      if (ev.type == ButtonEventManager::PressType::Short) {
        if (pageHasPlaceholders) {
          forceLoadLargeImages = true;
          pageHasPlaceholders = false;
          requestUpdate();
          return;
        }
        openReaderMenu();
        return;
      }
    }

    if (ev.button == MappedInputManager::Button::Back) {
      if (ev.type == ButtonEventManager::PressType::Long) {
        ReaderUtils::enforceExitFullRefresh(renderer);
        if (tryAutoPushOnClose()) return;
        onGoHome();
        return;
      }
      if (ev.type == ButtonEventManager::PressType::Short) {
        if (footnoteDepth > 0) {
          restoreSavedPosition();
          return;
        }
        ReaderUtils::enforceExitFullRefresh(renderer);
        if (tryAutoPushOnClose()) return;
        finish();
        return;
      }
    }

    // Built-in default for a long-press on a page-turn button is chapter skip
    // (prev for Left/PageBack, next for Right/PageForward). Non-default long
    // actions are dispatched by the global handler in main.cpp and never reach
    // here, so a Long event arriving for these buttons with the setting at
    // BTN_DEFAULT is the built-in case. markLongPressDispatched() suppresses the
    // wasReleased-based page turn that detectPageTurn() would otherwise fire when
    // the button is released after the skip.
    if (ev.type == ButtonEventManager::PressType::Long) {
      const bool prevChapter =
          (ev.button == MappedInputManager::Button::PageBack && SETTINGS.btnLongPageBack == BA::BTN_DEFAULT) ||
          (ev.button == MappedInputManager::Button::Left && SETTINGS.btnLongLeft == BA::BTN_DEFAULT);
      const bool nextChapter =
          (ev.button == MappedInputManager::Button::PageForward && SETTINGS.btnLongPageForward == BA::BTN_DEFAULT) ||
          (ev.button == MappedInputManager::Button::Right && SETTINGS.btnLongRight == BA::BTN_DEFAULT);
      if (prevChapter || nextChapter) {
        globalButtonEvents().markLongPressDispatched(ev.button);
        onButtonAction(nextChapter ? BA::BTN_NEXT_SECTION : BA::BTN_PREV_SECTION);
        return;
      }
    }

    if (ev.type == ButtonEventManager::PressType::Short) {
      if ((ev.button == MappedInputManager::Button::PageBack && SETTINGS.btnShortPageBack == BA::BTN_DEFAULT &&
           globalButtonEvents().hasDoubleAction(MappedInputManager::Button::PageBack)) ||
          (ev.button == MappedInputManager::Button::Left && SETTINGS.btnShortLeft == BA::BTN_DEFAULT &&
           globalButtonEvents().hasDoubleAction(MappedInputManager::Button::Left))) {
        delayedPrevTurn = true;
        continue;
      }
      if ((ev.button == MappedInputManager::Button::PageForward && SETTINGS.btnShortPageForward == BA::BTN_DEFAULT &&
           globalButtonEvents().hasDoubleAction(MappedInputManager::Button::PageForward)) ||
          (ev.button == MappedInputManager::Button::Right && SETTINGS.btnShortRight == BA::BTN_DEFAULT &&
           globalButtonEvents().hasDoubleAction(MappedInputManager::Button::Right))) {
        delayedNextTurn = true;
        continue;
      }
    }
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    if (!delayedPrevTurn && !delayedNextTurn) {
      // Input-idle and no page turn pending: hand the idle slice to the background
      // routines (deferred AA, then Background A pre-render, then Background B).
      serviceBackgroundWork();
      return;
    }
    prevTriggered = delayedPrevTurn;
    nextTriggered = delayedNextTurn;
  }
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button opens the finished-book flow and back returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      const int lastSpineIndex = epub->getSpineItemsCount() - 1;
      int lastPageIndex = 0;
      int lastPageCount = 0;
      if (section && currentSpineIndex == lastSpineIndex) {
        lastPageCount = section->pageCount;
        lastPageIndex = std::max(0, section->pageCount - 1);
      }
      writeReaderProgressCache(epub->getCachePath(), lastSpineIndex, lastPageIndex, lastPageCount, 100);

      BookFinished::launchFinishedBookFlow(*this, renderer, mappedInput, epub->getPath(), epub->getSeries(),
                                           epub->getSeriesIndex(), epub->getAuthor());
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      navTarget = NavigationTarget::makeLastPage();
      requestUpdate();
    }
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

void EpubReaderActivity::serviceBackgroundWork() {
  // Priority order, highest first. Each routine self-gates on its own pending state and
  // is expected to do a bounded amount of work and return so the next loop tick can
  // service input. Background A is already scheduled inside render() via pendingPreRender
  // and runs as the PreRender pass when requestUpdate() fires; from the idle loop we run
  // the deferred AA pass first, then Background B (next-section pre-build) — B also
  // self-gates on A having finished, since A drives perceived page-turn speed.
  runDeferredGrayscalePass();
  if (pendingGrayscale_.active) {
    return;  // AA still owed (display bus busy); it keeps priority over B/C
  }
  // Background C (build of the section the reader is waiting on) takes priority over A and B:
  // the user has nothing to read until it produces pages. A/B are look-ahead work for a section
  // that is already displayed, so they only matter once the current section is built.
  if (section && section->hasActiveBuild()) {
    stepCurrentSectionBuild();
    return;
  }
  stepBackgroundSectionBuild();
}

void EpubReaderActivity::serviceBackgroundDebugLog() {
#if DEBUG_BACKGROUND_WORK
  const unsigned long now = millis();
  if (lastBgDebugLogMs_ != 0UL && (now - lastBgDebugLogMs_) < 5000UL) {
    return;
  }
  lastBgDebugLogMs_ = now;
  // B state + gate inputs so a stalled B explains itself from the log alone (e.g. parked
  // in waitheap because free/contig sit below the BG_BUILD_* floors, or css=1 with too
  // little heap for Section::heapAllowsEmbeddedStyle()).
  static constexpr const char* kBStateNames[] = {"probe", "waitheap", "building", "settled"};
  LOG_INF("ERS",
          "BG work: A runs=%lu completes=%lu | B runs=%lu completes=%lu state=%s spine=%d css=%d | preReady=%d "
          "buildPct=%d free=%lu contig=%lu",
          static_cast<unsigned long>(bgCounters_.aRuns), static_cast<unsigned long>(bgCounters_.aCompletes),
          static_cast<unsigned long>(bgCounters_.bRuns), static_cast<unsigned long>(bgCounters_.bCompletes),
          kBStateNames[static_cast<uint8_t>(backgroundBuildState_)], backgroundBuildSpineIndex_,
          lastRenderStats.embeddedStyle ? 1 : 0,
          (preRenderedPage.ready && preRenderedPage.spineIndex == currentSpineIndex) ? 1 : 0, backgroundBuildPercent_,
          static_cast<unsigned long>(esp_get_free_heap_size()),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  checkHeapIntegrity("idle_5s");
#endif
}

void EpubReaderActivity::runDeferredGrayscalePass() {
  // Guard: do not start the AA pass while the render task is still inside
  // completeDisplay() — both paths touch the SPI display bus and must not
  // run concurrently. isRefreshPending() is true from triggerDisplay() until
  // completeDisplay() clears it; when it returns false the render task has
  // finished all post-waveform SPI work and it is safe to issue new SPI writes.
  if (!pendingGrayscale_.active || !pendingGrayscale_.page || renderer.isRefreshPending()) {
    return;
  }
  // Serialize deferred AA with the render task. This prevents loop-side
  // grayscale SPI/framebuffer work from racing with render() updates.
  RenderLock lock;
  // Re-check under the lock: the render task may have flipped these between the
  // unlocked test above and acquiring the lock.
  // cppcheck-suppress knownConditionTrueFalse ; render task mutates these concurrently
  if (!pendingGrayscale_.active || !pendingGrayscale_.page || renderer.isRefreshPending()) {
    return;
  }
  pendingGrayscale_.active = false;
  renderer.setFastGrayscaleLut(pendingGrayscale_.fastLut);
  const int fontId = pendingGrayscale_.fontId;
  const int marginLeft = pendingGrayscale_.marginLeft;
  const int contentTop = pendingGrayscale_.contentTop;
  const Page* pagePtr = pendingGrayscale_.page.get();
  const auto gt = renderer.renderGrayscalePlanesSequential([&](GfxRenderer::RenderMode) {
    pagePtr->renderTextOnly(renderer, fontId, marginLeft, contentTop);
    pagePtr->renderImagesFromGrayscaleCache(renderer, marginLeft, contentTop);
  });
  pendingGrayscale_.page.reset();
  LOG_DBG("ERS", "Deferred AA: planes=%lums gray=%lums restore=%lums", gt.planesMs, gt.displayMs, gt.restoreMs);
  checkHeapIntegrity("after_deferred_aa");
  // The AA cleanup just reseeded frameBuffer from frameBufferActive (the current page),
  // so the buffer state is now correct for a pre-render. On X3, render() holds off the
  // PreRender pass while a deferred AA is owed (see the guard there); kick it now so a
  // pre-render armed during that window actually runs against the freshly-settled buffer.
  // X4 never holds off the pre-render (the guard is X3-only there), so this re-request
  // would just be a redundant trigger — skip it to keep X4's refresh sequence unchanged.
  if (renderer.isX3() && pendingPreRender) {
    requestUpdate();
  }
}

Section::BuildParams EpubReaderActivity::makeSectionBuildParams() const {
  const RenderLayout layout = computeRenderLayout();
  Section::BuildParams p;
  p.fontId = getEffectiveReaderFontId();
  p.lineCompression = getEffectiveReaderLineCompression();
  p.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  p.paragraphAlignment = getEffectiveParagraphAlignment();
  p.viewportWidth = layout.viewportWidth;
  p.viewportHeight = layout.viewportHeight;
  p.hyphenationEnabled = getEffectiveHyphenation();
  p.embeddedStyle = lastRenderStats.embeddedStyle;
  p.bionicReadingEnabled = getEffectiveBionicReading();
  p.inlineFootnotePreviews = getEffectiveInlineFootnotePreviews();
  p.imageRendering = lastRenderStats.imageRendering;
  p.fontSizeLadder = buildReaderFontSizeLadder(p.fontId);
  return p;
}

void EpubReaderActivity::resetBackgroundBuild() {
  backgroundSection_.reset();  // ~Section aborts a partial build and deletes its partial file
  backgroundBuildSpineIndex_ = -1;
  backgroundBuildInflatedSize_ = 0;
  backgroundBuildGateCheckMs_ = 0;
  backgroundBuildState_ = BackgroundBuildState::Probe;
  backgroundBuildPercent_ = -1;
}

void EpubReaderActivity::stepBackgroundSectionBuild() {
  if (!epub || !section || readerPhase_ != ReaderPhase::READING) {
    return;
  }
  // Never bake a preview-enabled section variant before footnotes.bin exists: the pages
  // would be cached preview-less under the previews-on hash and stay that way. The gather
  // is foreground-only (first preview-enabled build); B just waits for it.
  if (getEffectiveInlineFootnotePreviews() && !footnotePreviewCacheReady_) {
    return;
  }
  // Background A keeps priority: it determines perceived page-turn speed, and its total
  // cost is small against a multi-second page-read window. Wait until its pass has run
  // (pendingPreRender clears whether or not it produced a ready page).
  if (pendingPreRender || usePreRenderedBuffer) {
    return;
  }
  // B does SD I/O only, no SPI — but it must not contend with the render task for the
  // render lock while a waveform (or a render) is in flight: a blocked loop task cannot
  // service input. Skip the tick instead; idle ticks are plentiful while the user reads.
  // Note RenderLock::peek() returns true when the mutex is HELD (busy), not when free.
  if (renderer.isRefreshPending() || RenderLock::peek()) {
    return;
  }
  // Don't start a heap-hungry build slice while the render task is decoding an image:
  // both compete for the same ~48-52 KB contiguous block. RenderLock::peek() above already
  // excludes this in practice (renderContents() holds the lock for the whole warm pass), but
  // check explicitly too — see the comment on imageProcessingActive_.
  if (imageProcessingActive_) {
    return;
  }
  // One lock for the whole step: every branch below touches the SD (even discarding a
  // stale build removes its partial file) and the parse slice reads glyph metrics from
  // the shared renderer, so all of it must be serialised against the render task.
  RenderLock lock;
  // Re-check under the lock: the render task may have scheduled A or started a refresh
  // between the unlocked test above and acquiring the lock (mirrors runDeferredGrayscalePass).
  // cppcheck-suppress knownConditionTrueFalse ; render task mutates these concurrently
  if (pendingPreRender || renderer.isRefreshPending()) {
    return;
  }

  // Background A re-arm (one retry per displayed page): A's pass runs right after the
  // page render, while the deferred AA still holds the just-rendered page (~10 KB) —
  // its heap floor can refuse at that moment (measured 55.9 KB free vs the 56 KB floor)
  // and nothing retries it. Done HERE, under the render lock, because it dereferences
  // section state the render task mutates — an earlier unlocked version in
  // serviceBackgroundWork() raced buildSection's reassignment of `section`. B keeps
  // waiting behind pendingPreRender until the retry has run, preserving A's priority.
  if (!preRenderedPage.ready && section->currentPage + 1 < section->pageCount &&
      esp_get_free_heap_size() >= PRE_RENDER_MIN_FREE_HEAP_BYTES &&
      (preRenderRearmSpine_ != currentSpineIndex || preRenderRearmPage_ != section->currentPage)) {
    preRenderRearmSpine_ = currentSpineIndex;
    preRenderRearmPage_ = section->currentPage;
    pendingPreRender = true;
    requestUpdate();
    return;
  }

  const int spineCount = epub->getSpineItemsCount();
  // Re-anchor the lookahead window whenever the reading position moves (any navigation):
  // the per-target state held below is then stale, and the cursor restarts at the new +1.
  if (backgroundBuildBaseSpine_ != currentSpineIndex) {
    resetBackgroundBuild();
    backgroundBuildBaseSpine_ = currentSpineIndex;
    backgroundBuildSpineIndex_ = currentSpineIndex + 1;
    backgroundWindowPagesBuilt_ = 0;
  }
  // Walk forward from currentSpineIndex+1 to the book end. The cursor advances as each target
  // settles (Settled case below); already-cached spines settle for free in Probe.
  if (backgroundBuildSpineIndex_ < currentSpineIndex + 1 || backgroundBuildSpineIndex_ >= spineCount) {
    return;
  }
  // Page-budget gate: stop pre-building once ~BG_BUILD_LOOKAHEAD_PAGES of runway is laid out ahead
  // of the reader — the current section's unread tail plus the subsequent sections built so far.
  // Only gate at a section boundary (state==Probe, no build in flight) so a section in progress is
  // never abandoned mid-build; the runway shrinks as the reader advances, re-opening the window.
  if (backgroundBuildState_ == BackgroundBuildState::Probe) {
    const int currentTailPages = (section && section->pageCount > 0)
                                     ? std::max(0, static_cast<int>(section->pageCount) - 1 - section->currentPage)
                                     : 0;
    if (currentTailPages + backgroundWindowPagesBuilt_ >= BG_BUILD_LOOKAHEAD_PAGES) {
      return;
    }
  }
  const int targetSpine = backgroundBuildSpineIndex_;

  switch (backgroundBuildState_) {
    case BackgroundBuildState::Settled: {
      // Target indexed (freshly built or already cached): advance the cursor to the next
      // section. The window/navigation guard above stops the walk at the window or book end.
      const int next = targetSpine + 1;
      resetBackgroundBuild();
      backgroundBuildSpineIndex_ = next;
      return;  // re-probe `next` on the following tick
    }

    case BackgroundBuildState::Probe: {
      // One SD probe per target: if the exact cache variant already exists there is
      // nothing to pre-build.
      backgroundSection_ = std::make_unique<Section>(epub, targetSpine, renderer);
      const Section::BuildParams p = makeSectionBuildParams();
      const bool cached = backgroundSection_->loadSectionFile(
          p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment, p.viewportWidth, p.viewportHeight,
          p.hyphenationEnabled, p.embeddedStyle, p.bionicReadingEnabled, p.inlineFootnotePreviews, p.imageRendering);
      if (cached && !backgroundSection_->isEmbeddedStyleFallback()) {
        backgroundWindowPagesBuilt_ += backgroundSection_->pageCount;  // already-built runway counts toward the budget
        backgroundSection_.reset();
        backgroundBuildState_ = BackgroundBuildState::Settled;
      } else {
        // The inflate ring is sized to the entry, so the extraction heap gate needs the
        // uncompressed size (one central-dir scan, once per target spine).
        backgroundBuildInflatedSize_ = 0;
        epub->getItemSize(epub->getSpineItem(targetSpine).href, &backgroundBuildInflatedSize_);
        backgroundBuildState_ = BackgroundBuildState::WaitHeap;
      }
      return;  // one bounded step per tick
    }

    case BackgroundBuildState::WaitHeap: {
      // Re-check at most ~1×/s: the gates walk the heap free-list, and their outcome
      // only changes when other allocations move — not per ~70 ms loop tick.
      const unsigned long now = millis();
      if (backgroundBuildGateCheckMs_ != 0 && now - backgroundBuildGateCheckMs_ < 1000UL) {
        return;
      }
      backgroundBuildGateCheckMs_ = now;
      const uint32_t ringBytes =
          static_cast<uint32_t>(std::min<size_t>(32768, std::max<size_t>(backgroundBuildInflatedSize_, 512)));
      const uint32_t freeHeap = esp_get_free_heap_size();
      const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
      if (freeHeap <
              std::max<uint32_t>(BG_BUILD_PARSE_MIN_FREE_HEAP_BYTES, BG_BUILD_EXTRACT_BASE_HEAP_BYTES + ringBytes) ||
          contigHeap < std::max<uint32_t>(BG_BUILD_MIN_CONTIG_HEAP_BYTES, ringBytes + 8 * 1024)) {
        return;
      }
      // Refuse — don't let startBuild silently downgrade — when the book wants embedded
      // CSS but the heap can't fit it: a no-CSS background build would only produce the
      // fallback variant and the foreground would still rebuild with CSS on entry.
      // (Silent: state=waitheap + free/contig in the 5 s BG debug line tell the story.)
      if (lastRenderStats.embeddedStyle) {
        const CssParser* css = epub->getCssParser();
        if (!Section::heapAllowsEmbeddedStyle(css ? css->ruleCount() : 0)) {
          return;
        }
        // B builds resident, so a CSS parse below ~68 KB free would dip under the runtime
        // CSS-resolve floor mid-parse and come out css-degraded — seconds of work B then
        // discards. Refuse here and let Background-C build it released (clean) on navigation.
        if (freeHeap < BG_BUILD_CSS_MIN_FREE_HEAP_BYTES) {
          return;
        }
      }
      backgroundBuildState_ = BackgroundBuildState::Building;
      return;
    }

    case BackgroundBuildState::Building: {
      // Layout needs glyph metrics only (advance/kerning), never bitmaps — but the
      // reading-path prewarm leaves SD-font styles wired to a page-scoped FULL (bitmap)
      // cache covering only the displayed page's glyphs, and ensureFontReady's mmap
      // metadata fast-path no-ops when any cache is already wired. The build's layout
      // then misses on nearly every glyph and pulls BITMAPS through the 8-slot overflow
      // loader (~180 ms each; measured 7.4 s of a 9.6 s background build). Reset
      // accumulation so the parser's next metadata-only prewarm re-wires the
      // flash-resident full metric tables — the same thing the foreground indexing path
      // does before createSectionFile. Re-done every slice because an interleaved page
      // render re-enters page mode; for mmap fonts the re-wire is pointer assignments,
      // and the next foreground render rebuilds its page cache in its normal prewarm.
      renderer.clearFontAccumulation();
#if DEBUG_BACKGROUND_WORK
      bgCounters_.bRuns++;
#endif
      const Section::BuildStep step =
          backgroundSection_->stepSectionBuild(makeSectionBuildParams(), BG_BUILD_BUDGET_MS);
      checkHeapIntegrity("after_b_slice");
      if (step == Section::BuildStep::More) {
        // Heap can drop after the WaitHeap gate passed (an interleaved page render allocates).
        // The moment the CSS resolver starts skipping lookups the result is doomed to be
        // css-degraded and discarded — bail now instead of grinding through the rest of the
        // build. Background-C will rebuild it released (clean) when the reader navigates in.
        if (backgroundSection_->activeBuildCssDegraded()) {
          LOG_INF("ERS", "Background build spine=%d css-degrading mid-build; aborting early for foreground rebuild",
                  targetSpine);
          backgroundSection_->abortSectionBuild();
          backgroundSection_.reset();
          backgroundBuildPercent_ = -1;
          backgroundBuildState_ = BackgroundBuildState::Settled;
          return;
        }
        backgroundBuildPercent_ = static_cast<int8_t>(backgroundSection_->activeBuildPercent());
        return;
      }

      backgroundBuildPercent_ = -1;
      if (step == Section::BuildStep::Done) {
        if (backgroundSection_->isTruncatedCache() || backgroundSection_->isCssLowHeapDegraded()) {
          // Memory ran short mid-parse: either pages are missing (truncated) or CSS
          // lookups were skipped (styles silently absent from the cached pages). Don't
          // hand either to the foreground: its blocking path runs with the secondary
          // buffer released (~52 KB more headroom) and will likely build it clean.
          LOG_INF("ERS", "Background build spine=%d %s; discarding for foreground rebuild", targetSpine,
                  backgroundSection_->isTruncatedCache() ? "truncated" : "css-degraded");
          backgroundSection_->clearCache();
          backgroundSection_.reset();
        } else {
#if DEBUG_BACKGROUND_WORK
          bgCounters_.bCompletes++;
#endif
          backgroundWindowPagesBuilt_ += backgroundSection_->pageCount;  // count this section toward the page budget
          LOG_INF("ERS", "Background build spine=%d complete: %u pages", targetSpine, backgroundSection_->pageCount);
        }
      } else {
        LOG_ERR("ERS", "Background build spine=%d failed", targetSpine);
        backgroundSection_.reset();
      }
      // Flush any image dimensions this background build resolved (valid regardless of the
      // build's outcome). One write per completed background section, under the render lock.
      epub->persistImageManifest();
      backgroundBuildState_ = BackgroundBuildState::Settled;
      return;
    }
  }
}

void EpubReaderActivity::stepCurrentSectionBuild() {
  if (!epub || !section || !section->hasActiveBuild()) {
    return;
  }
  // Don't contend with the render task for the lock while a refresh/render is in flight: a
  // blocked loop task can't service input. peek() is true when the mutex is HELD (busy).
  if (renderer.isRefreshPending() || RenderLock::peek()) {
    return;
  }
  // Don't start a build slice while the render task is mid image-decode — see the comment
  // on imageProcessingActive_ in renderContents().
  if (imageProcessingActive_) {
    return;
  }
  RenderLock lock;
  // Re-check under the lock: the render task may have started a refresh, turned a page, or
  // finished/aborted the build between the unlocked test above and acquiring the lock.
  // cppcheck-suppress knownConditionTrueFalse ; render task mutates these concurrently
  if (!section || !section->hasActiveBuild() || renderer.isRefreshPending()) {
    return;
  }

  // Discard the in-flight build and re-render with more headroom. Two retry flavours, chosen by
  // the caller via `retryIncremental`:
  //  - true  (low-heap abort of a RESIDENT build): retry as IncrementalReleased — the release
  //    frees the ~52 KB the build was starved of, and the build stays sliced so the first page
  //    still appears mid-build. Latched via forceReleasedBuildSpine_.
  //  - false (parse failure / truncated / css-degraded): retry on the old blocking released path
  //    (latched via forceBlockingBuildSpine_; compileSectionCache honours the latch by forcing
  //    the buffer release).
  const auto fallbackToReleasedRebuild = [&](const char* reason, const bool retryIncremental) {
    LOG_ERR("ERS", "Background-C spine=%d %s; falling back to released %s rebuild", currentSpineIndex, reason,
            retryIncremental ? "incremental" : "blocking");
    section->clearCache();
    section.reset();  // aborts the in-flight build, releasing into the scratch arena
    // If this build ran inside the BORROWED secondary buffer, hand it back before the released
    // rebuild: borrow only gives phase-b reading-heap (~62 KB), so CSS-heavy books that need the
    // full freed block (~90 KB) fall back here to the legacy release path, which needs the buffer
    // back on the heap first. The return cannot fail (the region never entered the heap). Leave a
    // clean flag state; compileSectionCache re-establishes release/degraded/fast-diff as needed.
    if (secondaryBorrowed_) {
      buildScratch_.reset();
      renderer.returnSecondaryBuffer();
      secondaryBorrowed_ = false;
      secondaryBufferDegraded_ = false;
      renderer.setSingleBufferFastDiff(false);
      LOG_INF("ERS", "Background-C spine=%d: returned borrowed secondary buffer before released rebuild",
              currentSpineIndex);
    }
    if (retryIncremental) {
      forceReleasedBuildSpine_ = currentSpineIndex;
    } else {
      forceBlockingBuildSpine_ = currentSpineIndex;
    }
    readerPhase_ = ReaderPhase::READING;
    buildingPopupShown_ = false;
    buildDisplayedPage_ = -1;
    backgroundBuildPercent_ = -1;
    requestUpdate();  // -> BuildSection -> released (incremental or blocking) build
  };

  // Layout needs glyph metrics only; reset accumulation so the next prewarm re-wires the
  // flash-resident metric tables rather than the page-scoped bitmap cache (see
  // stepBackgroundSectionBuild for the full rationale).
  renderer.clearFontAccumulation();
#if DEBUG_BACKGROUND_WORK
  bgCounters_.bRuns++;
#endif
  const Section::BuildStep step = section->stepSectionBuild(makeSectionBuildParams(), BG_BUILD_BUDGET_MS);
  checkHeapIntegrity("after_c_slice");

  if (step == Section::BuildStep::More) {
    // Proactive low-heap guard: while the build is resident (AA buffer kept), bail to the released
    // path before heap reaches the fault zone. Only meaningful when the buffer is still resident —
    // a build already running released has that headroom and should ride it out.
    if (!secondaryBufferDegraded_ && (esp_get_free_heap_size() < RESIDENT_BUILD_ABORT_FREE_HEAP_BYTES ||
                                      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT) <
                                          RESIDENT_BUILD_ABORT_CONTIG_HEAP_BYTES)) {
      fallbackToReleasedRebuild("low heap mid-build", /*retryIncremental=*/true);
      return;
    }
    backgroundBuildPercent_ = static_cast<int8_t>(section->activeBuildPercent());
    // If the page the user is waiting on just became readable, ask the render task to draw it.
    const int want = section->currentPage;
    if (navTarget.kind == NavigationTarget::Kind::Page && want >= 0 &&
        want < static_cast<int>(section->activeBuildPageCount()) && want != buildDisplayedPage_) {
      requestUpdate();
    }
    return;
  }

  backgroundBuildPercent_ = -1;

  // Failed, or finished but truncated / CSS-degraded: discard and retry on the released path. The
  // latch (set inside the helper) stops buildSection from re-entering Background-C for this spine.
  if (step == Section::BuildStep::Failed || section->isTruncatedCache() || section->isCssLowHeapDegraded()) {
    fallbackToReleasedRebuild(step == Section::BuildStep::Failed ? "failed" : "incomplete",
                              /*retryIncremental=*/false);
    return;
  }

  // Done & clean: the on-disk LUT is written and `section` is now a complete cache. Resolve the
  // navigation target now that the final page count is known, then transition to reading.
#if DEBUG_BACKGROUND_WORK
  bgCounters_.bCompletes++;
#endif
  LOG_INF("ERS", "Background-C spine=%d complete: %u pages", currentSpineIndex, section->pageCount);
  epub->persistImageManifest();
  readerPhase_ = ReaderPhase::READING;

  // Resolve the display position. For a Page target, section->currentPage already tracked the
  // user's position through any mid-build page turns, so DON'T resolveInto (it would reset to the
  // original page). Clamp it; if they ran past the end while building, cross into the next spine.
  const int spineCount = epub->getSpineItemsCount();
  if (navTarget.kind == NavigationTarget::Kind::Page) {
    if (section->currentPage >= section->pageCount) {
      if (currentSpineIndex + 1 < spineCount) {
        navTarget = NavigationTarget::makePage(0);
        currentSpineIndex++;
        section.reset();
      } else if (currentSpineIndex + 1 == spineCount) {
        navTarget = NavigationTarget::makeLastPage();
        currentSpineIndex++;
        section.reset();
      } else {
        section->currentPage = std::max(0, section->pageCount - 1);
      }
    } else if (section->currentPage < 0) {
      section->currentPage = 0;
    }
  } else {
    navTarget.resolveInto(*section, currentSpineIndex);
  }
  if (section) {
    navTarget = NavigationTarget::makePage(section->currentPage);
  }
  forceLoadLargeImages = false;
  pageHasPlaceholders = false;
  buildingPopupShown_ = false;
  buildDisplayedPage_ = -1;
  requestUpdate();  // -> Normal pass renders the resolved page (with AA)
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  float spineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (spineProgress < 0.0f)
    spineProgress = 0.0f;
  else if (spineProgress > 1.0f)
    spineProgress = 1.0f;

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    navTarget = NavigationTarget::makePercent(spineProgress);
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const int tocIdx = section ? section->getTocIndexForPage(section->currentPage)
                                 : epub->getTocIndexForSpineIndex(currentSpineIndex);
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx, tocIdx),
          [this](const ActivityResult& result) {
            if (result.isCancelled) return;
            // The chapter list's own paint consumed the override armed before it launched
            // (one-shot, see consumeRefreshOverride); arm a fresh one here so the resumed
            // reader page gets a clean HALF_REFRESH instead of a FAST diff against RED RAM
            // that still holds the chapter list's last frame.
            ReaderUtils::enforceExitFullRefresh(renderer);
            RenderLock lock(*this);
            const auto& chapter = std::get<ChapterResult>(result.data);
            auto resolvedPage = (chapter.tocIndex && chapter.spineIndex == currentSpineIndex && section)
                                    ? section->getPageForTocIndex(*chapter.tocIndex)
                                    : std::nullopt;
            if (resolvedPage) {
              section->currentPage = *resolvedPage;
              forceLoadLargeImages = false;
              pageHasPlaceholders = false;
            } else {
              navTarget =
                  chapter.tocIndex ? NavigationTarget::makeTocIndex(*chapter.tocIndex) : NavigationTarget::makePage(0);
              currentSpineIndex = chapter.spineIndex;
              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      // Opportunistic: when the book-level footnote cache exists (gathered for inline
      // previews), show each entry's note text in the list. Purely passive — no cache,
      // plain marker list as before; opening the list never triggers a gather.
      std::vector<std::string> footnotePreviews(currentPageFootnotes.size());
      FootnotePreviews::Lookup previewLookup;
      if (previewLookup.open(epub->getCachePath(), epub.get(), currentSpineIndex)) {
        for (size_t i = 0; i < currentPageFootnotes.size(); ++i) {
          previewLookup.find(currentPageFootnotes[i].href, footnotePreviews[i]);
        }
      }
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes,
                                                                           std::move(footnotePreviews)),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PRINTED_PAGE: {
      if (!epub) break;
      // Integer label range for the numeric input's bounds. Streamed (not loadPrintedPageList) so
      // the whole list is never held in RAM — see Epub::hasNumericPrintedPages for why.
      int minLabel = 0;
      int maxLabel = 0;
      if (!epub->getPrintedPageLabelRange(minLabel, maxLabel)) {
        break;  // no integer labels — shouldn't happen if the menu item was shown
      }

      // Pre-fill with the printed page the reader is currently on (or the nearest one before
      // it — rendered device pages rarely carry an anchor themselves, but they sit between
      // two printed pages, so the closest prior anchor is the "you're here" hint). Falls
      // back to the lowest integer label in the book if no prior anchor exists.
      int initialValue = minLabel;
      if (section) {
        if (const auto rawLabel =
                section->getNearestPrintedPageLabelAtOrBefore(static_cast<uint16_t>(section->currentPage))) {
          if (const auto n = parsePrintedPageLabel(*rawLabel)) {
            initialValue = *n;
          }
        }
      }

      startActivityForResult(
          std::make_unique<EpubReaderPrintedPageInputActivity>(renderer, mappedInput, initialValue, minLabel, maxLabel),
          [this](const ActivityResult& result) {
            if (result.isCancelled) return;
            const auto& pick = std::get<PrintedPageResult>(result.data);
            // pick.label is always a numeric string (std::to_string of the picked value). Resolve it
            // back to a (href, anchor) by streaming the list again — no full-list vector retained.
            const auto value = parsePrintedPageLabel(pick.label);
            if (!value) return;
            const auto entry = epub->findPrintedPageByLabel(*value);
            if (!entry) {
              LOG_DBG("ERS", "printed-page jump: label '%s' not found in pagelist", pick.label.c_str());
              return;
            }
            const int spineIdx = epub->resolveHrefToSpineIndex(entry->href);
            if (spineIdx < 0) {
              LOG_DBG("ERS", "printed-page jump: could not resolve spine for href=%s", entry->href.c_str());
              return;
            }
            {
              RenderLock lock(*this);
              currentSpineIndex = spineIdx;
              navTarget =
                  entry->anchor.empty() ? NavigationTarget::makePage(0) : NavigationTarget::makeAnchor(entry->anchor);
              section.reset();
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPageFromSectionFile();
        if (p) {
          std::string fullText;
          for (const auto& el : p->elements) {
            if (el->getTag() == TAG_PageLine) {
              const auto& line = static_cast<const PageLine&>(*el);
              if (line.getBlock()) {
                const auto& block = *line.getBlock();
                const uint16_t wordCount = block.wordCount();
                for (uint16_t i = 0; i < wordCount; ++i) {
                  if (!fullText.empty()) fullText += " ";
                  fullText += block.wordText(i);
                }
              }
            }
          }
          if (!fullText.empty()) {
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                   [this](const ActivityResult& result) {});
            break;
          }
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STAR_PAGE: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        bookmarkStore.toggle(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(section->currentPage));
        requestUpdate();
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STARRED_PAGES: {
      startActivityForResult(
          std::make_unique<StarredPagesActivity>(renderer, mappedInput, bookmarkStore, epub),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& starred = std::get<StarredPageResult>(result.data);
              if (currentSpineIndex != starred.spineIndex || !section || section->currentPage != starred.pageNumber) {
                RenderLock lock(*this);
                currentSpineIndex = starred.spineIndex;
                navTarget = NavigationTarget::makePage(starred.pageNumber);
                section.reset();
              }
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      if (tryAutoPushOnClose()) return;
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::READING_STATS: {
      // Jump to this book's detail screen using the same filename-hash docId
      // the session was opened with. The in-flight session's time isn't
      // visible here — it lands in the store only when end() runs on reader
      // exit. For a brand-new book that's never been finished a session yet
      // the screen will show "no data"; that's accurate.
      if (!epub) break;
      startActivityForResult(std::make_unique<ReadingStatsBookDetailActivity>(
                                 renderer, mappedInput, KOReaderDocumentId::calculateFromFilename(epub->getPath())),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOK_INFO: {
      if (!epub) break;
      startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, epub->getPath()),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::MARK_AS_READ: {
      if (!epub) {
        break;
      }
      const int spineCount = epub->getSpineItemsCount();
      if (spineCount > 0) {
        const int lastSpineIndex = spineCount - 1;
        int lastPageIndex = 0;
        int lastPageCount = 0;
        if (section && currentSpineIndex == lastSpineIndex) {
          lastPageCount = section->pageCount;
          lastPageIndex = std::max(0, section->pageCount - 1);
        }
        if (lastPageCount > 0) {
          writeReaderProgressCache(epub->getCachePath(), lastSpineIndex, lastPageIndex, lastPageCount, 100);
        } else {
          writeReaderProgressCache(epub->getCachePath(), lastSpineIndex, 0, 0, 100);
        }
      }
      BookFinished::launchFinishedBookFlow(*this, renderer, mappedInput, epub->getPath(), epub->getSeries(),
                                           epub->getSeriesIndex(), epub->getAuthor());
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache(true);
          epub->setupCacheDir();
          saveProgress(backupSpine, backupPage, backupPageCount);
          if (!bookmarkStore.isEmpty()) {
            bookmarkStore.markDirty();
            bookmarkStore.save();
            GLOBAL_BOOKMARKS.syncFromStore(bookmarkStore, epub->getPath(), epub->getCachePath(), epub->getTitle(),
                                           false);
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::RENDER_BENCHMARK: {
#if ENABLE_BENCHMARKS
      runRenderBenchmark();
#endif  // ENABLE_BENCHMARKS
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::PULL_REMOTE: {
      // One-tap pull path: run network preconditions and apply remote progress
      // directly instead of showing an intermediate chooser screen.
      if (KOREADER_STORE.hasCredentials()) {
        launchKOReaderSync(SyncLaunchMode::PULL_REMOTE);
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::PUSH_LOCAL: {
      // One-tap push path: run network preconditions and upload local progress
      // directly for KOReader-like "sync now" behavior.
      if (KOREADER_STORE.hasCredentials()) {
        launchKOReaderSync(SyncLaunchMode::PUSH_LOCAL);
      }
      break;
    }
  }
}

void EpubReaderActivity::applyPendingBookmarkJump() {
  auto& jump = APP_STATE.pendingBookmarkJump;
  if (!jump.active || !epub || jump.bookPath != epub->getPath()) {
    return;
  }
  LOG_DBG("ERS", "Applying pending bookmark jump: spine=%u page=%u", jump.spineIndex, jump.pageNumber);
  if (jump.spineIndex >= static_cast<uint16_t>(epub->getSpineItemsCount())) {
    LOG_ERR("ERS", "Invalid bookmark jump spine index %u, resetting to 0", jump.spineIndex);
    jump.spineIndex = 0;
    jump.pageNumber = 0;
  }
  // Seed live state directly; the persistent write is for crash recovery only.
  // saveProgress() on the next render overwrites with the real percent.
  currentSpineIndex = jump.spineIndex;
  navTarget = NavigationTarget::makePage(jump.pageNumber);
  navTarget.cachedSpineIdx = jump.spineIndex;
  if (!writeReaderProgressCache(epub->getCachePath(), jump.spineIndex, jump.pageNumber, 0, 0)) {
    LOG_ERR("ERS", "Failed to persist bookmark jump to progress.bin; live state still seeded");
  }
  jump.clear();
  APP_STATE.saveToFile();
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      navTarget = NavigationTarget::makePage(section->currentPage);
      navTarget.cachedPageCount = section->pageCount;
      navTarget.cachedSpineIdx = currentSpineIndex;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::applyTextDarkness(const uint8_t textDarkness) {
  if (SETTINGS.textDarkness == textDarkness) {
    return;
  }
  SETTINGS.textDarkness = textDarkness;
  SETTINGS.saveToFile();
  renderer.setTextDarkness(textDarkness);
  // Force a re-render so the new darkness is visible immediately.
  requestUpdate();
}

void EpubReaderActivity::stopAutomaticPageTurn() {
  if (!automaticPageTurnActive) {
    return;
  }

  automaticPageTurnActive = false;

  if (UITheme::getStatusBarHeight(true) == UITheme::getStatusBarHeight()) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  RenderLock lock(*this);
  if (section) {
    navTarget = NavigationTarget::makePage(section->currentPage);
    navTarget.cachedPageCount = section->pageCount;
    navTarget.cachedSpineIdx = currentSpineIndex;
  }
  pendingPreRender = false;
  usePreRenderedBuffer = false;
  preRenderedPage.ready = false;
  pendingGrayscale_ = {};
  section.reset();
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_LABELS)) {
    stopAutomaticPageTurn();
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_LABELS[selectedPageTurnOption];
  automaticPageTurnActive = true;

  // Reset cached section when automatic page turn adds a forced status item band.
  if (UITheme::getStatusBarHeight(true) != UITheme::getStatusBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      navTarget = NavigationTarget::makePage(section->currentPage);
      navTarget.cachedPageCount = section->pageCount;
      navTarget.cachedSpineIdx = currentSpineIndex;
    }
    section.reset();
  }
}

void EpubReaderActivity::applyBookReaderOverrides(const int8_t embeddedStyleOverride,
                                                  const int8_t imageRenderingOverride, const int8_t fontFamilyOverride,
                                                  const std::string& sdFontFamilyOverride,
                                                  const int8_t fontSizeOverride, const bool bionicReadingOverride,
                                                  const int8_t paragraphAlignmentOverride) {
  applyBookReaderOverrides(embeddedStyleOverride, imageRenderingOverride, fontFamilyOverride, sdFontFamilyOverride,
                           fontSizeOverride, static_cast<int8_t>(bionicReadingOverride ? 1 : 0),
                           paragraphAlignmentOverride, bookTextAntiAliasingOverride, bookHyphenationOverride,
                           bookGuideDotsOverride, bookInlineFootnotePreviewsOverride);
}

void EpubReaderActivity::applyBookReaderOverrides(
    const int8_t embeddedStyleOverride, const int8_t imageRenderingOverride, const int8_t fontFamilyOverride,
    const std::string& sdFontFamilyOverride, const int8_t fontSizeOverride, const int8_t bionicReadingOverride,
    const int8_t paragraphAlignmentOverride, const int8_t textAntiAliasingOverride, const int8_t hyphenationOverride,
    const int8_t guideDotsOverride, const int8_t inlineFootnotePreviewsOverride) {
  if (!epub) {
    return;
  }

  // Built-in and SD font overrides are mutually exclusive; explicit built-in wins.
  int8_t normalizedFontFamilyOverride = fontFamilyOverride;
  std::string normalizedSdFontFamilyOverride = sdFontFamilyOverride;
  if (normalizedFontFamilyOverride >= 0) {
    normalizedSdFontFamilyOverride.clear();
  } else if (!normalizedSdFontFamilyOverride.empty()) {
    normalizedFontFamilyOverride = -1;
  }

  // Guide dots are excluded from this comparison on purpose: they are render-time
  // only (see TextBlock::setGuideDots), so a guide-dots-only change must not fall
  // through to the section relayout below.
  const bool layoutOverridesUnchanged =
      bookEmbeddedStyleOverride == embeddedStyleOverride && bookImageRenderingOverride == imageRenderingOverride &&
      bookFontFamilyOverride == normalizedFontFamilyOverride &&
      bookSdFontFamilyOverride == normalizedSdFontFamilyOverride && bookFontSizeOverride == fontSizeOverride &&
      bookBionicReadingOverride == bionicReadingOverride &&
      bookParagraphAlignmentOverride == paragraphAlignmentOverride &&
      bookTextAntiAliasingOverride == textAntiAliasingOverride && bookHyphenationOverride == hyphenationOverride &&
      bookInlineFootnotePreviewsOverride == inlineFootnotePreviewsOverride;

  if (layoutOverridesUnchanged && bookGuideDotsOverride == guideDotsOverride) {
    return;
  }

  bookEmbeddedStyleOverride = embeddedStyleOverride;
  bookImageRenderingOverride = imageRenderingOverride;
  bookFontFamilyOverride = normalizedFontFamilyOverride;
  bookSdFontFamilyOverride = normalizedSdFontFamilyOverride;
  bookFontSizeOverride = fontSizeOverride;
  bookBionicReadingOverride = bionicReadingOverride;
  bookParagraphAlignmentOverride = paragraphAlignmentOverride;
  bookTextAntiAliasingOverride = textAntiAliasingOverride;
  bookHyphenationOverride = hyphenationOverride;
  bookGuideDotsOverride = guideDotsOverride;
  bookInlineFootnotePreviewsOverride = inlineFootnotePreviewsOverride;
  RECENT_BOOKS.setReaderOverrides(
      epub->getPath(), bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
      bookSdFontFamilyOverride, bookFontSizeOverride, bookBionicReadingOverride, bookParagraphAlignmentOverride,
      bookTextAntiAliasingOverride, bookHyphenationOverride, bookGuideDotsOverride, bookInlineFootnotePreviewsOverride);

  if (layoutOverridesUnchanged) {
    // Only guide dots changed: persisted above, and the repaint on resume picks
    // the new value up in render(). No section relayout, no refresh override.
    return;
  }

  // A changed override forces a full section relayout (section.reset() below → rebuild with the
  // "Indexing…" popup). That popup FAST-refreshes against whatever is on the panel; when the change
  // arrived via the full-screen selector, the extra menu/submenu/selector redraws leave the FAST
  // baseline out of sync and the popup box ghosts. Arm a one-shot HALF so drawPopup() establishes a
  // clean baseline — the same deliberate-transition signal the chapter/percent/footnote jumps use
  // (hasRefreshOverridePending() at the popup then also arms forceHalfRefreshAfterPopup_ for the
  // first content page). Reached only when something actually changed (early-out above), so routine
  // no-op reopens of the menu don't pay for it.
  ReaderUtils::enforceExitFullRefresh(renderer);

  RenderLock lock(*this);
  if (section) {
    const int currentPage = section->currentPage;
    if (!section->hasActiveBuild()) {
      if (const auto paragraphIndex = section->getParagraphIndexForPage(currentPage)) {
        navTarget = NavigationTarget::makeParagraph(*paragraphIndex, currentPage);
      } else {
        navTarget = NavigationTarget::makePage(currentPage);
      }
      navTarget.cachedPageCount = section->pageCount;
    } else {
      // pageCount is only the number of pages produced so far. Treating it as a
      // completed layout count would proportionally jump forward after relayout.
      navTarget = NavigationTarget::makePage(currentPage);
      LOG_DBG("ERS", "Preserving page %d without rescale during active section build", currentPage);
    }
    navTarget.cachedSpineIdx = currentSpineIndex;
  }
  section.reset();
}

bool EpubReaderActivity::getEffectiveEmbeddedStyle() const {
  if (bookEmbeddedStyleOverride >= 0) {
    return bookEmbeddedStyleOverride != 0;
  }
  return SETTINGS.embeddedStyle != 0;
}

bool EpubReaderActivity::getEffectiveBionicReading() const {
  if (bookBionicReadingOverride >= 0) {
    return bookBionicReadingOverride > 0;
  }
  return SETTINGS.bionicReading;
}

uint8_t EpubReaderActivity::getEffectiveImageRendering() const {
  if (bookImageRenderingOverride >= 0) {
    return static_cast<uint8_t>(bookImageRenderingOverride);
  }
  return SETTINGS.imageRendering;
}

bool EpubReaderActivity::getEffectiveTextAntiAliasing() const {
  if (bookTextAntiAliasingOverride >= 0) {
    return bookTextAntiAliasingOverride != 0;
  }
  return SETTINGS.textAntiAliasing != 0;
}

bool EpubReaderActivity::getEffectiveHyphenation() const {
  if (bookHyphenationOverride >= 0) {
    return bookHyphenationOverride != 0;
  }
  return SETTINGS.hyphenationEnabled != 0;
}

bool EpubReaderActivity::getEffectiveGuideDots() const {
  if (bookGuideDotsOverride >= 0) {
    return bookGuideDotsOverride != 0;
  }
  return SETTINGS.guideDots != 0;
}

bool EpubReaderActivity::getEffectiveInlineFootnotePreviews() const {
  if (bookInlineFootnotePreviewsOverride >= 0) {
    return bookInlineFootnotePreviewsOverride != 0;
  }
  return SETTINGS.inlineFootnotePreviews != 0;
}

uint8_t EpubReaderActivity::getEffectiveParagraphAlignment() const {
  if (bookParagraphAlignmentOverride >= 0) {
    return static_cast<uint8_t>(bookParagraphAlignmentOverride);
  }
  return SETTINGS.paragraphAlignment;
}

float EpubReaderActivity::getEffectiveReaderLineCompression() const {
  const uint8_t fontSize = (bookFontSizeOverride >= 0) ? static_cast<uint8_t>(bookFontSizeOverride) : SETTINGS.fontSize;
  const int effectiveFontId = getEffectiveReaderFontId();
  const int notosansId = CrossPointSettings::getBuiltinReaderFontId(CrossPointSettings::NOTOSANS, fontSize);

  if (effectiveFontId == notosansId) {
    switch (SETTINGS.lineSpacing) {
      case CrossPointSettings::TIGHT:
        return 0.90f;
      case CrossPointSettings::NORMAL:
      default:
        return 0.95f;
      case CrossPointSettings::WIDE:
        return 1.0f;
    }
  }

  switch (SETTINGS.lineSpacing) {
    case CrossPointSettings::TIGHT:
      return 0.95f;
    case CrossPointSettings::NORMAL:
    default:
      return 1.0f;
    case CrossPointSettings::WIDE:
      return 1.1f;
  }
}

int EpubReaderActivity::getEffectiveReaderFontId() const {
  // Per-book font override: when set, force a specific BUILT-IN family even if
  // an SD card font is the global default. This makes the override predictable
  // ("override forces back to a known built-in") and avoids surprising users
  // who set the override before they had any SD fonts.
  const uint8_t fontSize = (bookFontSizeOverride >= 0) ? static_cast<uint8_t>(bookFontSizeOverride) : SETTINGS.fontSize;
  if (bookFontFamilyOverride >= 0) {
    return CrossPointSettings::getBuiltinReaderFontId(static_cast<uint8_t>(bookFontFamilyOverride), fontSize);
  }
  if (!bookSdFontFamilyOverride.empty()) {
    const int id = resolveSdCardFontId(bookSdFontFamilyOverride.c_str(), fontSize);
    if (id != 0) return id;
  }
  // No override: defer to global resolution (which honors SD card font selection).
  // We synthesize a temporary lookup using the override fontSize if it's set; otherwise
  // SETTINGS.getReaderFontId() is the canonical answer.
  if (bookFontSizeOverride >= 0) {
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      const int id = resolveSdCardFontId(SETTINGS.sdFontFamilyName, fontSize);
      if (id != 0) return id;
    }
    return CrossPointSettings::getBuiltinReaderFontId(SETTINGS.fontFamily, fontSize);
  }
  return SETTINGS.getReaderFontId();
}

// Sibling-size ladder for a built-in body font: every size of the same family, with its
// point size expressed as a percent of the body's. Deterministic from the fontId ALONE —
// the section-cache property hash deliberately excludes the ladder on that basis, so any
// path that rebuilds a section (foreground, background, sleep) derives an identical ladder
// from the same fontId. Unknown ids (SD-card fonts, one loaded size) get an empty ladder,
// which keeps the pure-scale fallback.
//
// Glyph-cache note: an earlier taller-heading-font attempt thrashed the FontDecompressor's
// four page slots. Two things changed since: FontCacheManager now prewarms per fontId, and
// the parser caps sections at ONE auxiliary font (body R/B/I + aux R = exactly four slots).
static FontSizeLadder buildReaderFontSizeLadder(const int bodyFontId) {
  static constexpr uint8_t kSizeEnums[] = {CrossPointSettings::TINY, CrossPointSettings::SMALL,
                                           CrossPointSettings::MEDIUM, CrossPointSettings::LARGE,
                                           CrossPointSettings::EXTRA_LARGE};
  static constexpr uint8_t kPointSizes[] = {10, 12, 14, 16, 18};
  static constexpr uint8_t kFamilies[] = {CrossPointSettings::BOOKERLY, CrossPointSettings::NOTOSANS};

  FontSizeLadder ladder;
  for (const uint8_t family : kFamilies) {
    for (size_t i = 0; i < sizeof(kSizeEnums); ++i) {
      if (CrossPointSettings::getBuiltinReaderFontId(family, kSizeEnums[i]) != bodyFontId) continue;
      const uint8_t bodyPt = kPointSizes[i];
      for (size_t j = 0; j < sizeof(kSizeEnums); ++j) {
        ladder.addRung(CrossPointSettings::getBuiltinReaderFontId(family, kSizeEnums[j]),
                       static_cast<uint16_t>(kPointSizes[j] * 100 / bodyPt));
      }
      return ladder;
    }
  }
  return ladder;  // SD font or unknown id: empty ladder = scale-only fallback
}

void EpubReaderActivity::NavigationTarget::resolveInto(Section& sec, int spineIndex) const {
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

bool EpubReaderActivity::stepPageState(const bool isForwardTurn) {
  if (!epub || !section) {
    return false;
  }

  // Background-C in progress: the final page count isn't known yet, so navigate optimistically.
  // Only an explicit Page target has a meaningful position before completion (other targets are
  // resolved when the build finishes); for those, swallow the turn. Forward advances the display
  // cursor (the SectionBuilding pass shows the page once C builds it, or the popup until then);
  // running past the real end is reconciled at completion (clamp / cross to the next spine). Back
  // past page 0 leaves the chapter, aborting the in-flight build via ~Section.
  if (section->hasActiveBuild()) {
    if (navTarget.kind != NavigationTarget::Kind::Page) {
      return false;
    }
    // The lock must cover the GUARDS, not just the mutation: the PreRender pass temporarily
    // sets section->currentPage to the page it is laying out, so a guard evaluated unlocked
    // can pass on that transient value and the mutation then lands on the restored one —
    // observed on-device as a back turn at page 0 reading a transient 1, then decrementing
    // the restored 0 to -1 (a visible "out of bounds" frame).
    RenderLock lock(*this);
    if (isForwardTurn) {
      section->currentPage++;
    } else if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      navTarget = NavigationTarget::makeLastPage();
      currentSpineIndex--;
      section.reset();
    } else {
      return false;
    }
    // Persist mid-build turns too: the SectionBuilding pass never arms pendingProgressSave, so
    // without this a sleep/power-off during a long build resumes at the build-entry page (issue
    // #75). pageCount 0 = "unknown until completion" — the loader skips rescaling for it and the
    // first post-build render overwrites with the real count. Skipped when the back-cross branch
    // reset the section (position resolves when the previous spine loads).
    if (section) {
      pendingProgressSave.spineIndex = currentSpineIndex;
      pendingProgressSave.page = section->currentPage;
      pendingProgressSave.pageCount = 0;
      pendingProgressSave.pending.store(true, std::memory_order_release);
    }
    lastPageTurnTime = millis();
    forceLoadLargeImages = false;
    pageHasPlaceholders = false;
    return true;
  }

  // Serialize the WHOLE step decision against the render task, guards included: the
  // PreRender pass temporarily writes section->currentPage while laying out the next page,
  // so a guard evaluated outside the lock can pass on that transient value and the mutation
  // then lands on the restored one. Observed on-device: a back turn at page 0 read the
  // pre-render's transient 1, blocked on the lock, then decremented the restored 0 to -1 —
  // a visible "out of bounds" frame. The forward mirror can double-advance past the end.
  RenderLock lock(*this);

  // A 0-page section (permanently unparse-able chapter) has no within-chapter navigation,
  // but the user must still be able to cross spine boundaries to escape it.
  const bool hasPages = section->pageCount > 0;

  // NOTE: section changes served from cache or a completed Background-B build never release
  // the secondary buffer, so RED RAM baseline is intact and the first page uses a normal fast refresh.
  // When the secondary buffer IS released+reallocated (indexing path, image-warm pass, OOM recovery),
  // the release site immediately calls syncRedRamFromFrameBuffer() to restore the correct baseline.
  if (isForwardTurn) {
    if (hasPages && section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      navTarget = NavigationTarget::makePage(0);
      currentSpineIndex++;
      section.reset();
    } else if (currentSpineIndex + 1 == epub->getSpineItemsCount()) {
      navTarget = NavigationTarget::makeLastPage();
      currentSpineIndex++;
      section.reset();
    } else {
      return false;
    }
  } else {
    if (hasPages && section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      navTarget = NavigationTarget::makeLastPage();
      currentSpineIndex--;
      section.reset();
    } else {
      return false;
    }
  }

  lastPageTurnTime = millis();
  forceLoadLargeImages = false;
  pageHasPlaceholders = false;
  return true;
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  // Cancel any pending deferred AA pass — it belongs to the page we're leaving.
  pendingGrayscale_ = {};

  // If the "Indexing..." popup is currently on screen and the user turns the page/section now, the
  // destination page replaces a dark popup box on the X4 baseline. Whether it lands via a now-built
  // page (displayBuildPage) or by abandoning the build to an adjacent cached section (renderContents
  // Normal pass), a FAST diff against the popup frame leaves a ghost outline. Arm the post-popup HALF
  // so the replacing page establishes a clean baseline. This is a deliberate navigation away from the
  // popup — NOT the routine forward-reading crossing the cold-open/deliberate-jump gating protects —
  // so it doesn't reintroduce the "every section traversal pays a slow refresh" cost. X3's fast
  // differential reads the controller's DTM1 (drawPopup updated it correctly), so it never ghosts.
  if (!renderer.isX3() && buildingPopupShown_) {
    forceHalfRefreshAfterPopup_ = true;
  }

  auto logPageTurnWindowIfReady = [this]() {
    if (pageTurnStatsWindow.turns < PAGE_TURN_STATS_WINDOW_SIZE) {
      return;
    }
    const unsigned long hitRatePct =
        (static_cast<unsigned long>(pageTurnStatsWindow.preRenderHits) * 100UL) / pageTurnStatsWindow.turns;
    const unsigned long avgPreRenderMs =
        pageTurnStatsWindow.preRenderHits > 0
            ? (pageTurnStatsWindow.totalPreRenderMs / pageTurnStatsWindow.preRenderHits)
            : 0UL;
    const unsigned long avgIdleSlackMs =
        pageTurnStatsWindow.preRenderHits > 0
            ? (pageTurnStatsWindow.totalIdleSlackMs / pageTurnStatsWindow.preRenderHits)
            : 0UL;
    const uint16_t preRenderMisses = pageTurnStatsWindow.preRenderMisses;
    LOG_DBG("ERS",
            "PageTurn agg(%u): turns=%u prerenderHits=%u prerenderMisses=%u hitRatePct=%lu avgPreRenderMs=%lu "
            "avgIdleSlackMs=%lu",
            PAGE_TURN_STATS_WINDOW_SIZE, pageTurnStatsWindow.turns, pageTurnStatsWindow.preRenderHits, preRenderMisses,
            hitRatePct, avgPreRenderMs, avgIdleSlackMs);
    pageTurnStatsWindow = {};
  };

  const bool hadPreRenderedCandidate =
      isForwardTurn && section && preRenderedPage.ready && preRenderedPage.spineIndex == currentSpineIndex;
  const int expectedNextPage = (section ? section->currentPage + 1 : -1);

  if (isForwardTurn && section && preRenderedPage.ready && preRenderedPage.spineIndex == currentSpineIndex &&
      preRenderedPage.pageIndex == section->currentPage + 1) {
    // Fast path: the frame buffer already holds the next page content. Advance state here on the
    // loop task, then hand off to render() via usePreRenderedBuffer — all display work (status
    // bar, flush, AA pass) stays on the render task where it belongs.
    //
    // Serialize the shared-state mutation against the render task: the PreRender pass temporarily
    // writes section->currentPage and rewrites preRenderedPage, so reading/advancing them here
    // without the lock races (stale buffer shown, or a torn section pointer → reboot). Acquire the
    // lock, then re-check the condition under it — the render task may have invalidated the
    // pre-render between the unlocked test above and the lock.
    RenderLock lock;
    // cppcheck-suppress knownConditionTrueFalse ; render task mutates these concurrently
    if (!(section && preRenderedPage.ready && preRenderedPage.spineIndex == currentSpineIndex &&
          preRenderedPage.pageIndex == section->currentPage + 1)) {
      lock.unlock();
      if (!stepPageState(isForwardTurn)) {
        return;
      }
      sessionPagesAdvanced++;
      globalReadingSessionTracker().onPageTurn();
      preRenderedPage.ready = false;
      pendingPreRender = false;
      requestUpdate();
      return;
    }
    const unsigned long nowMs = millis();
    const unsigned long idleSlackMs = (preRenderedPage.completedAtMs > 0 && nowMs >= preRenderedPage.completedAtMs)
                                          ? (nowMs - preRenderedPage.completedAtMs)
                                          : 0UL;
    LOG_DBG("ERS", "PageTurn stats: prerendered=1 preRenderMs=%lu idleSlackMs=%lu", preRenderedPage.renderDurationMs,
            idleSlackMs);
    pageTurnStatsWindow.turns++;
    pageTurnStatsWindow.preRenderHits++;
    pageTurnStatsWindow.totalPreRenderMs += preRenderedPage.renderDurationMs;
    pageTurnStatsWindow.totalIdleSlackMs += idleSlackMs;
    LOG_DBG("ERS", "PageTurn summary: hit=1 window=%u/%u nextPage=%d", pageTurnStatsWindow.preRenderHits,
            pageTurnStatsWindow.turns, preRenderedPage.pageIndex);
    logPageTurnWindowIfReady();
    section->currentPage = preRenderedPage.pageIndex;
    preRenderedPage.ready = false;
    usePreRenderedBuffer = true;
    sessionPagesAdvanced++;
    globalReadingSessionTracker().onPageTurn();
    lastPageTurnTime = millis();
    requestUpdate();
    return;
  }

  if (!stepPageState(isForwardTurn)) {
    return;
  }

  LOG_DBG("ERS", "PageTurn stats: prerendered=0 candidate=%d expectedNext=%d cachedNext=%d pendingPreRender=%d",
          hadPreRenderedCandidate ? 1 : 0, expectedNextPage, preRenderedPage.pageIndex, pendingPreRender ? 1 : 0);
  pageTurnStatsWindow.turns++;
  pageTurnStatsWindow.preRenderMisses++;
  LOG_DBG("ERS", "PageTurn summary: hit=0 window=%u/%u expectedNext=%d", pageTurnStatsWindow.preRenderHits,
          pageTurnStatsWindow.turns, expectedNextPage);
  logPageTurnWindowIfReady();

  sessionPagesAdvanced++;
  globalReadingSessionTracker().onPageTurn();
  // Page state advanced without using a pre-render. Drop any pre-render that was
  // scheduled for the page we just left: otherwise the coalesced render() would
  // classify as a PreRender pass and try to pre-render the *new* current page's
  // next page instead of displaying the page we just navigated to — leaving the
  // previous (now stale) frame on screen. (This is the classic last-page case:
  // turning onto the final page would otherwise show the penultimate page.)
  preRenderedPage.ready = false;
  pendingPreRender = false;
  requestUpdate();
}

bool EpubReaderActivity::reallocSecondaryEvictingCaches() {
  // The FDC page slots are per-page state (every prewarmed render batch-clears and refills
  // them via endScanAndPrewarm), but a render done mid-released-build leaves the last page's
  // ~4 KB slot buffers allocated — frequently inside the released-framebuffer hole this
  // realloc is about to ask back as one contiguous block. Dropping them first is free (the
  // next render re-prewarms regardless) and deterministic, so do it before the first attempt.
  if (FontCacheManager* fontCache = renderer.getFontCacheManager()) {
    fontCache->clearCache();
  }
  if (renderer.reallocSecondaryBuffer()) {
    return true;
  }
  // Still blocked: evict the CSS resolve caches (hot/negative caches, container bucket
  // arrays and the retained selector index all reload lazily from SD, worst case ~240 ms
  // at the next section-build start) and drop Background-B's section — an in-flight B
  // build holds a parser plus its inflate ring, and even a settled one keeps LUT/TOC
  // state; B re-probes on its own cadence, so the only cost is redoing lookahead work.
  // All far cheaper than the recovery reboot this call stands between. Retry once.
  if (epub && epub->getCssParser()) {
    epub->getCssParser()->clearCaches(/*evictEverything=*/true);
  }
  if (backgroundSection_) {
    LOG_INF("ERS", "Dropping Background-B section (spine=%d) for secondary realloc", backgroundBuildSpineIndex_);
    resetBackgroundBuild();
  }
  if (renderer.reallocSecondaryBuffer()) {
    LOG_INF("ERS", "Secondary realloc succeeded after cache eviction");
    return true;
  }
  return false;
}

bool EpubReaderActivity::ensureFootnotePreviewCache() {
  if (!epub || !getEffectiveInlineFootnotePreviews()) {
    return true;
  }
  if (footnotePreviewCacheReady_) {
    return true;
  }
  if (FootnotePreviews::cacheExists(epub->getCachePath())) {
    footnotePreviewCacheReady_ = true;
    return true;
  }
  // One-time whole-book scan (foreground by design — no background variant to keep the
  // build paths simple). An empty result still writes a valid cache, so this runs once
  // per book, not once per open.
  GUI.drawPopup(renderer, tr(STR_GATHERING_FOOTNOTES));
  footnotePreviewCacheReady_ = FootnotePreviews::gather(*epub);
  if (!footnotePreviewCacheReady_) {
    LOG_ERR("ERS", "Footnote preview gather failed; building without previews");
  }
  return footnotePreviewCacheReady_;
}

void EpubReaderActivity::recoverSecondaryBufferIfNeeded() {
  // While Background-C is building with the buffer released for headroom, leave it released —
  // reallocating now would reclaim the ~48–52 KB the build is using. The buffer is restored here
  // on the first render after the build ends (hasActiveBuild() goes false).
  if (section && section->hasActiveBuild()) {
    return;
  }
  // Borrowed-buffer path: the build ran inside the LENT secondary framebuffer, so return it
  // instead of reallocating. The region never entered the heap, so returnSecondaryBuffer()
  // cannot fail — no realloc/eviction/forensics/heap-recovery needed here. Drop the section's
  // reference to the scratch arena first, then free the (small, non-owning) arena object, then
  // hand the block back to the display.
  if (secondaryBorrowed_ && !renderer.hasSecondaryBuffer()) {
    if (section) section->setExternalBuildScratch(nullptr);
    buildScratch_.reset();
    renderer.returnSecondaryBuffer();
    secondaryBorrowed_ = false;
    secondaryBufferDegraded_ = false;
    renderer.setSingleBufferFastDiff(false);
    LOG_INF("ERS", "Secondary display buffer returned (borrow); re-enabling normal refresh/AA paths");
    return;
  }
  // Opportunistic recovery: after an OOM during chapter indexing, or after a Background-C
  // released build, restore the secondary buffer when heap is healthy again.
  if (secondaryBufferDegraded_ && !renderer.hasSecondaryBuffer()) {
    if (reallocSecondaryEvictingCaches()) {
      secondaryBufferDegraded_ = false;
      // Undo the IncrementalReleased opt-in (see chooseSectionBuildMode/buildSection): once the
      // secondary buffer is back, double-buffer fast-diff is correct again and this flag would
      // otherwise leave RED RAM reseeding skipped on the normal path. No-op if it was never set
      // (e.g. recovering from an indexing OOM instead of a released build).
      renderer.setSingleBufferFastDiff(false);
      // Do NOT syncRedRamFromFrameBuffer() here: reallocSecondaryBuffer() whitened the new secondary,
      // and syncRedRamFromFrameBuffer() would copy that white into RED RAM, destroying the baseline.
      // RED already holds the last displayed page (kept current by the released build's FAST refreshes;
      // the controller retains it through realloc). Reseeding from white ghosted the next page.
      LOG_INF("ERS", "Secondary display buffer restored; re-enabling normal refresh/AA paths");
    } else {
      const uint32_t freeHeap = esp_get_free_heap_size();
      // Safe to walk the heap here (unlike the post-index OOM path): this is a routine
      // render-start recovery, not the aftermath of decode failures under pressure.
      const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
      LOG_ERR("ERS", "Secondary display buffer realloc failed (free=%lu contig=%lu); AA stays off, will retry",
              freeHeap, contigHeap);
      // One-shot forensic dump so field logs identify WHAT is pinning the released hole
      // (address + size of every block). Once per boot: the block map barely changes
      // between failed retries and the dump is hundreds of serial lines.
      static bool dumpedHeapOnce = false;
      if (!dumpedHeapOnce) {
        dumpedHeapOnce = true;
        LOG_ERR("ERS", "Heap block dump (one-shot, pin forensics):");
        heap_caps_dump(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
      }
      // Cache eviction plus opportunistic retries did not recover a framebuffer-sized
      // contiguous block, so escalate to a recovery reboot once free heap is plentiful
      // but the block still can't be found.
      maybeRestartForFragmentedHeap(freeHeap, contigHeap);
    }
  } else if (secondaryBufferDegraded_ && renderer.hasSecondaryBuffer()) {
    secondaryBufferDegraded_ = false;
    renderer.setSingleBufferFastDiff(false);
  }
}

void EpubReaderActivity::clampSpineIndex(const int spineCount) {
  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen (spineCount is the finished-book sentinel)
  if (currentSpineIndex > spineCount) {
    currentSpineIndex = spineCount;
  }
}

EpubReaderActivity::RenderLayout EpubReaderActivity::computeRenderLayout() const {
  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const int statusBarTopHeight = UITheme::getStatusBarTopHeight(automaticPageTurnActive);
  const int statusBarBottomHeight = UITheme::getStatusBarBottomHeight(automaticPageTurnActive);

  orientedMarginTop += std::max(static_cast<int>(SETTINGS.screenMargin), statusBarTopHeight);
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom += std::max(static_cast<int>(SETTINGS.screenMargin), statusBarBottomHeight);

  RenderLayout layout;
  layout.marginTop = orientedMarginTop;
  layout.marginRight = orientedMarginRight;
  layout.marginBottom = orientedMarginBottom;
  layout.marginLeft = orientedMarginLeft;
  layout.viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  layout.viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  return layout;
}

EpubReaderActivity::RenderPass EpubReaderActivity::classifyRenderPass() const {
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    return RenderPass::FinishedBook;
  }
  // Background-C owns the screen while the current section is being built: draw the requested
  // page from the in-progress LUT, or the indexing popup. Checked ahead of the pre-render /
  // buffer-display passes (which never arm during a build) so a build can never be pre-empted.
  if (section && section->hasActiveBuild()) {
    return RenderPass::SectionBuilding;
  }
  // BufferDisplay is checked before PreRender to preserve the former in-line order
  // (the buffer-display block ran ahead of the pre-render block). It applies only when
  // the prior waveform + post-waveform SPI work has settled; otherwise we fall through
  // to a full render that waits for the waveform naturally. The helper may still bail to
  // Normal at runtime if the page load fails / is an image page.
  if (usePreRenderedBuffer && !renderer.isRefreshPending()) {
    return RenderPass::BufferDisplay;
  }
  if (pendingPreRender) {
    return RenderPass::PreRender;
  }
  if (!section) {
    return RenderPass::BuildSection;
  }
  return RenderPass::Normal;
}

void EpubReaderActivity::renderFinishedBookPass(RenderLock& lock, const int spineCount) {
  // Immediately transition to finished-book flow instead of showing an end-of-book screen
  if (finishedBookActivityStarted_) {
    return;
  }
  finishedBookActivityStarted_ = true;
  const int lastSpineIndex = std::max(0, spineCount - 1);
  writeReaderProgressCache(epub->getCachePath(), lastSpineIndex, 0, 0, 100);
  lock.unlock();
  BookFinished::launchFinishedBookFlow(
      *this, renderer, mappedInput, epub->getPath(), epub->getSeries(), epub->getSeriesIndex(), epub->getAuthor(),
      [](void* ctx) { static_cast<EpubReaderActivity*>(ctx)->finishedBookActivityStarted_ = false; }, this);
}

bool EpubReaderActivity::renderBufferDisplayPass(const RenderLayout& layout) {
  // Fast-display pass: frame buffer holds pre-rendered content; superimpose live status bar,
  // flush to display, and run the AA pass — all on the render task with no SD font re-read.
  if (!section) {
    return false;
  }
  auto p = section->loadPageFromSectionFile();
  if (!p || p->hasImages()) {
    // Page load failed or was an image page — caller falls through to full render.
    return false;
  }
  currentPageFootnotes = std::move(p->footnotes);
  displayPreRenderedPage(*p, layout.marginTop, layout.marginRight, layout.marginBottom, layout.marginLeft);

  pendingProgressSave.spineIndex = currentSpineIndex;
  pendingProgressSave.page = section->currentPage;
  pendingProgressSave.pageCount = section->pageCount;
  pendingProgressSave.pending.store(true, std::memory_order_release);

  if (section->currentPage + 1 < section->pageCount) {
    pendingPreRender = true;
    requestUpdate();
  }
  LOG_DBG("ERS", "Page summary: spine=%d page=%d/%d prerendered=1 refresh=%s mode=0x%02X", currentSpineIndex,
          section->currentPage, section->pageCount, refreshModeName(lastPageRefreshMode_), lastPageDisplayModeByte_);
  return true;
}

void EpubReaderActivity::renderPreRenderPass(const RenderLayout& layout) {
  // Pre-render pass: render next page content into the frame buffer (no status bar, no flush).
  if (!section || preRenderedPage.ready) {
    return;
  }
  const int nextPage = section->currentPage + 1;
  // During an active build use the in-memory LUT via loadPageFromActiveBuild; the on-disk
  // LUT is not written until finalisation so loadPageFromSectionFile would always miss.
  const bool buildActive = section->hasActiveBuild();
  const int availablePages =
      buildActive ? static_cast<int>(section->activeBuildPageCount()) : static_cast<int>(section->pageCount);
  if (nextPage >= availablePages) {
    return;
  }
  if (esp_get_free_heap_size() < PRE_RENDER_MIN_FREE_HEAP_BYTES) {
    return;
  }
#if DEBUG_BACKGROUND_WORK
  bgCounters_.aRuns++;
#endif
  const int savedPage = section->currentPage;
  section->currentPage = nextPage;
  auto p = buildActive ? section->loadPageFromActiveBuild(static_cast<uint16_t>(nextPage))
                       : section->loadPageFromSectionFile();
  section->currentPage = savedPage;
  if (p && !p->hasImages()) {
    const unsigned long preRenderStart = millis();
    section->currentPage = nextPage;
    renderPageContentOnly(*p, layout.marginTop, layout.marginRight, layout.marginBottom, layout.marginLeft);
    section->currentPage = savedPage;
    const unsigned long preRenderDuration = millis() - preRenderStart;
    preRenderedPage = {true, currentSpineIndex, nextPage, preRenderDuration, millis()};
#if DEBUG_BACKGROUND_WORK
    bgCounters_.aCompletes++;
#endif
    LOG_DBG("ERS", "Pre-rendered page %d/%d in %lums", nextPage, section->pageCount - 1, preRenderDuration);
  }
  checkHeapIntegrity("after_prerender");
}

bool EpubReaderActivity::heapAllowsInPlaceBuild(const bool embeddedStyle, const size_t inflatedSize) const {
  // Mirror the gate Background-B uses to build with the secondary buffer resident: if a CSS
  // book's selector index won't fit alongside the buffer, don't even attempt the in-place
  // build. Then require a free/contig floor pinned above B's idle gate — the foreground build
  // runs at page-turn time with the secondary buffer (and possibly a pre-rendered page) live.
  if (embeddedStyle) {
    const CssParser* css = epub->getCssParser();
    if (!Section::heapAllowsEmbeddedStyle(css ? css->ruleCount() : 0)) {
      return false;
    }
  }
  // The extraction phase holds an inflate ring sized to the entry (≤32 KB) — a per-spine cost the
  // static floors were never tuned for (they fit the common few-KB chapter, where the ring is
  // noise). Add it to the floors the same way Background-B's WaitHeap gate does (ring on top of
  // the base, contig raised to ring + scratch), so a whole-book-in-one-spine entry is sent
  // straight to the released path instead of starting a resident build that is doomed to the
  // low-heap abort (~1.4 s of popup/setup/abort/re-setup waste, observed heap dip to <8 KB free).
  const uint32_t ringBytes = static_cast<uint32_t>(std::min<size_t>(32768, inflatedSize));
  const uint32_t freeFloor =
      (embeddedStyle ? IN_PLACE_BUILD_CSS_MIN_FREE_HEAP_BYTES : IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES) + ringBytes;
  const uint32_t contigFloor = std::max<uint32_t>(
      embeddedStyle ? IN_PLACE_BUILD_CSS_MIN_CONTIG_HEAP_BYTES : IN_PLACE_BUILD_MIN_CONTIG_HEAP_BYTES,
      ringBytes + 8 * 1024);
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  const bool ok = freeHeap >= freeFloor && contigHeap >= contigFloor;
  // One line per gate evaluation: the resident/released choice and the exact heap-vs-floor
  // arithmetic behind it. This is the first thing to check when a section build takes the wrong
  // path (a resident build starving on the inflate ring cascades into the released rebuild).
  LOG_INF("ERS", "heapAllowsInPlaceBuild=%d css=%d ring=%lu free=%lu(floor=%lu) contig=%lu(floor=%lu)", ok ? 1 : 0,
          embeddedStyle ? 1 : 0, static_cast<unsigned long>(ringBytes), static_cast<unsigned long>(freeHeap),
          static_cast<unsigned long>(freeFloor), static_cast<unsigned long>(contigHeap),
          static_cast<unsigned long>(contigFloor));
  return ok;
}

EpubReaderActivity::SectionBuildMode EpubReaderActivity::chooseSectionBuildMode(const bool embeddedStyle,
                                                                                const size_t inflatedSize) const {
  // A failed Background-C attempt latches the old blocking path for this spine.
  if (forceBlockingBuildSpine_ == currentSpineIndex) return SectionBuildMode::Blocking;
  // No secondary buffer to keep or release (already degraded): blocking path reallocs it at the end.
  if (!renderer.hasSecondaryBuffer()) return SectionBuildMode::Blocking;
  // A resident build that aborted on the low-heap guard retries released but still INCREMENTAL:
  // the heapAllowsInPlaceBuild floors below already proved optimistic for this spine (they gate on
  // heap at entry, not on what a big spine's ring + parser actually consume), so don't consult
  // them again — and don't collapse to Blocking, which would index the whole section before the
  // first page appears. Checked after the buffer guard: with the buffer already gone the released
  // headroom exists anyway and the blocking path handles the realloc at the end.
  if (forceReleasedBuildSpine_ == currentSpineIndex) return SectionBuildMode::IncrementalReleased;

  // X3 → always build released. Its differential baseline lives in the controller's DTM1, so
  // keeping the ~52 KB RAM buffer resident buys no display benefit (fast refresh works without it)
  // and only starves the build — exactly the foreground policy (inPlace is X4-only). Releasing
  // also keeps CSS parses above the runtime resolve floor (the resident build css-degraded
  // on-device, then had to be rebuilt blocking). Mid-build BW draws still work off DTM1.
  if (renderer.isX3()) return SectionBuildMode::IncrementalReleased;

  // X4 → keep the secondary buffer resident when the in-place floors fit (fast-refresh baseline
  // re-seeds from it, AA stays live during the build); otherwise release for headroom. This now
  // applies to CSS books too: every build is two-phase, so the inflate ring is released BEFORE the
  // CSS-resolving parse, keeping the resolve clear of the ~40 KB floor when the (higher) CSS
  // in-place floor is met. A resident CSS build that still degrades is caught by
  // isCssLowHeapDegraded() and rebuilt with the buffer released — so this gates "try in place"
  // rather than guaranteeing it. (Historically CSS books always released, from before the build
  // was two-phase, when the ring + parser + resolver were all live at once.)
  return heapAllowsInPlaceBuild(embeddedStyle, inflatedSize) ? SectionBuildMode::IncrementalResident
                                                             : SectionBuildMode::IncrementalReleased;
}

EpubReaderActivity::BuildOutcome EpubReaderActivity::compileSectionCache(const RenderLayout& layout,
                                                                         const bool embeddedStyle,
                                                                         const uint8_t imageRendering) {
  // Use a cleaner waveform for the indexing popup right after image pages; a FAST popup refresh
  // can leave visible bleed on X3 transitioning image -> text.
  if (pendingHalfRefreshAfterImagePage && SETTINGS.halfRefreshAfterImagePage) {
    renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
    pendingHalfRefreshAfterImagePage = false;
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  }
  // Draw the popup BEFORE any secondary-buffer release: drawPopup() → displayBuffer() →
  // swapBuffers(), and with the buffer released frameBufferActive is null, so swapBuffers()
  // would set frameBuffer = null and crash subsequent rendering. Drawing it here also makes the
  // popup the fast-refresh baseline on the in-place path — the panel shows the popup, and the
  // first page diffs popup → page cleanly with no ghosting.
  GUI.drawPopup(renderer, tr(STR_INDEXING));

  // Reset cumulative SD font metadata cache so this section starts fresh.
  renderer.clearFontAccumulation();
  readerPhase_ = ReaderPhase::PRECOMPILING;
  renderer.dropFontMetadata();

  // Build params from the current reader state. embeddedStyle and imageRendering come from
  // the caller (may differ from lastRenderStats on the CSS-retry path); viewport comes from
  // the layout already resolved by the caller.
  Section::BuildParams buildParams = makeSectionBuildParams();
  buildParams.embeddedStyle = embeddedStyle;
  buildParams.imageRendering = imageRendering;
  buildParams.viewportWidth = layout.viewportWidth;
  buildParams.viewportHeight = layout.viewportHeight;

  // Prefer to build WITHOUT releasing the secondary buffer when heap is ample, so the chapter's
  // first page keeps a valid fast-refresh baseline. The in-place attempt defers image decode to
  // the lazy per-page path, so a failure here is a graceful parser abort (not a corruption-prone
  // decode under pressure). On X3 we always release: its baseline lives in the controller, so
  // keeping the RAM buffer buys no display benefit, only less headroom.
  bool released = false;
  // Honour the Background-C failure latch: its whole point is retrying with the buffer RELEASED
  // (~52 KB more headroom). Re-consulting heapAllowsInPlaceBuild here would happily go resident
  // again — heap recovers between the abort and this retry — and repeat exactly the starvation
  // that failed (observed on-device: low-heap abort -> "blocking" retry rebuilt in place and
  // ground through the whole spine at <8 KB min free).
  size_t inflatedSize = 0;
  epub->getSpineItemInflatedSize(currentSpineIndex, &inflatedSize);
  const bool inPlace = !renderer.isX3() && renderer.hasSecondaryBuffer() &&
                       forceBlockingBuildSpine_ != currentSpineIndex &&
                       heapAllowsInPlaceBuild(embeddedStyle, inflatedSize);
  if (inPlace) {
    LOG_INF("ERS", "Building section in place (secondary buffer kept): free=%lu contig=%lu", esp_get_free_heap_size(),
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
  } else {
    LOG_INF("ERS", "Index start mem (before fb release): free=%lu", esp_get_free_heap_size());
    renderer.releaseSecondaryBuffer();  // frees ~52 KB for CSS parser + image decoder
    released = true;
    LOG_INF("ERS", "Index start mem (after fb release): free=%lu", esp_get_free_heap_size());
  }

  // Blocking build (the fallback path: X3, tight heap, a failed Background-C attempt, or a
  // CSS-fallback rebuild). Background-C owns the responsive, build-while-you-read case; here we
  // just build to completion. A resumed partial build continues via the same stepSectionBuild
  // state inside createSectionFile.
  const auto runParse = [&]() {
    return section->createSectionFile(
        buildParams.fontId, buildParams.lineCompression, buildParams.extraParagraphSpacing,
        buildParams.paragraphAlignment, buildParams.viewportWidth, buildParams.viewportHeight,
        buildParams.hyphenationEnabled, buildParams.embeddedStyle, buildParams.bionicReadingEnabled,
        buildParams.inlineFootnotePreviews, buildParams.imageRendering, nullptr,
        /*skipEviction=*/false, buildParams.fontSizeLadder);
  };

  const auto runCreate = [&]() -> bool { return runParse(); };

  const uint32_t createStart = millis();
  bool createOk = runCreate();
  LOG_INF("ERS", "createSectionFile returned %d in %ums (free=%lu)", createOk ? 1 : 0, millis() - createStart,
          esp_get_free_heap_size());
  checkHeapIntegrity("after_createSectionFile");

  if (!createOk && inPlace) {
    // The conservative in-place gate was too optimistic. createSectionFile already reset its
    // build state on failure, so the retry starts clean; free the buffer for the headroom the
    // blocking foreground path has always relied on.
    LOG_INF("ERS", "In-place section build failed; retrying with secondary buffer released (free=%lu)",
            esp_get_free_heap_size());
    renderer.releaseSecondaryBuffer();
    released = true;
    const uint32_t retryStart = millis();
    createOk = runCreate();
    LOG_INF("ERS", "createSectionFile retry returned %d in %ums (free=%lu)", createOk ? 1 : 0, millis() - retryStart,
            esp_get_free_heap_size());
    checkHeapIntegrity("after_createSectionFile_retry");
  }

  // Eager image pre-decode only on the released path (it needs the freed headroom).
  // warmAllImageCaches writes pixels into the framebuffer as a side effect; clearScreen()
  // follows. In-place builds skip this — images decode lazily at first render, where
  // renderContents releases + reallocs the buffer per image page on demand.
  if (createOk && released) {
    const bool indexForceLoad = forceLoadLargeImages || !SETTINGS.largeImagePlaceholder;
    const uint32_t warmStart = millis();
    section->warmAllImageCaches(0, 0, indexForceLoad, /*monochromeOutput=*/true);
    LOG_INF("ERS", "warmAllImageCaches done in %ums (free=%lu)", millis() - warmStart, esp_get_free_heap_size());
    renderer.clearScreen();
    checkHeapIntegrity("after_warmAllImageCaches");
  }

  // Restore the secondary buffer only if we released it. The realloc gives a white baseline that
  // no longer matches the panel, so arm a one-shot half-refresh (X4 only). The in-place path
  // leaves the buffer — and the baseline — untouched, so the first page uses a normal fast
  // refresh.
  const BuildOutcome outcome = createOk ? BuildOutcome::Built : BuildOutcome::Failed;
  if (released) {
    if (!reallocSecondaryEvictingCaches()) {
      LOG_ERR("ERS", "Failed to reallocate secondary display buffer — display quality degraded");
      secondaryBufferDegraded_ = true;
      const uint32_t freeAfterIndex = esp_get_free_heap_size();
      // Do NOT call heap_caps_get_largest_free_block here: the heap may be corrupt after image
      // decode failures under pressure, and walking the TLSF free-block list on a corrupt heap
      // causes an interrupt WDT crash. Pass 0 for contigHeap — the restart heuristic treats 0 as
      // "contiguous block definitely too small", which is correct: malloc for ~52 KB just failed.
      LOG_ERR("ERS", "Heap after index: free=%lu", freeAfterIndex);
      if (maybeRestartForFragmentedHeap(freeAfterIndex, 0)) {
        return BuildOutcome::Restarting;
      }
    } else {
      secondaryBufferDegraded_ = false;
      // Symmetric with recoverSecondaryBufferIfNeeded(): a prior IncrementalReleased build may have
      // left single-buffer fast-diff opted in, and a failed opportunistic realloc could route the
      // re-render here instead. Clear it now that the double buffer is back so the next FAST refresh
      // uses the normal host-reseeded baseline, not a stale controller-retained one. No-op if it was
      // never set.
      renderer.setSingleBufferFastDiff(false);
      // Do NOT syncRedRamFromFrameBuffer() here: reallocSecondaryBuffer() whitened the new secondary,
      // and syncRedRamFromFrameBuffer() would copy that white into RED RAM, destroying the baseline.
      // RED already holds the frame displayed before the build (the popup); reseeding from white made
      // the first page after the build diff against white and ghost.
      LOG_DBG("ERS", "Index end mem (after fb realloc): free=%lu", esp_get_free_heap_size());
    }
  }
  checkHeapIntegrity("after_fb_realloc");
  return outcome;
}

bool EpubReaderActivity::buildSection(const RenderLayout& layout) {
  const int spineCount = epub->getSpineItemsCount();
  if (currentSpineIndex < 0 || currentSpineIndex >= spineCount) {
    LOG_ERR("ERS", "Render rejected invalid spine index %d (valid 0..%d)", currentSpineIndex, spineCount - 1);
    currentSpineIndex = 0;
    navTarget = NavigationTarget::makePage(0);
    automaticPageTurnActive = false;
    requestUpdate();
    return false;
  }

  const uint16_t viewportWidth = layout.viewportWidth;
  const uint16_t viewportHeight = layout.viewportHeight;
  const bool embeddedStyle = lastRenderStats.embeddedStyle;
  const uint8_t imageRendering = lastRenderStats.imageRendering;
  const auto filepath = epub->getSpineItem(currentSpineIndex).href;
  LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
  // Adopt the Background-B Section when entering exactly the spine it was working on: a
  // completed background build turns into a plain cache hit below, and a partial build
  // resumes in the indexing path instead of restarting from scratch. On any other
  // navigation the B state is stale — drop it (aborting a partial build). While a build
  // is live, loadSectionFile must be skipped: it would clobber the live write handle.
  bool resumeBackgroundBuild = false;
  if (backgroundSection_ && backgroundBuildSpineIndex_ == currentSpineIndex) {
    resumeBackgroundBuild = backgroundSection_->hasActiveBuild();
    // Distinguish the three adopt cases for an accurate log: a live partial build (resume), a
    // B-completed section with pages (cache hit follows), or a section B only probed and parked
    // in WaitHeap (no pages — loadSectionFile will miss and a foreground/C build follows).
    const char* adoptKind = resumeBackgroundBuild                 ? "resuming partial build"
                            : (backgroundSection_->pageCount > 0) ? "build complete"
                                                                  : "probed only, will build";
    section = std::move(backgroundSection_);
    LOG_INF("ERS", "Adopting background section for spine %d (%s)", currentSpineIndex, adoptKind);
  } else {
    section = std::make_unique<Section>(epub, currentSpineIndex, renderer);
  }
  resetBackgroundBuild();
  const unsigned long sectionStart = millis();

  // Preview text is baked into laid-out pages. Prepare its book-level source before
  // probing the preview-enabled section variant so a cache hit can never bypass gather.
  if (getEffectiveInlineFootnotePreviews()) {
    ensureFootnotePreviewCache();
  }

  // A resumed partial Background-B build has no on-disk LUT yet, so skip loadSectionFile (it
  // would clobber the live write handle); it always needs building. Otherwise probe the cache.
  const bool cacheHit =
      !resumeBackgroundBuild &&
      section->loadSectionFile(getEffectiveReaderFontId(), getEffectiveReaderLineCompression(),
                               SETTINGS.extraParagraphSpacing, getEffectiveParagraphAlignment(), viewportWidth,
                               viewportHeight, getEffectiveHyphenation(), embeddedStyle, getEffectiveBionicReading(),
                               getEffectiveInlineFootnotePreviews(), imageRendering);
  const bool cssFallbackRebuild = cacheHit && section->isEmbeddedStyleFallback();
  const bool needBuild = resumeBackgroundBuild || !cacheHit || cssFallbackRebuild;

  // One grep-able line per section entry tying the cache decision to the effective
  // preview inputs — discriminates "stale variant loaded" from "effective value not
  // what the UI shows" from "parser gate leak" in field logs.
  LOG_INF("ERS", "Section probe spine=%d: cacheHit=%d previews=%d (override=%d global=%u)", currentSpineIndex,
          cacheHit ? 1 : 0, getEffectiveInlineFootnotePreviews() ? 1 : 0,
          static_cast<int>(bookInlineFootnotePreviewsOverride), static_cast<unsigned>(SETTINGS.inlineFootnotePreviews));

  if (needBuild) {
    lastRenderStats.cacheRebuilt = true;

    // Background-C: build the current section incrementally on the loop task so input stays
    // responsive and pages appear as they are written. Used for the clean cases — a cache miss
    // or a resumed partial B build. The CSS-fallback rebuild keeps the blocking path: the section
    // already shows usable fallback content.
    // The spine's uncompressed size sizes the extraction inflate ring, so the resident/released
    // choice needs it (one central-dir scan; ~zero next to the ~1.4 s a doomed resident attempt
    // wastes). A lookup failure leaves 0 = unknown -> static floors only, the old behaviour.
    size_t inflatedSize = 0;
    epub->getSpineItemInflatedSize(currentSpineIndex, &inflatedSize);

    const SectionBuildMode mode = (resumeBackgroundBuild || !cacheHit) && !cssFallbackRebuild
                                      ? chooseSectionBuildMode(embeddedStyle, inflatedSize)
                                      : SectionBuildMode::Blocking;
    const bool incremental = mode != SectionBuildMode::Blocking;
    bool runBlocking = !incremental;

    // Single grep-able marker for the build mode actually taken for this spine — pairs with the
    // heapAllowsInPlaceBuild line above to explain every section-entry build decision from the log.
    LOG_INF("ERS", "Section build mode spine=%d: %s (cacheHit=%d cssFallback=%d resumeB=%d)", currentSpineIndex,
            mode == SectionBuildMode::Blocking              ? "BLOCKING"
            : mode == SectionBuildMode::IncrementalResident ? "INCR_RESIDENT"
                                                            : "INCR_RELEASED",
            cacheHit ? 1 : 0, cssFallbackRebuild ? 1 : 0, resumeBackgroundBuild ? 1 : 0);

    if (incremental) {
      // Draw the popup BEFORE any secondary-buffer release. drawPopup() overlays the box on the
      // on-screen frame via syncWriteBufferFromDisplayed(), which copies from frameBufferActive —
      // once the secondary buffer is released that copy is gone (the call no-ops) and the box would
      // be composited onto the stale two-refreshes-ago write buffer, ghosting the previous page
      // under the popup. Capture the "dramatic transition" signal first, because the popup's own
      // refresh would otherwise consume the pending exit-full-refresh override:
      //   - cold open of this book (coldOpenHalfRefreshArmed_), or
      //   - a pending exit-full-refresh override left by a deliberate jump (chapter/percent/footnote)
      //     to a possibly-uncached section. Capture it now (a peek, not a consume) so the content
      //     page below can be forced to HALF; without capturing it the content page would fall back
      //     to a FAST diff against the popup frame and ghost its outline.
      const bool dramaticTransition = coldOpenHalfRefreshArmed_ || renderer.hasRefreshOverridePending();
      coldOpenHalfRefreshArmed_ = false;
      // X4: the dramatic-transition HALF belongs on the first CONTENT page (forceHalfRefreshAfterPopup_
      // below), not the popup. The popup is a transient box over the already-correct current page, so a
      // FAST overlay is clean and instant — drop the pending HALF override here so drawPopup() doesn't
      // spend a second ~1.7s HALF on the popup itself. On X3 the popup's own refresh IS the baseline
      // step (its displayBuffer updates DTM1, which the following FAST content page diffs against), so
      // leave the override for drawPopup() to consume there.
      if (!renderer.isX3() && dramaticTransition) {
        renderer.clearRefreshOverride();  // discard the armed HALF -> popup paints FAST
      }
      GUI.drawPopup(renderer, tr(STR_INDEXING));  // immediate feedback before the first page lands

      if (mode == SectionBuildMode::IncrementalReleased) {
        // Tight heap: free the secondary buffer (~48–52 KB) for the build. AA is off until the
        // build ends and recoverSecondaryBufferIfNeeded() reallocates it (marked via
        // secondaryBufferDegraded_); mid-build draws are BW. No display downside on X3 (baseline
        // in controller).
        //
        // X4: a long chapter can keep this build running for many page turns (10s of seconds),
        // and displayBuildPage() requests FAST refreshes via the normal cadence the whole time —
        // without the opt-in below, EInkDisplay::triggerDisplay() silently downgrades every one
        // of those to HALF (no host-side previous-frame copy to diff against), so every mid-build
        // page turn pays the slow waveform. Seed RED RAM from the popup frame now on screen (drawn
        // just above, while the buffer was still resident) BEFORE releasing, then opt in to
        // single-buffer fast differential so FAST refreshes keep diffing against the controller's
        // retained RED RAM copy. Symmetric setSingleBufferFastDiff(false) lives in
        // recoverSecondaryBufferIfNeeded(), the one place this released state gets cleanly restored.
        const uint32_t freeBefore = esp_get_free_heap_size();
        if (!renderer.isX3()) renderer.syncRedRamFromFrameBuffer();
        // Prefer BORROWING the secondary buffer over freeing it: the lent block never enters
        // the heap, so a survivor can't split it into a fragmented hole and the return cannot
        // fail (the realloc-failure / heap-recovery-restart class of bugs is impossible). The
        // build's arena bump-allocates inside the lent region instead of the heap. Fall back to
        // the legacy release only when there is no secondary buffer to lend.
        size_t borrowedSize = 0;
        uint8_t* borrowed = renderer.borrowSecondaryBuffer(&borrowedSize);
        if (borrowed) {
          buildScratch_ = makeUniqueNoThrow<BuildArena>(borrowed, borrowedSize);
          section->setExternalBuildScratch(buildScratch_ && buildScratch_->valid() ? buildScratch_.get() : nullptr);
          secondaryBorrowed_ = true;
        } else {
          renderer.releaseSecondaryBuffer();
        }
        // Display semantics are identical to a released buffer (single-buffer mode either way).
        renderer.setSingleBufferFastDiff(true);
        secondaryBufferDegraded_ = true;
        LOG_INF("ERS", "Background-C: building spine %d incrementally, secondary buffer %s (free %lu->%lu)",
                currentSpineIndex, borrowed ? "BORROWED" : "RELEASED", freeBefore, esp_get_free_heap_size());
      } else {
        LOG_INF("ERS", "Background-C: building spine %d incrementally, buffer resident (free=%lu contig=%lu)",
                currentSpineIndex, esp_get_free_heap_size(),
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
      }
      // Force the first REAL page that replaces the popup — whether shown by displayBuildPage()
      // (multi-slice build) or directly by renderContents() (build finishes in one slice, e.g. a
      // one-page cover) — to HALF, so a dramatic content change (text popup -> photo) doesn't leave
      // a ghost outline of the popup box, and so released-buffer builds don't compound that ghosting
      // across many subsequent FAST mid-build pages before the periodic full-resync cadence cleans
      // it up. X4 only: its FAST refresh needs a host-side previous-frame copy (or the single-buffer-
      // fast-diff opt-in above) to diff against, so a dramatic frame change can under-drive pixels.
      // X3's fast differential reads the controller's own DTM1 RAM, which drawPopup()'s displayBuffer()
      // call already updated correctly — no host-side baseline gap to paper over, so forcing HALF
      // there is pure unnecessary cost. A routine forward-reading crossing into a still-building
      // Background-B section is NOT dramatic (neither signal is set), so it keeps the fast cadence.
      if (!renderer.isX3() && dramaticTransition) {
        forceHalfRefreshAfterPopup_ = true;
      }
      renderer.clearFontAccumulation();
      readerPhase_ = ReaderPhase::PRECOMPILING;
      // Seed the display cursor so the SectionBuilding pass knows which page to show first. For a
      // Page target it's the requested page (usually 0); other targets show the popup until the
      // build completes and the position is resolved, so the exact value doesn't matter there.
      section->currentPage = (navTarget.kind == NavigationTarget::Kind::Page) ? navTarget.page : 0;
      buildDisplayedPage_ = -1;
      buildingPopupShown_ = true;
      // Kick the build off so hasActiveBuild() is true (a resumed B build already is). One slice
      // may already finish a tiny section, in which case we fall through to a normal render.
      Section::BuildStep step = Section::BuildStep::More;
      if (!section->hasActiveBuild()) {
        step = section->stepSectionBuild(makeSectionBuildParams(), BG_BUILD_BUDGET_MS);
      }
      if (step == Section::BuildStep::More) {
        requestUpdate();  // SectionBuilding pass + stepCurrentSectionBuild() take over
        return false;
      }
      readerPhase_ = ReaderPhase::READING;
      if (step == Section::BuildStep::Failed) {
        LOG_ERR("ERS", "Background-C kickoff failed for spine %d; using blocking path", currentSpineIndex);
        // The failed kickoff tore its build state down (releasing into the arena). If we borrowed
        // the secondary buffer above, hand it back before the blocking path: compileSectionCache
        // manages a resident buffer (releases it for real heap), so it must start with the block
        // back on the display. The return cannot fail — the region never entered the heap.
        if (secondaryBorrowed_) {
          section->setExternalBuildScratch(nullptr);
          buildScratch_.reset();
          renderer.returnSecondaryBuffer();
          secondaryBorrowed_ = false;
          secondaryBufferDegraded_ = false;
          renderer.setSingleBufferFastDiff(false);
        }
        forceBlockingBuildSpine_ = currentSpineIndex;
        runBlocking = true;
      }
      // step == Done: tiny section finished in one slice — fall through to resolveInto/return true.
    }

    if (runBlocking) {
      if (!incremental) {
        // Background-C was not chosen (CSS-fallback rebuild, no secondary buffer, or the
        // blocking-fallback latch is set for this spine) — log so the mode is visible in traces.
        LOG_INF("ERS", "Background-C declined for spine %d (cssFallback=%d hasBuf=%d latched=%d); blocking build",
                currentSpineIndex, cssFallbackRebuild ? 1 : 0, renderer.hasSecondaryBuffer() ? 1 : 0,
                forceBlockingBuildSpine_ == currentSpineIndex ? 1 : 0);
      }
      const BuildOutcome outcome = compileSectionCache(layout, embeddedStyle, imageRendering);
      if (outcome == BuildOutcome::Restarting) {
        return false;  // fragmented-heap recovery reboot in progress
      }
      renderer.restoreFontMetadata();
      readerPhase_ = ReaderPhase::READING;
      if (outcome == BuildOutcome::Failed) {
        if (cssFallbackRebuild) {
          LOG_ERR("ERS", "Failed to rebuild CSS section cache; keeping fallback");
          section->loadSectionFile(getEffectiveReaderFontId(), getEffectiveReaderLineCompression(),
                                   SETTINGS.extraParagraphSpacing, getEffectiveParagraphAlignment(), viewportWidth,
                                   viewportHeight, getEffectiveHyphenation(), embeddedStyle,
                                   getEffectiveBionicReading(), getEffectiveInlineFootnotePreviews(), imageRendering);
        } else {
          LOG_ERR("ERS", "Failed to build section; showing empty chapter");
          // Do NOT reset section: leave it alive with pageCount=0 so getRenderPass()
          // returns Normal on the next cycle (not BuildSection), breaking the retry loop.
          // renderNormalPass handles pageCount==0 gracefully with an "empty chapter" screen.
          requestUpdate();
          return false;
        }
      }
    }
  } else {
    LOG_DBG("ERS", "Cache found, skipping build...");
  }
  // Any section entry that resolved here without an ongoing incremental build (cache hit, blocking
  // build, or a tiny incremental build that finished in one slice) spends the cold-open arm: it is
  // valid for the FIRST section entry only, so a cached re-open's later forward crossings into
  // still-building sections are not mistaken for the dramatic cold-open transition. A multi-slice
  // incremental build returns earlier and has already consumed the arm in its popup branch.
  coldOpenHalfRefreshArmed_ = false;
  lastRenderStats.sectionLoadMs = millis() - sectionStart;

  if (section->isTruncatedCache() && currentSpineIndex != lastWarnedTruncatedSpineIndex) {
    lastWarnedTruncatedSpineIndex = currentSpineIndex;
    truncatedSectionHintRendersRemaining = TRUNCATED_SECTION_HINT_RENDER_COUNT;
    LOG_INF("ERS", "Section %d is truncated; showing mitigation hint", currentSpineIndex);
  }

  // Section is ready (cache hit, blocking build, or a tiny C build that finished in one slice):
  // the Background-C fallback latches for this spine have served their purpose.
  forceBlockingBuildSpine_ = -1;
  forceReleasedBuildSpine_ = -1;

  LOG_DBG("ERS", "resolveInto: navTarget.kind=%d pageCount=%d", (int)navTarget.kind, (int)section->pageCount);
  navTarget.resolveInto(*section, currentSpineIndex);
  LOG_DBG("ERS", "resolveInto result: currentPage=%d", (int)section->currentPage);
  navTarget = NavigationTarget::makePage(section->currentPage);
  forceLoadLargeImages = false;
  pageHasPlaceholders = false;
  return true;
}

void EpubReaderActivity::renderNormalPass(RenderLock& lock, const RenderLayout& layout) {
  const int orientedMarginTop = layout.marginTop;
  const int orientedMarginRight = layout.marginRight;
  const int orientedMarginBottom = layout.marginBottom;
  const int orientedMarginLeft = layout.marginLeft;

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  {
    const unsigned long pageLoadStart = millis();
    auto p = section->loadPageFromSectionFile();
    lastRenderStats.pageLoadMs = millis() - pageLoadStart;
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      section->clearCache();
      section.reset();
      requestUpdate();  // Try again after clearing cache
                        // TODO: prevent infinite loop if the page keeps failing to load for some reason
      automaticPageTurnActive = false;
      return;
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);
    lastRenderStats.hadImages = p->hasImages();
    lastRenderStats.footnoteCount = static_cast<int>(currentPageFootnotes.size());
    lastRenderStats.spineIndex = currentSpineIndex;
    lastRenderStats.pageIndex = section->currentPage;
    lastRenderStats.pageCount = section->pageCount;
    showTruncatedSectionHintThisRender = truncatedSectionHintRendersRemaining > 0;

    const auto start = millis();
    renderContents(lock, std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom,
                   orientedMarginLeft);
    lastRenderStats.requestRenderMs = millis() - start;
    if (truncatedSectionHintRendersRemaining > 0) {
      truncatedSectionHintRendersRemaining--;
    }
    LOG_DBG("ERS", "Rendered page in %dms", lastRenderStats.requestRenderMs);
    checkHeapIntegrity("after_page_render");
  }
  // Re-acquire the render lock before any further state mutation.
  // renderContents() released it early (after triggerDisplay) to free the loop
  // task during the waveform. Everything below touches shared reader state and
  // must be serialised against loop()-side mutations.
  {
    RenderLock relock;

    // Defensive guard: section can be invalidated by loop-side flows while the
    // render lock was released during display waveform wait.
    if (!section) {
      LOG_ERR("ERS", "render: section became null after display completion");
      return;
    }

    pendingProgressSave.spineIndex = currentSpineIndex;
    pendingProgressSave.page = section->currentPage;
    pendingProgressSave.pageCount = section->pageCount;
    pendingProgressSave.pending.store(true, std::memory_order_release);
    lastRenderStats.freeHeapAfter = esp_get_free_heap_size();
    // Avoid heap walk in the hot render path; largest free block is sampled in index lifecycle logs.
    lastRenderStats.largestFreeBlockAfter = 0;
    lastRenderStats.valid = true;
    const uint32_t totalFontLookups = lastRenderStats.fontCacheHits + lastRenderStats.fontCacheMisses;
    const uint32_t fontHitRatePct =
        totalFontLookups > 0 ? (lastRenderStats.fontCacheHits * 100UL) / totalFontLookups : 0UL;
    LOG_DBG(
        "ERS",
        "Page summary: spine=%d page=%d/%d prerendered=0 refresh=%s mode=0x%02X renderMs=%lu "
        "prewarmMs=%lu bwMs=%lu displayMs=%lu fontHits=%lu fontMisses=%lu fontHitPct=%lu glyphCalls=%lu glyphUs=%lu",
        currentSpineIndex, section->currentPage, section->pageCount, refreshModeName(lastPageRefreshMode_),
        lastPageDisplayModeByte_, lastRenderStats.requestRenderMs, lastRenderStats.phases.prewarmMs,
        lastRenderStats.phases.bwRenderMs, lastRenderStats.phases.displayMs, lastRenderStats.fontCacheHits,
        lastRenderStats.fontCacheMisses, fontHitRatePct, lastRenderStats.fontGetBitmapCalls,
        lastRenderStats.fontGetBitmapTimeUs);

    if (pendingScreenshot) {
      // No restoreCurrentPageToBufferIfPreRendered() needed here: we are inside renderContents()
      // right after a fresh full render of the current page, before any pre-render is re-armed,
      // so the frame buffer already holds exactly what is on screen.
      pendingScreenshot = false;
      ScreenshotUtil::takeScreenshot(renderer);
    }

    // Pre-render was already scheduled in renderContents() before the lock was
    // released, so the loop task could start it during the waveform wait.
  }
}

void EpubReaderActivity::renderSectionBuildingPass(RenderLock& lock, const RenderLayout& layout) {
  if (!section || !section->hasActiveBuild()) {
    // Build finished or was aborted between classifyRenderPass() and here — re-dispatch so the
    // correct pass (Normal, or a fresh BuildSection after a cross/fallback) runs.
    requestUpdate();
    return;
  }

  // Draw the requested page if Background-C has already written it. Only for an explicit Page
  // target (other targets resolve at completion). buildDisplayedPage_ records the target we've
  // already acted on so we neither redraw it every tick nor (via the C step's nudge gate) get
  // pinged every slice.
  const int target = section->currentPage;
  const int built = static_cast<int>(section->activeBuildPageCount());
  if (navTarget.kind == NavigationTarget::Kind::Page && target >= 0 && target < built) {
    if (target == buildDisplayedPage_) {
      return;  // already handled this target (drawn, or an image page we're waiting out)
    }
    // Text-only pages render cleanly without the image decode / secondary-buffer dance that the
    // lock-light build path avoids; an image target waits for the final Normal render but is
    // still marked handled so the C step stops nudging us about it.
    auto page = section->loadPageFromActiveBuild(static_cast<uint16_t>(target));
    buildDisplayedPage_ = target;
    if (page && !page->hasImages()) {
      buildingPopupShown_ = false;
      displayBuildPage(lock, *page, layout);  // releases the lock before the waveform wait
      return;
    }
    // Image page or load failure: fall through to the popup until the build completes.
  }

  // Requested page not built yet (or it's an image page / non-Page target): show the indexing
  // popup once, leaving any already-displayed page underneath it.
  if (!buildingPopupShown_) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    buildingPopupShown_ = true;
  }
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  recoverSecondaryBufferIfNeeded();

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) {
    LOG_ERR("ERS", "EPUB has no spine items, aborting render");
    automaticPageTurnActive = false;
    return;
  }

  clampSpineIndex(spineCount);

  // The finished-book pass needs no layout/stats setup and consumes the lock itself.
  if (currentSpineIndex == spineCount) {
    renderFinishedBookPass(lock, spineCount);
    return;
  }

  // Push the render-time guide-dots option before any page draws (the scheduled
  // pre-render also picks it up: it only runs after this). Unlike bionic reading
  // this is not part of the section cache key, so toggling needs no rebuild.
  TextBlock::setGuideDots(getEffectiveGuideDots());

  const RenderLayout layout = computeRenderLayout();
  lastRenderStats = {};
  lastRenderStats.orientation = static_cast<uint8_t>(renderer.getOrientation());
  lastRenderStats.marginTop = layout.marginTop;
  lastRenderStats.marginRight = layout.marginRight;
  lastRenderStats.marginBottom = layout.marginBottom;
  lastRenderStats.marginLeft = layout.marginLeft;
  lastRenderStats.viewportWidth = layout.viewportWidth;
  lastRenderStats.viewportHeight = layout.viewportHeight;
  lastRenderStats.embeddedStyle = getEffectiveEmbeddedStyle();
  lastRenderStats.imageRendering = getEffectiveImageRendering();
  lastRenderStats.effectiveFontId = getEffectiveReaderFontId();
  lastRenderStats.textAntiAliasing = getEffectiveTextAntiAliasing();
  lastRenderStats.freeHeapBefore = esp_get_free_heap_size();
  // Avoid heap walk in the hot render path; largest free block is sampled in index lifecycle logs.
  lastRenderStats.largestFreeBlockBefore = 0;
  showTruncatedSectionHintThisRender = false;

  // Hold off a pure pre-render while a deferred AA pass is still owed for the
  // CURRENT page. The pre-render writes the next page into frameBuffer, but the
  // deferred AA's cleanup (cleanupGrayscaleWithPreviousBuffer) ends by copying
  // frameBufferActive — the current page — back over frameBuffer. If the pre-render
  // ran first, that copy would clobber the pre-rendered next page, and the next
  // page turn (BufferDisplay) would then flush the stale current page with only a
  // fresh status bar drawn over it (observed on X3 as "every second page doesn't
  // change"). Keep pendingPreRender armed and bail; runDeferredGrayscalePass()
  // re-requests an update once the AA pass has run, at which point the pre-render
  // proceeds against the correct buffer state. Guard only the pre-render: a real
  // page turn (usePreRenderedBuffer / Normal) must still render immediately.
  //
  // X3 only: this ordering hazard exists because X3 keeps _refreshPending asserted
  // for the whole multi-second waveform, which blocks runDeferredGrayscalePass()
  // (it self-gates on !isRefreshPending()) and lets the pre-render slip in ahead of
  // the AA. X4 clears _refreshPending inline in triggerDisplay(), so the deferred AA
  // — which serviceBackgroundWork() runs first — always completes before any
  // pre-render; the guard is unnecessary there and reordering its differential
  // refresh / RED-RAM baseline only reintroduces ghosting.
  if (renderer.isX3() && pendingGrayscale_.active && pendingPreRender && !usePreRenderedBuffer &&
      classifyRenderPass() == RenderPass::PreRender) {
    return;
  }

  // Classify the pass, then consume the pre-render flags.
  const RenderPass pass = classifyRenderPass();
  pendingPreRender = false;
  usePreRenderedBuffer = false;
  // Any other pass redraws the write framebuffer and flushes/swaps, destroying pre-rendered
  // pixels — so a completed pre-render must be discarded here even when its (spineIndex,
  // pageIndex) still matches the current page. Keeping ready=true across an intervening
  // Normal render (periodic status-bar/battery/clock update) let the next page turn take the
  // BufferDisplay path and flush the STALE current page with only a fresh status bar drawn
  // over it (observed as "page counter advances but content doesn't, every 30-40 pages" —
  // the battery-percent tick cadence). This does not cost the hit: renderContents() re-arms
  // pendingPreRender when ready is false, and the re-render runs during the same render's
  // waveform wait. The BufferDisplay/PreRender passes manage ready themselves.
  if (pass != RenderPass::PreRender && pass != RenderPass::BufferDisplay && preRenderedPage.ready) {
    preRenderedPage.ready = false;
  }

  switch (pass) {
    case RenderPass::FinishedBook:
      // Handled above before layout setup; unreachable here.
      return;
    case RenderPass::PreRender:
      renderPreRenderPass(layout);
      return;
    case RenderPass::BufferDisplay:
      if (renderBufferDisplayPass(layout)) {
        return;
      }
      // Fast path could not be taken (load failed / image page) — fall through to Normal.
      renderNormalPass(lock, layout);
      return;
    case RenderPass::BuildSection:
      if (!buildSection(layout)) {
        return;
      }
      // Flush any image dimensions this build just resolved (foreground path).
      epub->persistImageManifest();
      renderNormalPass(lock, layout);
      return;
    case RenderPass::SectionBuilding:
      renderSectionBuildingPass(lock, layout);
      return;
    case RenderPass::Normal:
      renderNormalPass(lock, layout);
      return;
  }
}

bool EpubReaderActivity::maybeRestartForFragmentedHeap(const uint32_t freeHeap, const uint32_t contigHeap) {
  // Reboot-based defrag should only run when the failure clearly looks like
  // fragmentation (plenty of total heap, but contiguous block too small).
  constexpr uint32_t RESTART_MIN_FREE_HEAP_BYTES = 96 * 1024;
  constexpr uint32_t SECONDARY_BUFFER_BYTES = 52 * 1024;

  if (fragmentationRecoveryRestartAttempted_) {
    return false;
  }
  if (freeHeap < RESTART_MIN_FREE_HEAP_BYTES || contigHeap >= SECONDARY_BUFFER_BYTES) {
    return false;
  }

  fragmentationRecoveryRestartAttempted_ = true;

  // Persist the live position only when it is authoritative. During a section (re)build
  // (readerPhase_ != READING, or section already reset for one) the saved navTarget has not
  // been applied yet — a fresh Section still sits at currentPage 0 — and saving that would
  // clobber the user's real position in progress.bin, which already holds the correct value
  // (issue #75: progress reset to the chapter start after a heap-recovery reboot).
  const int page = (section ? section->currentPage : 0);
  const int pageCount = (section ? section->pageCount : 0);
  if (section && readerPhase_ == ReaderPhase::READING) {
    saveProgress(currentSpineIndex, page, pageCount);
  }

  // Release both framebuffers (primary + secondary already gone) to free ~48 KB
  // more contiguous heap, then do a pre-reboot warm pass for any images that
  // couldn't be decoded with only the secondary released. Pixel writes land in a
  // small scratch buffer (discarded on reboot); we only care about the .pxc files
  // that get written to SD so the next boot can render images without a decoder.
  if (section) {
    const size_t scratchSize = static_cast<size_t>(renderer.getDisplayWidthBytes()) * renderer.getDisplayHeight();
    auto* scratch = static_cast<uint8_t*>(malloc(scratchSize));
    if (scratch && renderer.releaseFrameBuffersWithScratch(scratch, scratchSize)) {
      LOG_ERR("ERS", "Pre-reboot image warm pass: freed primary fb, scratch=%u bytes", scratchSize);
      const bool preRebootForceLoad = forceLoadLargeImages || !SETTINGS.largeImagePlaceholder;
      section->warmAllImageCaches(0, 0, preRebootForceLoad, /*monochromeOutput=*/true);
      // scratch is leaked intentionally — reboot follows immediately
    } else {
      free(scratch);
    }
  }

  LOG_ERR("ERS", "Fragmented heap recovery: free=%lu contig=%lu, restarting directly to reader (spine=%d page=%d/%d)",
          freeHeap, contigHeap, currentSpineIndex, page, pageCount);
  if (trySilentRestartToReaderForHeapRecovery()) {
    return true;
  }
  LOG_ERR("ERS", "Heap recovery restart blocked by safety latch; staying in degraded mode");
  return false;
}

bool EpubReaderActivity::writeReaderProgressCache(const std::string& cachePath, const int spineIndex,
                                                  const int currentPage, const int pageCount, const uint8_t percent) {
  FsFile f;
  if (!Storage.openFileForWrite("ERS", cachePath + "/progress.bin", f)) {
    LOG_ERR("ERS", "Failed to open progress cache: %s", cachePath.c_str());
    return false;
  }

  uint8_t data[7];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = currentPage & 0xFF;
  data[3] = (currentPage >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  data[6] = percent;
  f.write(data, 7);
  f.close();
  return true;
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  const uint8_t percent = epubProgressPercentByte(*epub, spineIndex, currentPage, pageCount);
  if (!writeReaderProgressCache(epub->getCachePath(), spineIndex, currentPage, pageCount, percent)) {
    LOG_ERR("ERS", "Could not save progress!");
    return;
  }
  // pageCount 0 means the percent is the "unknown" placeholder (see epubProgressPercentByte),
  // e.g. a mid-build page turn. Don't push it into the session tracker: recordSession()
  // overwrites the stored per-book progress, so an unknown 0 at session end would regress it.
  if (pageCount > 0) {
    globalReadingSessionTracker().updateProgress(percent);
  }
  LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d (%d%%)", spineIndex, currentPage, percent);
}
void EpubReaderActivity::renderContents(RenderLock& lock, std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  logReaderMemSnapshot("render_start");
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();

  const int viewportHeight = std::max(0, renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom);
  const int contentTop = orientedMarginTop + getImageOnlyPageYOffset(*page, viewportHeight);

  const bool aaEnabledForThisRender =
      getEffectiveTextAntiAliasing() && renderer.hasSecondaryBuffer() && !secondaryBufferDegraded_;
  if (getEffectiveTextAntiAliasing() && !aaEnabledForThisRender) {
    LOG_DBG("ERS", "AA skipped: secondary display buffer unavailable/degraded");
  }
  lastRenderStats.textAntiAliasing = aaEnabledForThisRender;

  // Always use 1-bit Atkinson dither for images in the epub reader.
  const bool imageMonochrome = true;

  // Warm any missing image pixel caches BEFORE font prewarm and BW backup chunks
  // reduce heap contig below the ~49 KB the PNG decoder needs. The decode
  // writes pixels into the framebuffer as a side effect, so we reclear before
  // the real BW render begins. Skips when no decode is needed (all images cached
  // or the page is text-only). Mirrors the effectiveForceLoad rule used by the
  // BW render below so placeholder logic is identical.
  //
  // If the secondary frame buffer is allocated (~52 KB on X3, ~48 KB on X4) and
  // the page has images that still need decoding, release it before the warm pass
  // so the PNG/JPEG decoder can use that contiguous block. The secondary buffer
  // is safe to release here because no waveform is pending: displayBuffer() has
  // not been called yet this render cycle. We reallocate after warm completes.
  // This mirrors the same technique used during section indexing (createSectionFile).
  const bool warmForceLoad = forceLoadLargeImages || !SETTINGS.largeImagePlaceholder;
  bool releasedSecondaryForWarm = false;
  // Set for the whole warm pass (decode included, not just the release/realloc bracket):
  // stepBackgroundSectionBuild()/stepCurrentSectionBuild() check this and refuse to start
  // heap-hungry work while it's true. RenderLock already excludes them structurally (both
  // bail on RenderLock::peek(), and this whole pass runs under the lock the render task took
  // before calling renderContents()) — this flag is a second, explicit guard against that
  // invariant silently breaking if a future change adds a yield point in here.
  imageProcessingActive_ = page->hasUncachedImages(warmForceLoad, imageMonochrome);
  if (imageProcessingActive_ && renderer.hasSecondaryBuffer()) {
    renderer.releaseSecondaryBuffer();
    releasedSecondaryForWarm = true;
    LOG_DBG("ERS", "Released secondary buffer for image warm pass");
  }
  page->warmImageCaches(renderer, orientedMarginLeft, contentTop, warmForceLoad, imageMonochrome);
  // Image decode (JPEG/PNG) is the deepest, most heap-hungry work in a render pass
  // and the prime suspect for the lazy multi_heap poisoning assert. Check here, right
  // after the warm/decode pass, so corruption is attributed to the decode rather than
  // to whatever frees next. Mirrors the after_createSectionFile tripwire.
  checkHeapIntegrity("after_image_warm_pass");
  if (releasedSecondaryForWarm) {
    // Safe to evict here: the font prewarm for this page runs AFTER this realloc (image
    // warm is deliberately ordered before it), so cleared glyph slots are rebuilt anyway.
    if (!reallocSecondaryEvictingCaches()) {
      LOG_ERR("ERS", "Failed to reallocate secondary buffer after image warm — display quality degraded");
      secondaryBufferDegraded_ = true;
      const uint32_t freeAfterWarm = esp_get_free_heap_size();
      // See the matching comment in compileSectionCache: do not walk the TLSF free-block
      // list here either, for the same post-decode-failure corruption risk.
      if (maybeRestartForFragmentedHeap(freeAfterWarm, 0)) {
        imageProcessingActive_ = false;
        return;  // fragmented-heap recovery reboot in progress
      }
    }
    // NOTE: do NOT syncRedRamFromFrameBuffer() here. reallocSecondaryBuffer() fills the new
    // secondary with WHITE, and syncRedRamFromFrameBuffer() copies frameBufferActive (that white
    // buffer) into RED RAM — which DESTROYS the correct baseline: RED already holds the previously
    // displayed frame (the page under this one) from its own post-display sync, and _redRamSynced
    // survives release/realloc. Reseeding from the white buffer made the next FAST differential diff
    // the new page against white, so the previous page (e.g. the cover) bled through ("next page
    // overloaded over the cover"). The displayed frame is gone from both host buffers after the
    // release, so the controller's retained RED RAM is the only correct baseline — leave it intact.
  }
  imageProcessingActive_ = false;
  renderer.clearScreen();

  logReaderMemSnapshot("prewarm_begin");

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  const uint32_t heapBefore = esp_get_free_heap_size();
  auto scope = fcm->createPrewarmScope();
  page->renderTextOnly(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop);  // scan pass
  scope.endScanAndPrewarm();
  const uint32_t heapAfter = esp_get_free_heap_size();
  fcm->logStats("prewarm");
  const auto tPrewarm = millis();

  LOG_DBG("ERS", "Heap: before=%lu after=%lu delta=%ld", heapBefore, heapAfter,
          (int32_t)heapAfter - (int32_t)heapBefore);
  logReaderMemSnapshot("prewarm_end");

  const bool effectiveForceLoad = forceLoadLargeImages || !SETTINGS.largeImagePlaceholder;
  pageHasPlaceholders = page->hasPlaceholderImages(effectiveForceLoad, imageMonochrome);

  bool forceHalfRefreshThisPage = pendingHalfRefreshAfterImagePage && SETTINGS.halfRefreshAfterImagePage;
  pendingHalfRefreshAfterImagePage = false;
  lastRenderStats.imagePageWithAA = false;
  lastRenderStats.forcedHalfRefresh = forceHalfRefreshThisPage;

  logReaderMemSnapshot("before_bw_render");
  page->render(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop, effectiveForceLoad,
               imageMonochrome);
  // The BW render also touches images (placeholder/cache draws) and runs the full
  // glyph pipeline; check here too so a clean after_image_warm_pass followed by a
  // corrupt reading convicts the BW render rather than the decode.
  checkHeapIntegrity("after_bw_render");
#if DEBUG_BACKGROUND_WORK
  // This page was rendered fresh on the render task (not served from a Background-A
  // pre-render). Mark the overlay as a miss before the status bar draws it.
  backgroundAGlyph_ = '-';
#endif
  renderStatusBar();
  if (showTruncatedSectionHintThisRender) {
    const int hintX = orientedMarginLeft + 4;
    const int hintY1 = contentTop + 4;
    const int hintY2 = hintY1 + 20;
    const int maxWidth = std::max(0, renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight - 8);
    // Clear a dedicated band so the hint stays readable over any page content.
    const int boxX = hintX - 2;
    const int boxY = hintY1 - 2;
    const int boxW = maxWidth + 4;
    const int boxH = 44;
    renderer.fillRect(boxX, boxY, boxW, boxH, false);
    renderer.drawRect(boxX, boxY, boxW, boxH, true);
    const std::string line1 = renderer.truncatedText(UI_10_FONT_ID, TRUNCATED_SECTION_HINT_LINE_1, maxWidth);
    const std::string line2 = renderer.truncatedText(UI_10_FONT_ID, TRUNCATED_SECTION_HINT_LINE_2, maxWidth);
    renderer.drawText(UI_10_FONT_ID, hintX, hintY1, line1.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, hintX, hintY2, line2.c_str(), true);
  }
  fcm->logStats("bw_render");
  const auto tBwRender = millis();
  logReaderMemSnapshot("after_bw_render");
  // Resolve this page's refresh mode (consuming any force flags), then fire it.
  // With AA enabled on X4 the refresh goes out async so the grayscale planes
  // can render during the waveform (inline AA below); everywhere else the
  // trigger blocks through the waveform exactly as before.
  HalDisplay::RefreshMode pageRefreshMode;
  if (secondaryBufferDegraded_) {
    // FULL_REFRESH already gives a clean baseline, same goal as forceHalfRefreshAfterPopup_;
    // consume it here too so it doesn't carry over and force an unrelated later page to HALF.
    forceHalfRefreshAfterPopup_ = false;
    pageRefreshMode = HalDisplay::FULL_REFRESH;
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else if (forceRefreshModeNextRender_ >= 0) {
    // Manual force-refresh button: apply the requested mode for this one render. A manual refresh
    // gives its own clean baseline, so consume any armed post-popup HALF too rather than letting it
    // carry over and force an unrelated later page to HALF.
    forceHalfRefreshAfterPopup_ = false;
    pageRefreshMode = static_cast<HalDisplay::RefreshMode>(forceRefreshModeNextRender_);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    forceRefreshModeNextRender_ = -1;
  } else if (forceHalfRefreshAfterPopup_) {
    // First real page after the indexing popup, shown directly here because the build finished
    // in a single slice (e.g. a one-page cover) and never went through displayBuildPage(). See
    // forceHalfRefreshAfterPopup_.
    forceHalfRefreshAfterPopup_ = false;
    pageRefreshMode = HalDisplay::HALF_REFRESH;
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else if (forceHalfRefreshThisPage) {
    pageRefreshMode = HalDisplay::HALF_REFRESH;
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    pageRefreshMode = ReaderUtils::nextRefreshCycleMode(pagesUntilFullRefresh);
  }
  // Inline AA is X4-only: X4 waits out the waveform inside the trigger, so the
  // async split hands that window to the plane renders. X3 returns pre-waveform
  // from its trigger already and keeps the deferred loop-task pass.
  const bool inlineAaThisRender = aaEnabledForThisRender && !renderer.isX3();
  if (inlineAaThisRender) {
    renderer.triggerDisplayAsync(pageRefreshMode);
  } else {
    renderer.triggerDisplay(pageRefreshMode);
  }
  // Real content is now on screen; any indexing popup it replaced is gone. Clear the flag for the
  // abandon-to-adjacent-section path, which reaches this Normal pass without going through
  // renderSectionBuildingPass()/displayBuildPage() where buildingPopupShown_ is otherwise reset.
  buildingPopupShown_ = false;
  // Capture this page's refresh mode/byte NOW, before the lock is released and the deferred-AA
  // grayscale display overwrites the renderer's last-mode (which would otherwise mislabel the
  // page summary logged afterwards).
  lastPageRefreshMode_ = renderer.getLastRefreshMode();
  lastPageDisplayModeByte_ = renderer.getLastDisplayModeByte();
  const auto tDisplay = millis();

  // Schedule a half-refresh on the next page turn after an image page to reduce ghosting.
  // Must be checked BEFORE page is moved into pendingGrayscale_ below.
  if (page->hasImages() && !page->allImagesArePlaceholders(effectiveForceLoad, imageMonochrome) &&
      getEffectiveImageRendering() != CrossPointSettings::IMAGES_SUPPRESS) {
    pendingHalfRefreshAfterImagePage = true;
  }

  if (inlineAaThisRender) {
    // Inline AA (X4): the BW waveform is still running from triggerDisplayAsync().
    // Render the grayscale planes now — the LSB plane lands inside the waveform
    // window, so after the wait only the LSB SPI write, the MSB plane and the
    // short gray flush remain. The AA touch-up then reads as the tail of the
    // page refresh instead of a separate later update (issue #71).
    renderer.setFastGrayscaleLut(SETTINGS.fastAntiAliasing);
    const int aaFontId = getEffectiveReaderFontId();
    const Page* pagePtr = page.get();
    const auto gt = renderer.renderGrayscalePlanesInterleaved([&](GfxRenderer::RenderMode) {
      pagePtr->renderTextOnly(renderer, aaFontId, orientedMarginLeft, contentTop);
      pagePtr->renderImagesFromGrayscaleCache(renderer, orientedMarginLeft, contentTop);
    });
    LOG_DBG("ERS", "Inline AA: planes=%lums gray=%lums restore=%lums", gt.planesMs, gt.displayMs, gt.restoreMs);
    checkHeapIntegrity("after_inline_aa");
    lastRenderStats.usedGrayscale = true;
    lastRenderStats.phases = {tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, 0, gt.planesMs, 0,
                              gt.displayMs,  gt.restoreMs,         millis() - t0};
  } else if (aaEnabledForThisRender) {
    // Deferred grayscale (X3): store context before releasing the lock, so
    // loop() can run the AA pass once the waveform ends. The page is kept
    // alive via shared_ptr.
    pendingGrayscale_.active = true;
    pendingGrayscale_.page = std::move(page);
    pendingGrayscale_.fontId = getEffectiveReaderFontId();
    pendingGrayscale_.marginLeft = orientedMarginLeft;
    pendingGrayscale_.contentTop = contentTop;
    pendingGrayscale_.fastLut = SETTINGS.fastAntiAliasing;
    lastRenderStats.usedGrayscale = true;
    lastRenderStats.phases = {tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, 0, 0, 0, 0, 0, tDisplay - t0};
  }

  // Collect font stats before releasing the lock (these read renderer state).
  if (const auto* cacheManager = renderer.getFontCacheManager()) {
    if (const auto* decompressor = cacheManager->getDecompressor()) {
      const auto& stats = decompressor->getStats();
      lastRenderStats.fontCacheHits = stats.cacheHits;
      lastRenderStats.fontCacheMisses = stats.cacheMisses;
      lastRenderStats.fontDecompressMs = stats.decompressTimeMs;
      lastRenderStats.fontUniqueGroups = stats.uniqueGroupsAccessed;
      lastRenderStats.fontPageBufferBytes = stats.pageBufferBytes;
      lastRenderStats.fontPageGlyphsBytes = stats.pageGlyphsBytes;
      lastRenderStats.fontPeakTempBytes = stats.peakTempBytes;
      lastRenderStats.fontGetBitmapTimeUs = stats.getBitmapTimeUs;
      lastRenderStats.fontGetBitmapCalls = stats.getBitmapCalls;
    }
  }

  // Schedule the pre-render BEFORE releasing the lock so the loop task can
  // execute it during the waveform wait (~2-4s on X3). frameBuffer is already
  // swapped — the loop task writes into the new (inactive) buffer while the
  // display controller scans the old one. If a page turn fires during the
  // waveform it will clear preRenderedPage.ready and the stale pre-render
  // result is discarded — no correctness issue.
  if (!preRenderedPage.ready && section && section->currentPage + 1 < section->pageCount) {
    pendingPreRender = true;
    requestUpdate();
  }

  // Release the render lock NOW — the waveform is running in hardware and
  // frameBuffer is already swapped. The loop task gets full CPU during the
  // waveform for input handling and pre-render scheduling.
  lock.unlock();

  // Sleep until BUSY deasserts, then do post-waveform SPI work (DTM1 resync
  // on X3, conditioning passes, flag updates). SPI ownership transfers back
  // to this task only after completeDisplay() returns.
  renderer.completeDisplay();
}

void EpubReaderActivity::displayBuildPage(RenderLock& lock, const Page& page, const RenderLayout& layout) {
  // Draws one text-only page from an in-progress Background-C build: a plain BW render + status
  // bar, no AA and no pre-render arming (those belong to the steady reading state set up by
  // renderNormalPass() once the build completes). Caller guarantees the page is text-only, so
  // no image decode / secondary-buffer release is needed. The lock is released before the
  // waveform wait — exactly like renderContents() — so a C build slice can run on the loop task
  // during the refresh.
  const int viewportHeight = std::max(0, renderer.getScreenHeight() - layout.marginTop - layout.marginBottom);
  const int contentTop = layout.marginTop + getImageOnlyPageYOffset(page, viewportHeight);

  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page.renderTextOnly(renderer, getEffectiveReaderFontId(), layout.marginLeft, contentTop);  // scan pass
  scope.endScanAndPrewarm();

  renderer.clearScreen();
  page.render(renderer, getEffectiveReaderFontId(), layout.marginLeft, contentTop, /*forceLoadLargeImages=*/false,
              /*monochromeOutput=*/true);
  renderStatusBar();
  if (forceHalfRefreshAfterPopup_) {
    // First real page after the indexing popup: establish a clean baseline (see
    // forceHalfRefreshAfterPopup_) instead of compounding onto the popup's FAST refresh.
    forceHalfRefreshAfterPopup_ = false;
    renderer.triggerDisplay(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    ReaderUtils::triggerWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  // Release the lock before the (blocking) waveform wait so stepCurrentSectionBuild() can run a
  // build slice on the loop task while the panel refreshes — the same hand-off renderContents()
  // uses for Background-A.
  lock.unlock();
  renderer.completeDisplay();
}

void EpubReaderActivity::renderPageContentOnly(const Page& page, const int orientedMarginTop,
                                               const int orientedMarginRight, const int orientedMarginBottom,
                                               const int orientedMarginLeft) {
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();

  const int viewportHeight = std::max(0, renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom);
  const int contentTop = orientedMarginTop + getImageOnlyPageYOffset(page, viewportHeight);

  auto scope = fcm->createPrewarmScope();
  page.renderTextOnly(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop);
  scope.endScanAndPrewarm();

  renderer.clearScreen();
  page.render(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop);
  // Status bar intentionally omitted — superimposed at display time with live values.
}

void EpubReaderActivity::displayPreRenderedPage(const Page& page, const int orientedMarginTop,
                                                const int orientedMarginRight, const int orientedMarginBottom,
                                                const int orientedMarginLeft) {
  const int viewportHeight = std::max(0, renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom);
  const int contentTop = orientedMarginTop + getImageOnlyPageYOffset(page, viewportHeight);

#if DEBUG_BACKGROUND_WORK
  // This page is being served from the Background-A pre-render buffer (a hit) — set the
  // overlay glyph BEFORE renderStatusBar() draws it, so the status bar reflects that
  // background rendering produced this page.
  backgroundAGlyph_ = 'x';
#endif
  renderStatusBar();

  // Pre-rendered pages are text-only (image pages are excluded from pre-rendering), so
  // imagePageWithAA never applies here. The image-page follow-up half-refresh can still carry
  // over if the previous page had images; honour and clear it here so the pre-render path
  // doesn't skip it.
  const bool forceHalfRefreshThisPage = pendingHalfRefreshAfterImagePage && SETTINGS.halfRefreshAfterImagePage;
  pendingHalfRefreshAfterImagePage = false;
  if (secondaryBufferDegraded_) {
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else if (forceHalfRefreshThisPage) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  // Capture before the AA replay below overwrites the renderer's last-mode (see renderContents).
  lastPageRefreshMode_ = renderer.getLastRefreshMode();
  lastPageDisplayModeByte_ = renderer.getLastDisplayModeByte();

  if (getEffectiveTextAntiAliasing() && renderer.hasSecondaryBuffer() && !secondaryBufferDegraded_) {
    const int fontId = getEffectiveReaderFontId();
    // Re-warm the page's glyph BITMAPS before the AA replay. The pre-render pass warmed
    // them, but background work since may have dropped or re-wired the cache (B's
    // build slices reset the font accumulation to the metadata-only flash tables) —
    // and replaying against a metadata-only table dereferences a null bitmap base
    // (observed: Load access fault at glyph->dataOffset + heap corruption). When the
    // cache is still warm this is nearly free via the prewarm coverage fast-path.
    {
      auto* fcm = renderer.getFontCacheManager();
      auto scope = fcm->createPrewarmScope();
      page.renderTextOnly(renderer, fontId, orientedMarginLeft, contentTop);  // scan pass
      scope.endScanAndPrewarm();
    }
    renderer.setFastGrayscaleLut(SETTINGS.fastAntiAliasing);
    renderer.renderGrayscalePlanesSequential(
        [&](GfxRenderer::RenderMode) { page.renderTextOnly(renderer, fontId, orientedMarginLeft, contentTop); });
    // timings not recorded for the pre-rendered path
  }
  checkHeapIntegrity("after_bufferdisplay_aa");
}

void EpubReaderActivity::restoreCurrentPageToBufferIfPreRendered() {
  if (!preRenderedPage.ready || !section || !epub) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const int statusBarTopHeight = UITheme::getStatusBarTopHeight(automaticPageTurnActive);
  const int statusBarBottomHeight = UITheme::getStatusBarBottomHeight(automaticPageTurnActive);
  orientedMarginTop += std::max(static_cast<int>(SETTINGS.screenMargin), statusBarTopHeight);
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom += std::max(static_cast<int>(SETTINGS.screenMargin), statusBarBottomHeight);

  auto p = section->loadPageFromSectionFile();
  if (!p) {
    return;
  }
  renderPageContentOnly(*p, orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  preRenderedPage.ready = false;
  pendingPreRender = false;
  usePreRenderedBuffer = false;
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book. During an active section build pageCount only reflects pages
  // built so far, not the final chapter length, so show a byte-based estimate ("page X of ~Y")
  // instead of the misleading watermark. estimatedTotalPages() returns 0 while it's still too
  // early to project, which suppresses the fraction/progress (same as a plain pageCount of 0).
  const bool building = section->hasActiveBuild();
  const int currentPage = section->currentPage + 1;
  const int displayPageCount = building ? section->estimatedTotalPages() : section->pageCount;
  const float pageCount = static_cast<float>(displayPageCount);
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    const int tocIndex =
        section ? section->getTocIndexForPage(section->currentPage) : epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex == -1) {
      title = tr(STR_UNNAMED);
    } else {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  const bool isStarred = section && bookmarkStore.has(static_cast<uint16_t>(currentSpineIndex),
                                                      static_cast<uint16_t>(section->currentPage));
  std::string printedPageLabel;
  if (section && SETTINGS.statusBarPrintedPage) {
    const auto page = static_cast<uint16_t>(section->currentPage);
    if (const auto label = section->getPrintedPageLabelForPage(page)) {
      // Exact-match label (already parenthesised, may be "7/8" when multiple anchors collapse).
      printedPageLabel = *label;
    } else if (const auto nearest = section->getNearestPrintedPageLabelAtOrBefore(page)) {
      // No pagebreak on this device page: show the last printed-page label we passed within
      // this section so the status bar still tells the reader which printed page they're on.
      printedPageLabel = std::string("(") + *nearest + ")";
    }
  }
  GUI.drawStatusBar(renderer, bookProgress, currentPage, displayPageCount, title, 0, isStarred, printedPageLabel,
                    /*fillMargin=*/true, /*pageCountApproximate=*/building);

#if DEBUG_BACKGROUND_WORK
  renderBackgroundDebugOverlay();
#endif

  lastStatusBarPage = currentPage;
  lastStatusBarBattery = SETTINGS.statusBarBattery ? static_cast<int>(powerManager.getBatteryPercentage()) : -1;
  if (SETTINGS.useClock && SETTINGS.statusBarClock && HalClock::isSynced()) {
    const time_t now = HalClock::now();
    lastStatusBarClockMinute = now > 0 ? static_cast<int>(now / 60) : -1;
  } else {
    lastStatusBarClockMinute = -1;
  }
}

void EpubReaderActivity::renderBackgroundDebugOverlay() const {
#if DEBUG_BACKGROUND_WORK && DEBUG_BACKGROUND_OVERLAY
  // Background A state is latched per displayed page into backgroundAGlyph_ (see the
  // field comment), set just before renderStatusBar() draws it. The live scheduling
  // flags are cleared at the top of render() before this draws, so they always read
  // idle; latching at display time gives a glyph that is correct and visible:
  //   'x' hit  — this page was served from the Background-A pre-render buffer
  //   '-' miss — this page was rendered fresh (first page, heap-gated, or no pre-render)
  const char aGlyph = backgroundAGlyph_;

  // B is sampled from its state machine rather than the live percent alone: the overlay
  // only draws when a page renders, and B works precisely while the reader is idle — a
  // build usually finishes between two page turns, so a percent-only chip was almost
  // never visible (percent resets to -1 on completion). States:
  //   Bp      — probing whether the next section is already cached
  //   Bw      — waiting for the heap gates
  //   B<nn>%  — build in flight (percent of the chapter consumed)
  //   B+      — build complete, Section held for adoption on the next chapter cross
  //   B.      — settled with nothing held (already cached, refused, or failed)
  // Render task holds the render lock here and B mutates only under it, so the reads
  // are consistent.
  char bBuf[8];
  switch (backgroundBuildState_) {
    case BackgroundBuildState::Probe:
      snprintf(bBuf, sizeof(bBuf), "p");
      break;
    case BackgroundBuildState::WaitHeap:
      snprintf(bBuf, sizeof(bBuf), "w");
      break;
    case BackgroundBuildState::Building:
      snprintf(bBuf, sizeof(bBuf), "%d%%", backgroundBuildPercent_ >= 0 ? backgroundBuildPercent_ : 0);
      break;
    case BackgroundBuildState::Settled:
      snprintf(bBuf, sizeof(bBuf), "%s", backgroundSection_ ? "+" : ".");
      break;
  }

  // Build a compact "A<x|-> B<state>" string and draw it at the top-left of the content
  // area, over whatever the status bar drew. Intentionally crude — a diagnostic aid.
  char buf[24];
  snprintf(buf, sizeof(buf), "A%c B%s", aGlyph, bBuf);
  // Draw near the top-left corner; UI_10_FONT_ID is the small status font used elsewhere.
  renderer.drawText(UI_10_FONT_ID, 4, 2, buf, true, EpdFontFamily::BOLD);
#endif
}

bool EpubReaderActivity::shouldSkipPeriodicUpdate() const {
  if (lastStatusBarPage < 0) return false;  // no baseline yet — let the first render happen
  const int currentPage = section ? section->currentPage + 1 : -1;
  if (currentPage != lastStatusBarPage) return false;
  if (SETTINGS.statusBarBattery) {
    if (static_cast<int>(powerManager.getBatteryPercentage()) != lastStatusBarBattery) return false;
  }
  if (SETTINGS.useClock && SETTINGS.statusBarClock && HalClock::isSynced()) {
    const time_t now = HalClock::now();
    const int minute = now > 0 ? static_cast<int>(now / 60) : -1;
    if (minute != lastStatusBarClockMinute) return false;
  }
  return true;
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    navTarget = anchor.empty() ? NavigationTarget::makePage(0) : NavigationTarget::makeAnchor(std::move(anchor));
    currentSpineIndex = targetSpineIndex;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    navTarget = NavigationTarget::makePage(pos.pageNumber);
    section.reset();
  }
  requestUpdate();
}

bool EpubReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  // Resolve per-book overrides FIRST: every layout input below must be the EFFECTIVE value
  // (global setting resolved against the book override), mirroring the reader's
  // getEffective*() helpers. Passing raw SETTINGS values here diverges the section property
  // hash from the reader's whenever an override is set, and this path then silently rebuilds
  // a duplicate section variant during sleep preparation.
  const RecentBook currentBook = RECENT_BOOKS.getBookByPath(filePath);
  const bool effectiveEmbeddedStyle =
      currentBook.embeddedStyleOverride >= 0 ? currentBook.embeddedStyleOverride != 0 : SETTINGS.embeddedStyle != 0;
  const bool effectiveHyphenation =
      currentBook.hyphenationOverride >= 0 ? currentBook.hyphenationOverride != 0 : SETTINGS.hyphenationEnabled != 0;
  const bool effectiveBionicReading =
      currentBook.bionicReadingOverride >= 0 ? currentBook.bionicReadingOverride != 0 : SETTINGS.bionicReading != 0;
  const bool effectiveInlineFootnotePreviews = currentBook.inlineFootnotePreviewsOverride >= 0
                                                   ? currentBook.inlineFootnotePreviewsOverride != 0
                                                   : SETTINGS.inlineFootnotePreviews != 0;
  const uint8_t effectiveParagraphAlignment = currentBook.paragraphAlignmentOverride >= 0
                                                  ? static_cast<uint8_t>(currentBook.paragraphAlignmentOverride)
                                                  : SETTINGS.paragraphAlignment;
  const uint8_t effectiveImageRendering = currentBook.imageRenderingOverride >= 0
                                              ? static_cast<uint8_t>(currentBook.imageRenderingOverride)
                                              : SETTINGS.imageRendering;

  auto epub = std::make_shared<Epub>(filePath, "/.crosspoint");
  // Load CSS when embeddedStyle is effectively enabled, as createSectionFile may need it to
  // rebuild the cache.
  if (!epub->load(true, !effectiveEmbeddedStyle)) {
    LOG_DBG("SLP", "EPUB: failed to load %s", filePath.c_str());
    return false;
  }

  epub->setupCacheDir();

  // Load saved spine index and page number
  int spineIndex = 0, pageNumber = 0;
  FsFile f;
  if (Storage.openFileForRead("SLP", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    if (f.read(data, 6) >= 4) {
      spineIndex = (int)((uint32_t)data[0] | ((uint32_t)data[1] << 8));
      pageNumber = (int)((uint32_t)data[2] | ((uint32_t)data[3] << 8));
    }
    f.close();
  }
  if (spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) spineIndex = 0;

  // Apply the reader orientation so margins match what the reader would produce
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Compute margins exactly as render() does
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += std::max(static_cast<int>(SETTINGS.screenMargin), UITheme::getStatusBarTopHeight());
  marginLeft += SETTINGS.screenMargin;
  marginRight += SETTINGS.screenMargin;
  marginBottom += std::max(static_cast<int>(SETTINGS.screenMargin), UITheme::getStatusBarBottomHeight());

  const uint16_t viewportWidth = renderer.getScreenWidth() - marginLeft - marginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - marginTop - marginBottom;

  // Load or rebuild the section cache. Rebuilding is needed when the cache is missing or stale
  // (e.g. after a firmware update). A no-op popup callback avoids any UI during sleep preparation.
  const bool hasLocalSdOverride = !currentBook.sdFontFamilyOverride.empty();
  const uint8_t effectiveFontFamily =
      currentBook.fontFamilyOverride >= 0 ? static_cast<uint8_t>(currentBook.fontFamilyOverride) : SETTINGS.fontFamily;
  const uint8_t effectiveFontSize =
      currentBook.fontSizeOverride >= 0 ? static_cast<uint8_t>(currentBook.fontSizeOverride) : SETTINGS.fontSize;
  int effectiveFontId = 0;
  if (hasLocalSdOverride) {
    effectiveFontId = resolveSdCardFontId(currentBook.sdFontFamilyOverride.c_str(), effectiveFontSize);
  }
  if (effectiveFontId == 0 && currentBook.fontFamilyOverride >= 0) {
    effectiveFontId = CrossPointSettings::getBuiltinReaderFontId(effectiveFontFamily, effectiveFontSize);
  }
  if (effectiveFontId == 0 && currentBook.fontSizeOverride >= 0 && SETTINGS.sdFontFamilyName[0] != '\0') {
    effectiveFontId = resolveSdCardFontId(SETTINGS.sdFontFamilyName, effectiveFontSize);
  }
  if (effectiveFontId == 0 && currentBook.fontSizeOverride >= 0) {
    effectiveFontId = CrossPointSettings::getBuiltinReaderFontId(SETTINGS.fontFamily, effectiveFontSize);
  }
  if (effectiveFontId == 0) {
    effectiveFontId = SETTINGS.getReaderFontId();
  }
  const auto getEffectiveLineCompression = [&](int fontId) {
    const int notosansId = CrossPointSettings::getBuiltinReaderFontId(CrossPointSettings::NOTOSANS, effectiveFontSize);

    if (fontId == notosansId) {
      switch (SETTINGS.lineSpacing) {
        case CrossPointSettings::TIGHT:
          return 0.90f;
        case CrossPointSettings::NORMAL:
        default:
          return 0.95f;
        case CrossPointSettings::WIDE:
          return 1.0f;
      }
    }

    switch (SETTINGS.lineSpacing) {
      case CrossPointSettings::TIGHT:
        return 0.95f;
      case CrossPointSettings::NORMAL:
      default:
        return 1.0f;
      case CrossPointSettings::WIDE:
        return 1.1f;
    }
  };

  const float effectiveLineCompression = getEffectiveLineCompression(effectiveFontId);
  auto section = std::make_unique<Section>(epub, spineIndex, renderer);
  if (!section->loadSectionFile(effectiveFontId, effectiveLineCompression, SETTINGS.extraParagraphSpacing,
                                effectiveParagraphAlignment, viewportWidth, viewportHeight, effectiveHyphenation,
                                effectiveEmbeddedStyle, effectiveBionicReading, effectiveInlineFootnotePreviews,
                                effectiveImageRendering)) {
    LOG_DBG("SLP", "EPUB: section cache not found for spine %d, rebuilding", spineIndex);
    if (!section->createSectionFile(effectiveFontId, effectiveLineCompression, SETTINGS.extraParagraphSpacing,
                                    effectiveParagraphAlignment, viewportWidth, viewportHeight, effectiveHyphenation,
                                    effectiveEmbeddedStyle, effectiveBionicReading, effectiveInlineFootnotePreviews,
                                    effectiveImageRendering, nullptr,
                                    /*skipEviction=*/false, buildReaderFontSizeLadder(effectiveFontId))) {
      LOG_ERR("SLP", "EPUB: failed to rebuild section cache for spine %d", spineIndex);
      return false;
    }
  }

  if (pageNumber < 0 || pageNumber >= section->pageCount) pageNumber = 0;
  section->currentPage = pageNumber;

  // During an active build the on-disk LUT is not yet written; load from the in-memory LUT.
  auto page = section->hasActiveBuild() ? section->loadPageFromActiveBuild(static_cast<uint16_t>(pageNumber))
                                        : section->loadPageFromSectionFile();
  if (!page) {
    LOG_DBG("SLP", "EPUB: failed to load page %d", pageNumber);
    return false;
  }

  const int imageOnlyOffset = getImageOnlyPageYOffset(*page, viewportHeight);
  const int renderMarginTop = marginTop + imageOnlyOffset;
  renderer.clearScreen();
  page->render(renderer, effectiveFontId, marginLeft, renderMarginTop);
  // No displayBuffer call — caller (SleepActivity) handles that after compositing the overlay
  return true;
}

void EpubReaderActivity::openQuickOverrides() {
  ReaderUtils::enforceExitFullRefresh(renderer);
  startActivityForResult(std::make_unique<QuickOverridesActivity>(
                             renderer, mappedInput, bookEmbeddedStyleOverride, bookImageRenderingOverride,
                             bookFontFamilyOverride, bookSdFontFamilyOverride, bookFontSizeOverride,
                             bookBionicReadingOverride, bookGuideDotsOverride, bookParagraphAlignmentOverride,
                             bookTextAntiAliasingOverride, bookHyphenationOverride, bookInlineFootnotePreviewsOverride),
                         [this](const ActivityResult& result) {
                           const auto& menu = std::get<MenuResult>(result.data);
                           applyBookReaderOverrides(
                               menu.embeddedStyleOverride, menu.imageRenderingOverride, menu.fontFamilyOverride,
                               menu.sdFontFamilyOverride, menu.fontSizeOverride,
                               static_cast<int8_t>(menu.bionicReadingOverride), menu.paragraphAlignmentOverride,
                               menu.textAntiAliasingOverride, menu.hyphenationOverride, menu.guideDotsOverride,
                               menu.inlineFootnotePreviewsOverride);
                         });
}

void EpubReaderActivity::openReaderMenu() {
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->pageCount : 0;

  if (!epub) {
    return;
  }

  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
    const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  const bool isCurrentPageStarred = section && bookmarkStore.has(static_cast<uint16_t>(currentSpineIndex),
                                                                 static_cast<uint16_t>(section->currentPage));

  // Show the "Go to printed page" item only when this book has at least one integer-labelled
  // entry in pagelist.bin. Roman-only or empty page lists are excluded — the numeric input
  // dialog can't address them anyway. Streamed (not loadPrintedPageList) so opening the menu never
  // reserves the whole list: that ~200 KB contiguous allocation aborts the firmware via uncaught
  // bad_alloc when the menu is opened mid section-build on a long book (heap fragmented to tens of
  // KB) — the tag 2.05 "Confirm reboots" crash.
  const bool hasPrintedPages = epub->hasNumericPrintedPages();

  ReaderUtils::enforceExitFullRefresh(renderer);
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(
          renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent, SETTINGS.orientation,
          !currentPageFootnotes.empty(), bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
          bookSdFontFamilyOverride, bookFontSizeOverride, SETTINGS.textDarkness, getEffectiveBionicReading(),
          bookGuideDotsOverride, bookParagraphAlignmentOverride, bookTextAntiAliasingOverride, bookHyphenationOverride,
          bookInlineFootnotePreviewsOverride, !bookmarkStore.isEmpty(), isCurrentPageStarred, hasPrintedPages),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);
        applyOrientation(menu.orientation);
        applyTextDarkness(menu.textDarkness);
        toggleAutoPageTurn(menu.pageTurnOption);
        applyBookReaderOverrides(menu.embeddedStyleOverride, menu.imageRenderingOverride, menu.fontFamilyOverride,
                                 menu.sdFontFamilyOverride, menu.fontSizeOverride,
                                 static_cast<bool>(menu.bionicReadingOverride), menu.paragraphAlignmentOverride,
                                 menu.textAntiAliasingOverride, menu.hyphenationOverride, menu.guideDotsOverride,
                                 menu.inlineFootnotePreviewsOverride);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }
      });
}

void EpubReaderActivity::onButtonAction(const CrossPointSettings::BUTTON_ACTION action) {
  using BA = CrossPointSettings::BUTTON_ACTION;
  switch (action) {
    case BA::BTN_PAGE_FORWARD:
      pageTurn(true);
      break;
    case BA::BTN_PAGE_BACK:
      pageTurn(false);
      break;
    case BA::BTN_PAGE_FORWARD_10:
      for (int i = 0; i < 10; i++) {
        if (!stepPageState(true)) break;
      }
      requestUpdate();
      break;
    case BA::BTN_PAGE_BACK_10:
      for (int i = 0; i < 10; i++) {
        if (!stepPageState(false)) break;
      }
      requestUpdate();
      break;
    case BA::BTN_STAR_PAGE:
      if (section) {
        bookmarkStore.toggle(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(section->currentPage));
        requestUpdate();
      }
      break;
    case BA::BTN_FOOTNOTES:
      if (!currentPageFootnotes.empty()) {
        if (currentPageFootnotes.size() == 1) {
          navigateToHref(currentPageFootnotes[0].href, true);
        } else {
          startActivityForResult(
              std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
              [this](const ActivityResult& result) {
                if (!result.isCancelled) {
                  const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                  navigateToHref(footnoteResult.href, true);
                }
              });
        }
      }
      break;
    case BA::BTN_OPEN_TOC:
      if (epub) {
        const int spineIdx = currentSpineIndex;
        const int tocIdx = section ? section->getTocIndexForPage(section->currentPage)
                                   : epub->getTocIndexForSpineIndex(currentSpineIndex);
        ReaderUtils::enforceExitFullRefresh(renderer);
        startActivityForResult(std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub,
                                                                                    epub->getPath(), spineIdx, tocIdx),
                               [this](const ActivityResult& result) {
                                 if (result.isCancelled) return;
                                 // See the matching comment in onReaderMenuConfirm's SELECT_CHAPTER
                                 // case: the override armed before launch was already consumed by
                                 // the chapter list's own paint, so arm a fresh one for the resumed page.
                                 ReaderUtils::enforceExitFullRefresh(renderer);
                                 RenderLock lock(*this);
                                 const auto& chapter = std::get<ChapterResult>(result.data);
                                 auto resolvedPage =
                                     (chapter.tocIndex && chapter.spineIndex == currentSpineIndex && section)
                                         ? section->getPageForTocIndex(*chapter.tocIndex)
                                         : std::nullopt;
                                 if (resolvedPage) {
                                   section->currentPage = *resolvedPage;
                                   forceLoadLargeImages = false;
                                   pageHasPlaceholders = false;
                                 } else {
                                   navTarget = chapter.tocIndex ? NavigationTarget::makeTocIndex(*chapter.tocIndex)
                                                                : NavigationTarget::makePage(0);
                                   currentSpineIndex = chapter.spineIndex;
                                   section.reset();
                                 }
                               });
      }
      break;
    case BA::BTN_NEXT_SECTION:
    case BA::BTN_PREV_SECTION: {
      const bool forward = (action == BA::BTN_NEXT_SECTION);
      // Deliberate chapter jump: arm a HALF refresh for the next displayed screen, exactly like the
      // other deliberate jumps (percent / TOC / footnote / reader exit). The indexing popup consumes
      // this override so hasRefreshOverridePending() reads true there, which arms
      // forceHalfRefreshAfterPopup_; the first page of the target chapter then paints HALF instead of
      // a FAST differential. Without it, the dramatic previous-chapter -> new-chapter transition
      // under-drives on X4 and the previous chapter's text ghosts through the new page.
      ReaderUtils::enforceExitFullRefresh(renderer);
      {
        RenderLock lock(*this);
        if (section && section->pageCount > 0) {
          const int curTocIndex = section->getTocIndexForPage(section->currentPage);
          const int nextTocIndex = forward ? curTocIndex + 1 : curTocIndex - 1;
          if (curTocIndex < 0) {
            navTarget = NavigationTarget::makePage(0);
            currentSpineIndex = forward ? currentSpineIndex + 1 : currentSpineIndex - 1;
            section.reset();
          } else if (nextTocIndex >= 0 && nextTocIndex < epub->getTocItemsCount()) {
            const int newSpineIndex = epub->getSpineIndexForTocIndex(nextTocIndex);
            if (newSpineIndex == currentSpineIndex) {
              if (const auto resolvedPage = section->getPageForTocIndex(nextTocIndex)) {
                section->currentPage = *resolvedPage;
                forceLoadLargeImages = false;
                pageHasPlaceholders = false;
              }
            } else {
              navTarget = NavigationTarget::makeTocIndex(nextTocIndex);
              currentSpineIndex = newSpineIndex;
              section.reset();
            }
          } else if (forward) {
            navTarget = NavigationTarget::makePage(0);
            currentSpineIndex = epub->getSpineItemsCount();
            section.reset();
          } else {
            navTarget = NavigationTarget::makePage(0);
            currentSpineIndex = epub->getTocItem(curTocIndex).spineIndex - 1;
            section.reset();
          }
        } else {
          navTarget = NavigationTarget::makePage(0);
          currentSpineIndex = forward ? currentSpineIndex + 1 : currentSpineIndex - 1;
          section.reset();
        }
      }
      requestUpdate();
      break;
    }
    case BA::BTN_EXIT_READER:
      ReaderUtils::enforceExitFullRefresh(renderer);
      if (tryAutoPushOnClose()) break;
      finish();
      break;
    case BA::BTN_READER_MENU:
      if (epub) {
        openReaderMenu();
      }
      break;
    case BA::BTN_TOGGLE_BIONIC_READING:
      if (epub) {
        applyBookReaderOverrides(bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
                                 bookSdFontFamilyOverride, bookFontSizeOverride, !getEffectiveBionicReading(),
                                 bookParagraphAlignmentOverride);
        requestUpdate();
      }
      break;
    case BA::BTN_CYCLE_FONT_SIZE:
      if (epub) {
        const uint8_t current =
            (bookFontSizeOverride >= 0) ? static_cast<uint8_t>(bookFontSizeOverride) : SETTINGS.fontSize;
        const int8_t next = static_cast<int8_t>((current + 1) % CrossPointSettings::FONT_SIZE_COUNT);
        applyBookReaderOverrides(bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
                                 bookSdFontFamilyOverride, next, bookBionicReadingOverride,
                                 bookParagraphAlignmentOverride);
        requestUpdate();
      }
      break;
    case BA::BTN_CYCLE_ORIENTATION:
      if (epub) {
        const uint8_t nextOrientation =
            static_cast<uint8_t>((SETTINGS.orientation + 1) % CrossPointSettings::ORIENTATION_COUNT);
        applyOrientation(nextOrientation);
        requestUpdate();
      }
      break;
    case BA::BTN_KOREADER_SYNC:
      launchKOReaderSync(SyncLaunchMode::COMPARE);
      break;
    case BA::BTN_QUICK_OVERRIDES:
      if (epub) {
        openQuickOverrides();
      }
      break;
    case BA::BTN_FORCE_REFRESH:
    case BA::BTN_FORCE_FAST_REFRESH:
      // Re-display the CURRENT page to clear ghosting — do NOT raw displayBuffer() (the
      // framebuffer may hold a Background-A pre-render of the *next* page, which would look
      // like a page turn). Clear the pre-render flags so classifyRenderPass() picks a Normal
      // render of the current page, request the forced mode for that render, and re-render.
      pendingPreRender = false;
      usePreRenderedBuffer = false;
      preRenderedPage.ready = false;
      forceRefreshModeNextRender_ = static_cast<int8_t>(
          action == BA::BTN_FORCE_FAST_REFRESH ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH);
      requestUpdate();
      break;
    default:
      break;
  }
}
