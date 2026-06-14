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
#include "EpubRenderBenchmarkActivity.h"
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
#include "activities/settings/ReadingStatsBookDetailActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;

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
// Per-slice time budget for a Background-B parse step. Conservative start (handoff plan
// suggests 30–50 ms); tune from the DEBUG_BACKGROUND_WORK serial counters.
constexpr uint32_t BG_BUILD_BUDGET_MS = 40;

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

// Writes the canonical EPUB progress.bin layout: spine(2) + page(2) + pageCount(2) + percent(1).
// Used by the per-page saveProgress() and by transient writers (sync restore, bookmark jump) so
// the on-disk format stays consistent regardless of caller.
bool writeReaderProgressCache(const std::string& cachePath, const int spineIndex, const int currentPage,
                              const int pageCount, const uint8_t percent) {
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

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

const char* orientationToString(const GfxRenderer::Orientation orientation) {
  switch (orientation) {
    case GfxRenderer::Portrait:
      return "Portrait";
    case GfxRenderer::LandscapeClockwise:
      return "Landscape CW";
    case GfxRenderer::PortraitInverted:
      return "Portrait Inverted";
    case GfxRenderer::LandscapeCounterClockwise:
      return "Landscape CCW";
  }
  return "Unknown";
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
    // Building the image manifest scans the whole ZIP (~1s+); show a popup when it's
    // a cache miss. Warm re-opens load images.bin instantly and skip the flash.
    if (epub->needsImageManifestBuild()) {
      RenderLock lock;
      GUI.drawPopup(renderer, tr(STR_INDEXING));
    }
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
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      navTarget = NavigationTarget::makePage(data[2] + (data[3] << 8));
      navTarget.cachedSpineIdx = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, navTarget.page);
    }
    if (dataSize == 6) {
      navTarget.cachedPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex < 0 || currentSpineIndex >= epub->getSpineItemsCount()) {
    LOG_ERR("ERS", "Invalid saved spine index %d (valid 0..%d), resetting to start", currentSpineIndex,
            epub->getSpineItemsCount() > 0 ? epub->getSpineItemsCount() - 1 : 0);
    currentSpineIndex = 0;
    navTarget = NavigationTarget::makePage(0);
  }

  applyPendingSyncSession();
  applyPendingBookmarkJump();
  logReaderMemSnapshot("onEnter_after_pending_sync");

  if (currentSpineIndex == 0) {
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
  const std::string epubSidecar = ReaderActivity::sidecarCoverPath(epub->getPath());
  const std::string epubCover = epubSidecar.empty() ? epub->getThumbBmpPath() : epubSidecar;
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), series, epubCover);
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
  section.reset();
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
                                           epub->getSeriesIndex());
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
    return;  // AA still owed (display bus busy); it keeps priority over B
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
  p.imageRendering = lastRenderStats.imageRendering;
  p.headingFonts = buildHeadingFonts();
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

  const int targetSpine = currentSpineIndex + 1;
  if (targetSpine >= epub->getSpineItemsCount()) {
    resetBackgroundBuild();
    return;
  }
  // Navigation moved the target since the last tick: held state is for the wrong section.
  if (backgroundBuildSpineIndex_ != targetSpine) {
    resetBackgroundBuild();
    backgroundBuildSpineIndex_ = targetSpine;
  }

  switch (backgroundBuildState_) {
    case BackgroundBuildState::Settled:
      return;

    case BackgroundBuildState::Probe: {
      // One SD probe per target: if the exact cache variant already exists there is
      // nothing to pre-build.
      backgroundSection_ = std::make_unique<Section>(epub, targetSpine, renderer);
      const Section::BuildParams p = makeSectionBuildParams();
      const bool cached = backgroundSection_->loadSectionFile(
          p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment, p.viewportWidth, p.viewportHeight,
          p.hyphenationEnabled, p.embeddedStyle, p.bionicReadingEnabled, p.imageRendering);
      if (cached && !backgroundSection_->isEmbeddedStyleFallback()) {
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
          LOG_INF("ERS", "Background build spine=%d complete: %u pages", targetSpine, backgroundSection_->pageCount);
        }
      } else {
        LOG_ERR("ERS", "Background build spine=%d failed", targetSpine);
        backgroundSection_.reset();
      }
      backgroundBuildState_ = BackgroundBuildState::Settled;
      return;
    }
  }
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
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
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
      auto entries = epub->loadPrintedPageList();
      // Compute the integer label range from parseable entries; non-integer labels are
      // ignored (the dialog is numeric-only).
      int minLabel = std::numeric_limits<int>::max();
      int maxLabel = std::numeric_limits<int>::min();
      for (const auto& entry : entries) {
        if (const auto n = parsePrintedPageLabel(entry.label)) {
          if (*n < minLabel) minLabel = *n;
          if (*n > maxLabel) maxLabel = *n;
        }
      }
      if (maxLabel < minLabel) break;  // no integer labels — shouldn't happen if menu item was shown

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
          [this, entries = std::move(entries)](const ActivityResult& result) {
            if (result.isCancelled) return;
            const auto& pick = std::get<PrintedPageResult>(result.data);
            // Resolve the typed label back to a (href, anchor) by linear scan. Entries are
            // small (typically <500 even for long books) and this fires once per user action.
            for (const auto& entry : entries) {
              const auto entryLabelValue = parsePrintedPageLabel(entry.label);
              const auto pickLabelValue = parsePrintedPageLabel(pick.label);
              if (entry.label == pick.label ||
                  (entryLabelValue && pickLabelValue && *entryLabelValue == *pickLabelValue)) {
                const int spineIdx = epub->resolveHrefToSpineIndex(entry.href);
                if (spineIdx < 0) {
                  LOG_DBG("ERS", "printed-page jump: could not resolve spine for href=%s", entry.href.c_str());
                  return;
                }
                {
                  RenderLock lock(*this);
                  currentSpineIndex = spineIdx;
                  navTarget =
                      entry.anchor.empty() ? NavigationTarget::makePage(0) : NavigationTarget::makeAnchor(entry.anchor);
                  section.reset();
                }
                requestUpdate();
                return;
              }
            }
            LOG_DBG("ERS", "printed-page jump: label '%s' not found in pagelist", pick.label.c_str());
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
                const auto& words = line.getBlock()->getWords();
                for (const auto& w : words) {
                  if (!fullText.empty()) fullText += " ";
                  fullText += w;
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
                                           epub->getSeriesIndex());
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
      runRenderBenchmark();
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

void EpubReaderActivity::runRenderBenchmark() {
  if (!epub) {
    return;
  }

  if (!section) {
    requestUpdateAndWait();
    if (!section) {
      return;
    }
  }

  const LastRenderStats startSnapshot = lastRenderStats;
  BenchmarkAggregate aggregate;
  auto recordRender = [&aggregate](const LastRenderStats& snapshot) {
    if (!snapshot.valid) {
      return;
    }

    aggregate.renderCount++;
    aggregate.imagePageCount += snapshot.hadImages ? 1 : 0;
    aggregate.cacheRebuildCount += snapshot.cacheRebuilt ? 1 : 0;
    if (snapshot.footnoteCount > aggregate.maxFootnotes) {
      aggregate.maxFootnotes = snapshot.footnoteCount;
    }

    aggregate.totalRequestRenderMs += snapshot.requestRenderMs;
    if (aggregate.renderCount == 1 || snapshot.requestRenderMs < aggregate.minRequestRenderMs) {
      aggregate.minRequestRenderMs = snapshot.requestRenderMs;
    }
    if (snapshot.requestRenderMs > aggregate.maxRequestRenderMs) {
      aggregate.maxRequestRenderMs = snapshot.requestRenderMs;
    }

    aggregate.totalRenderMs += snapshot.phases.totalMs;
    if (aggregate.renderCount == 1 || snapshot.phases.totalMs < aggregate.minRenderMs) {
      aggregate.minRenderMs = snapshot.phases.totalMs;
    }
    if (snapshot.phases.totalMs > aggregate.maxRenderMs) {
      aggregate.maxRenderMs = snapshot.phases.totalMs;
    }

    aggregate.totalSectionLoadMs += snapshot.sectionLoadMs;
    aggregate.totalPageLoadMs += snapshot.pageLoadMs;
    aggregate.totalPhases.prewarmMs += snapshot.phases.prewarmMs;
    aggregate.totalPhases.bwRenderMs += snapshot.phases.bwRenderMs;
    aggregate.totalPhases.displayMs += snapshot.phases.displayMs;
    aggregate.totalPhases.bwStoreMs += snapshot.phases.bwStoreMs;
    aggregate.totalPhases.grayLsbMs += snapshot.phases.grayLsbMs;
    aggregate.totalPhases.grayMsbMs += snapshot.phases.grayMsbMs;
    aggregate.totalPhases.grayDisplayMs += snapshot.phases.grayDisplayMs;
    aggregate.totalPhases.bwRestoreMs += snapshot.phases.bwRestoreMs;
    aggregate.totalPhases.totalMs += snapshot.phases.totalMs;

    aggregate.totalFontCacheHits += snapshot.fontCacheHits;
    aggregate.totalFontCacheMisses += snapshot.fontCacheMisses;
    aggregate.totalFontDecompressMs += snapshot.fontDecompressMs;
    aggregate.totalFontGetBitmapTimeUs += snapshot.fontGetBitmapTimeUs;
    aggregate.totalFontGetBitmapCalls += snapshot.fontGetBitmapCalls;

    if (aggregate.renderCount == 1 || snapshot.freeHeapAfter < aggregate.minFreeHeapAfter) {
      aggregate.minFreeHeapAfter = snapshot.freeHeapAfter;
    }
    if (snapshot.freeHeapAfter > aggregate.maxFreeHeapAfter) {
      aggregate.maxFreeHeapAfter = snapshot.freeHeapAfter;
    }
  };
  const unsigned long startTime = millis();
  int forwardTurns = 0;
  int backwardTurns = 0;

  for (int i = 0; i < 10; i++) {
    if (!stepPageState(true)) {
      break;
    }
    requestUpdateAndWait();
    recordRender(lastRenderStats);
    forwardTurns++;
  }

  const unsigned long forwardMs = millis() - startTime;
  const unsigned long backwardStart = millis();

  for (int i = 0; i < 10; i++) {
    if (!stepPageState(false)) {
      break;
    }
    requestUpdateAndWait();
    recordRender(lastRenderStats);
    backwardTurns++;
  }

  const unsigned long backwardMs = millis() - backwardStart;

  startActivityForResult(
      std::make_unique<EpubRenderBenchmarkActivity>(
          renderer, mappedInput,
          buildRenderBenchmarkReport(startSnapshot, aggregate, forwardTurns, forwardMs, backwardTurns, backwardMs)),
      [this](const ActivityResult&) { requestUpdate(); });
}

std::string EpubReaderActivity::buildRenderBenchmarkReport(const LastRenderStats& startSnapshot,
                                                           const BenchmarkAggregate& aggregate, const int forwardTurns,
                                                           const unsigned long forwardMs, const int backwardTurns,
                                                           const unsigned long backwardMs) const {
  const LastRenderStats& endSnapshot = lastRenderStats.valid ? lastRenderStats : startSnapshot;

  std::string report;
  report.reserve(768);

  auto appendLine = [&report](const std::string& line) {
    if (!report.empty()) {
      report += '\n';
    }
    report += line;
  };

  appendLine("Forward 10: " + std::to_string(forwardTurns) + " turns in " + std::to_string(forwardMs) + " ms");
  if (forwardTurns > 0) {
    appendLine("Forward avg: " + std::to_string(forwardMs / static_cast<unsigned long>(forwardTurns)) + " ms/turn");
  }
  appendLine("Backward 10: " + std::to_string(backwardTurns) + " turns in " + std::to_string(backwardMs) + " ms");
  if (backwardTurns > 0) {
    appendLine("Backward avg: " + std::to_string(backwardMs / static_cast<unsigned long>(backwardTurns)) + " ms/turn");
  }
  appendLine("Measured renders: " + std::to_string(aggregate.renderCount) + ", image pages " +
             std::to_string(aggregate.imagePageCount) + ", cache rebuilds " +
             std::to_string(aggregate.cacheRebuildCount));

  appendLine("Start: spine " + std::to_string(startSnapshot.spineIndex) + ", page " +
             std::to_string(startSnapshot.pageIndex + 1) + "/" + std::to_string(startSnapshot.pageCount));
  appendLine("End: spine " + std::to_string(endSnapshot.spineIndex) + ", page " +
             std::to_string(endSnapshot.pageIndex + 1) + "/" + std::to_string(endSnapshot.pageCount));
  appendLine("Orientation: " +
             std::string(orientationToString(static_cast<GfxRenderer::Orientation>(endSnapshot.orientation))));
  appendLine("Viewport: " + std::to_string(endSnapshot.viewportWidth) + "x" +
             std::to_string(endSnapshot.viewportHeight) + " px, margins T/R/B/L " +
             std::to_string(endSnapshot.marginTop) + "/" + std::to_string(endSnapshot.marginRight) + "/" +
             std::to_string(endSnapshot.marginBottom) + "/" + std::to_string(endSnapshot.marginLeft));
  appendLine("Font: " + std::to_string(endSnapshot.effectiveFontId) + ", embedded CSS " +
             std::string(endSnapshot.embeddedStyle ? "on" : "off") + ", images " +
             std::to_string(endSnapshot.imageRendering) + ", AA " +
             std::string(endSnapshot.textAntiAliasing ? "on" : "off"));
  appendLine("Last page: images " + std::string(endSnapshot.hadImages ? "yes" : "no") + ", footnotes " +
             std::to_string(endSnapshot.footnoteCount) + ", cache rebuilt " +
             std::string(endSnapshot.cacheRebuilt ? "yes" : "no"));
  if (aggregate.renderCount > 0) {
    appendLine("Render avg/min/max: request " +
               std::to_string(aggregate.totalRequestRenderMs / static_cast<unsigned long>(aggregate.renderCount)) +
               "/" + std::to_string(aggregate.minRequestRenderMs) + "/" + std::to_string(aggregate.maxRequestRenderMs) +
               " ms, core " +
               std::to_string(aggregate.totalRenderMs / static_cast<unsigned long>(aggregate.renderCount)) + "/" +
               std::to_string(aggregate.minRenderMs) + "/" + std::to_string(aggregate.maxRenderMs) + " ms");
    appendLine("Aggregate loads: section " + std::to_string(aggregate.totalSectionLoadMs) + " ms, page " +
               std::to_string(aggregate.totalPageLoadMs) + " ms, max footnotes " +
               std::to_string(aggregate.maxFootnotes));
    appendLine("Aggregate phases: prewarm " + std::to_string(aggregate.totalPhases.prewarmMs) + ", bw " +
               std::to_string(aggregate.totalPhases.bwRenderMs) + ", display " +
               std::to_string(aggregate.totalPhases.displayMs) + ", planes " +
               std::to_string(aggregate.totalPhases.grayLsbMs) + ", gray display " +
               std::to_string(aggregate.totalPhases.grayDisplayMs) + ", restore " +
               std::to_string(aggregate.totalPhases.bwRestoreMs));
    appendLine("Aggregate font: hits " + std::to_string(aggregate.totalFontCacheHits) + ", misses " +
               std::to_string(aggregate.totalFontCacheMisses) + ", decompress " +
               std::to_string(aggregate.totalFontDecompressMs) + " ms");
    appendLine("Aggregate glyph lookups: " + std::to_string(aggregate.totalFontGetBitmapCalls) + " calls, " +
               std::to_string(aggregate.totalFontGetBitmapTimeUs) + " us total");
    appendLine("Heap after render min/max: " + std::to_string(aggregate.minFreeHeapAfter) + "/" +
               std::to_string(aggregate.maxFreeHeapAfter));
  }
  appendLine("Last render: request " + std::to_string(endSnapshot.requestRenderMs) + " ms, section load " +
             std::to_string(endSnapshot.sectionLoadMs) + " ms, page load " + std::to_string(endSnapshot.pageLoadMs) +
             " ms, render total " + std::to_string(endSnapshot.phases.totalMs) + " ms");
  appendLine("Phases: prewarm " + std::to_string(endSnapshot.phases.prewarmMs) + ", bw " +
             std::to_string(endSnapshot.phases.bwRenderMs) + ", display " +
             std::to_string(endSnapshot.phases.displayMs) + ", planes " + std::to_string(endSnapshot.phases.grayLsbMs) +
             ", gray display " + std::to_string(endSnapshot.phases.grayDisplayMs) + ", restore " +
             std::to_string(endSnapshot.phases.bwRestoreMs));
  appendLine("Font cache: hits " + std::to_string(endSnapshot.fontCacheHits) + ", misses " +
             std::to_string(endSnapshot.fontCacheMisses) + ", decompress " +
             std::to_string(endSnapshot.fontDecompressMs) + " ms, groups " +
             std::to_string(endSnapshot.fontUniqueGroups));
  appendLine("Font buffers: page " + std::to_string(endSnapshot.fontPageBufferBytes) + ", glyph table " +
             std::to_string(endSnapshot.fontPageGlyphsBytes) + ", peak temp " +
             std::to_string(endSnapshot.fontPeakTempBytes));
  appendLine("Glyph lookups: " + std::to_string(endSnapshot.fontGetBitmapCalls) + " calls, " +
             std::to_string(endSnapshot.fontGetBitmapTimeUs) + " us total");
  appendLine("Heap: before " + std::to_string(endSnapshot.freeHeapBefore) + "/" +
             std::to_string(endSnapshot.largestFreeBlockBefore) + ", after " +
             std::to_string(endSnapshot.freeHeapAfter) + "/" + std::to_string(endSnapshot.largestFreeBlockAfter));

  return report;
}

void EpubReaderActivity::launchKOReaderSync(const SyncLaunchMode mode) {
  if (!epub) {
    return;
  }

  const int currentPage = section ? section->currentPage : 0;
  const int totalPages = section ? section->pageCount : 0;
  KOReaderSyncIntentState syncIntent = KOReaderSyncIntentState::COMPARE;
  if (mode == SyncLaunchMode::PULL_REMOTE) {
    syncIntent = KOReaderSyncIntentState::PULL_REMOTE;
  } else if (mode == SyncLaunchMode::PUSH_LOCAL) {
    syncIntent = KOReaderSyncIntentState::PUSH_LOCAL;
  } else if (mode == SyncLaunchMode::AUTO_PUSH) {
    syncIntent = KOReaderSyncIntentState::AUTO_PUSH;
  }

  auto& sync = APP_STATE.koReaderSyncSession;
  sync.active = true;
  sync.epubPath = epub->getPath();
  sync.spineIndex = currentSpineIndex;
  sync.page = currentPage;
  sync.totalPagesInSpine = totalPages;
  // Populate paragraph index and XHTML seek hint from section LUT if available.
  if (section) {
    if (const auto pIdx = section->getParagraphIndexForPage(static_cast<uint16_t>(currentPage))) {
      sync.paragraphIndex = *pIdx;
      sync.hasParagraphIndex = true;
      if (const auto hint = section->getXhtmlByteOffsetForPage(static_cast<uint16_t>(currentPage))) {
        sync.xhtmlSeekHint = *hint;
      } else {
        sync.xhtmlSeekHint = 0;
      }
    } else {
      sync.paragraphIndex = 0;
      sync.hasParagraphIndex = false;
      sync.xhtmlSeekHint = 0;
    }
  } else {
    sync.paragraphIndex = 0;
    sync.hasParagraphIndex = false;
    sync.xhtmlSeekHint = 0;
  }
  sync.intent = syncIntent;
  sync.outcome = KOReaderSyncOutcomeState::PENDING;
  sync.resultSpineIndex = 0;
  sync.resultPage = 0;
  sync.resultParagraphIndex = 0;
  sync.resultHasParagraphIndex = false;
  // Only auto-push-on-close should bypass the reader on resume; explicit syncs from the
  // reader menu always come back to the reader. Reset here so a stale flag from a prior
  // run cannot steal the user back to home.
  sync.exitToHomeAfterSync = (mode == SyncLaunchMode::AUTO_PUSH);
  APP_STATE.saveToFile();

  LOG_DBG("ERS", "Standalone sync handoff: spine=%d page=%d/%d", currentSpineIndex, currentPage, totalPages);
  logReaderMemSnapshot("before_replace_with_sync");
  activityManager.goToKOReaderSync();
}

bool EpubReaderActivity::tryAutoPushOnClose() {
  // Three-page minimum filters out brief inspections — opening to check the cover or
  // skim the TOC shouldn't burn a network round-trip. Counter is per-activity-instance.
  constexpr int MIN_SESSION_PAGES = 3;
  if (!SETTINGS.koSyncOnBookClose) {
    return false;
  }
  if (!KOREADER_STORE.hasCredentials()) {
    return false;
  }
  if (sessionPagesAdvanced < MIN_SESSION_PAGES) {
    return false;
  }
  if (!epub) {
    return false;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0 || currentSpineIndex >= spineCount || !section) {
    LOG_DBG("ERS", "Skipping AUTO_PUSH on end-of-book sentinel: spine=%d section=%s", currentSpineIndex,
            section ? "present" : "null");
    return false;
  }

  // exitToHomeAfterSync flag is set inside launchKOReaderSync for AUTO_PUSH mode.
  launchKOReaderSync(SyncLaunchMode::AUTO_PUSH);
  return true;
}

void EpubReaderActivity::applyPendingSyncSession() {
  auto& sync = APP_STATE.koReaderSyncSession;
  if (!sync.active || !epub || sync.epubPath != epub->getPath()) {
    return;
  }

  LOG_DBG("ERS", "Applying pending sync session outcome=%d path=%s", static_cast<int>(sync.outcome),
          sync.epubPath.c_str());

  // Upload-complete returns to the same local position the reader already persisted
  // before sync launched, so there is no need to rewrite progress.bin here.
  if (sync.outcome == KOReaderSyncOutcomeState::UPLOAD_COMPLETE) {
    LOG_DBG("ERS", "Upload-complete resume keeps existing local progress.bin unchanged");
    sync.clear();
    APP_STATE.saveToFile();
    logReaderMemSnapshot("after_apply_pending_sync_session");
    return;
  }

  // AUTO_PULL handed off zeroed local state (the reader was not yet running when sync started),
  // so on cancel/fail we must NOT restore those zeros to progress.bin — they would clobber the
  // user's real local progress. Just clear the session and let the normal startup load progress.bin.
  if (sync.intent == KOReaderSyncIntentState::AUTO_PULL && sync.outcome != KOReaderSyncOutcomeState::APPLIED_REMOTE) {
    LOG_DBG("ERS", "AUTO_PULL non-success outcome=%d: leaving progress.bin untouched", static_cast<int>(sync.outcome));
    sync.clear();
    APP_STATE.saveToFile();
    logReaderMemSnapshot("after_apply_pending_sync_session");
    return;
  }

  int restoreSpineIndex = sync.spineIndex;
  int restorePage = sync.page;

  if (restoreSpineIndex < 0 || restoreSpineIndex >= epub->getSpineItemsCount()) {
    LOG_ERR("ERS", "Invalid sync restore spine index %d, resetting to 0", restoreSpineIndex);
    restoreSpineIndex = 0;
    restorePage = 0;
  }

  // Build the navigation target from the sync result. For LUT-anchored targets the
  // estimated restorePage is plumbed through as fallbackPage so a LUT miss in the
  // target spine still lands the user on a sensible page rather than page 0.
  NavigationTarget restoreTarget;
  if (sync.outcome == KOReaderSyncOutcomeState::APPLIED_REMOTE) {
    const int spineCount = epub->getSpineItemsCount();
    if (sync.resultSpineIndex < 0 || sync.resultSpineIndex >= spineCount) {
      LOG_ERR("ERS", "Sync resultSpineIndex %d out of range [0,%d), clamping to previous %d", sync.resultSpineIndex,
              spineCount, restoreSpineIndex);
      // Keep restoreSpineIndex / restorePage from the pre-validation block above.
    } else {
      restoreSpineIndex = sync.resultSpineIndex;
      restorePage = sync.resultPage;
    }
    if (sync.resultHasListItemIndex) {
      restoreTarget = NavigationTarget::makeListItem(sync.resultListItemIndex, restorePage);
      LOG_DBG("ERS", "Applied synced remote position: spine=%d page=%d li[%u]", restoreSpineIndex, restorePage,
              sync.resultListItemIndex);
    } else if (sync.resultHasParagraphIndex) {
      restoreTarget = NavigationTarget::makeParagraph(sync.resultParagraphIndex, restorePage);
      LOG_DBG("ERS", "Applied synced remote position: spine=%d page=%d p[%u]", restoreSpineIndex, restorePage,
              sync.resultParagraphIndex);
    } else {
      restoreTarget = NavigationTarget::makePage(restorePage);
      LOG_DBG("ERS", "Applied synced remote position: spine=%d page=%d (no LUT)", restoreSpineIndex, restorePage);
    }
  } else {
    restoreTarget = NavigationTarget::makePage(restorePage);
    LOG_DBG("ERS", "Restored local pre-sync position: spine=%d page=%d", restoreSpineIndex, restorePage);
  }

  // sync.totalPagesInSpine is the page count of the local spine at launch time.
  // When the restore targets a different spine, that count is meaningless for
  // rescaling the fallbackPage estimate (which was estimated from cross-spine
  // density anyway). Store 0 to disable rescaling — the LUT lookup is the precise
  // path, and the cross-spine fallback can't usefully be rescaled here.
  const int restorePageCount = (restoreSpineIndex == sync.spineIndex) ? sync.totalPagesInSpine : 0;
  restoreTarget.cachedPageCount = restorePageCount;
  restoreTarget.cachedSpineIdx = restoreSpineIndex;

  // Seed live state directly — the previous write-then-reload-from-disk pattern relied
  // on progress.bin being read after this function ran, which clobbered the LUT target.
  // Live-state seeding is authoritative; the persistent write below is just for crash
  // recovery so a power loss before the next saveProgress() doesn't lose the synced
  // spine/page. The next render's saveProgress() supplies the real percent before
  // the user can return to the home screen.
  currentSpineIndex = restoreSpineIndex;
  navTarget = restoreTarget;
  if (!writeReaderProgressCache(epub->getCachePath(), restoreSpineIndex, restorePage, restorePageCount, 0)) {
    LOG_ERR("ERS", "Failed to persist sync restore to progress.bin; live state still seeded");
  } else {
    LOG_DBG("ERS", "Prepared progress.bin for sync restore: spine=%d page=%d/%d", restoreSpineIndex, restorePage,
            sync.totalPagesInSpine);
  }

  sync.clear();
  APP_STATE.saveToFile();
  logReaderMemSnapshot("after_apply_pending_sync_session");
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
                           paragraphAlignmentOverride, bookTextAntiAliasingOverride, bookHyphenationOverride);
}

void EpubReaderActivity::applyBookReaderOverrides(
    const int8_t embeddedStyleOverride, const int8_t imageRenderingOverride, const int8_t fontFamilyOverride,
    const std::string& sdFontFamilyOverride, const int8_t fontSizeOverride, const int8_t bionicReadingOverride,
    const int8_t paragraphAlignmentOverride, const int8_t textAntiAliasingOverride, const int8_t hyphenationOverride) {
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

  if (bookEmbeddedStyleOverride == embeddedStyleOverride && bookImageRenderingOverride == imageRenderingOverride &&
      bookFontFamilyOverride == normalizedFontFamilyOverride &&
      bookSdFontFamilyOverride == normalizedSdFontFamilyOverride && bookFontSizeOverride == fontSizeOverride &&
      bookBionicReadingOverride == bionicReadingOverride &&
      bookParagraphAlignmentOverride == paragraphAlignmentOverride &&
      bookTextAntiAliasingOverride == textAntiAliasingOverride && bookHyphenationOverride == hyphenationOverride) {
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
  RECENT_BOOKS.setReaderOverrides(epub->getPath(), bookEmbeddedStyleOverride, bookImageRenderingOverride,
                                  bookFontFamilyOverride, bookSdFontFamilyOverride, bookFontSizeOverride,
                                  bookBionicReadingOverride, bookParagraphAlignmentOverride,
                                  bookTextAntiAliasingOverride, bookHyphenationOverride);

  RenderLock lock(*this);
  if (section) {
    navTarget = NavigationTarget::makePage(section->currentPage);
    navTarget.cachedPageCount = section->pageCount;
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

Section::HeadingFonts EpubReaderActivity::buildHeadingFonts() const {
  // Headings render by SCALING the body font (nearest-neighbor upscale in renderCharAtScale),
  // not by switching to a taller built-in font. The taller-font approach was tried and dropped:
  // it put a second font on chapter-opener pages, which thrashed the limited glyph-cache slots
  // (multi-second page stalls), and only delivered quantized 2pt steps capped at 18pt for
  // built-in fonts. Scaling gives the exact 1.6/1.4/1.2 ratios at any body size with one font
  // per page (no cache pressure). Defaults already encode that: fontId all 0 = scale fallback.
  return Section::HeadingFonts{};
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
  if (!epub || !section || section->pageCount == 0) {
    return false;
  }

  // NOTE: crossing a section boundary used to pre-arm pendingHalfRefreshAfterBufferRealloc_
  // here. It no longer does: the half-refresh is only needed when the secondary buffer is
  // actually released + reallocated (white baseline), so that flag is now owned by the real
  // release sites (buildSection's indexing path, the image-warm pass, buffer recovery). A
  // section change served from cache or from a completed Background-B build never releases the
  // buffer, so its baseline is intact and the first page can use a normal fast refresh.
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      // Serialize against the render task: it reads section->currentPage (and the
      // PreRender pass temporarily writes it), so the advance must not race.
      RenderLock lock(*this);
      section->currentPage++;
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      RenderLock lock(*this);
      navTarget = NavigationTarget::makePage(0);
      currentSpineIndex++;
      section.reset();
    } else if (currentSpineIndex + 1 == epub->getSpineItemsCount()) {
      RenderLock lock(*this);
      navTarget = NavigationTarget::makeLastPage();
      currentSpineIndex++;
      section.reset();
    } else {
      return false;
    }
  } else {
    if (section->currentPage > 0) {
      RenderLock lock(*this);
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      RenderLock lock(*this);
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

void EpubReaderActivity::recoverSecondaryBufferIfNeeded() {
  // Opportunistic recovery: after an OOM during chapter indexing, try to
  // restore the secondary buffer on subsequent renders when heap may be healthier.
  if (secondaryBufferDegraded_ && !renderer.hasSecondaryBuffer()) {
    if (renderer.reallocSecondaryBuffer()) {
      secondaryBufferDegraded_ = false;
      if (!renderer.isX3()) pendingHalfRefreshAfterBufferRealloc_ = true;
      LOG_INF("ERS", "Secondary display buffer restored; re-enabling normal refresh/AA paths");
    }
  } else if (secondaryBufferDegraded_ && renderer.hasSecondaryBuffer()) {
    secondaryBufferDegraded_ = false;
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
      *this, renderer, mappedInput, epub->getPath(), epub->getSeries(), epub->getSeriesIndex(),
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
          section->currentPage, section->pageCount, refreshModeName(renderer.getLastRefreshMode()),
          renderer.getLastDisplayModeByte());
  return true;
}

void EpubReaderActivity::renderPreRenderPass(const RenderLayout& layout) {
  // Pre-render pass: render next page content into the frame buffer (no status bar, no flush).
  if (!section || preRenderedPage.ready) {
    return;
  }
  const int nextPage = section->currentPage + 1;
  if (nextPage >= section->pageCount) {
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
  auto p = section->loadPageFromSectionFile();
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

bool EpubReaderActivity::heapAllowsInPlaceBuild(const bool embeddedStyle) const {
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
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  return freeHeap >= IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES && contigHeap >= IN_PLACE_BUILD_MIN_CONTIG_HEAP_BYTES;
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

  const auto runCreate = [&]() {
    return section->createSectionFile(getEffectiveReaderFontId(), getEffectiveReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, getEffectiveParagraphAlignment(),
                                      layout.viewportWidth, layout.viewportHeight, getEffectiveHyphenation(),
                                      embeddedStyle, getEffectiveBionicReading(), imageRendering, nullptr,
                                      /*skipEviction=*/false, buildHeadingFonts());
  };

  // Prefer to build WITHOUT releasing the secondary buffer when heap is ample, so the chapter's
  // first page keeps a valid fast-refresh baseline. The in-place attempt defers image decode to
  // the lazy per-page path, so a failure here is a graceful parser abort (not a corruption-prone
  // decode under pressure). On X3 we always release: its baseline lives in the controller, so
  // keeping the RAM buffer buys no display benefit, only less headroom.
  bool released = false;
  const bool inPlace = !renderer.isX3() && renderer.hasSecondaryBuffer() && heapAllowsInPlaceBuild(embeddedStyle);
  if (inPlace) {
    LOG_INF("ERS", "Building section in place (secondary buffer kept): free=%lu contig=%lu", esp_get_free_heap_size(),
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
  } else {
    LOG_INF("ERS", "Index start mem (before fb release): free=%lu", esp_get_free_heap_size());
    renderer.releaseSecondaryBuffer();  // frees ~52 KB for CSS parser + image decoder
    released = true;
    LOG_INF("ERS", "Index start mem (after fb release): free=%lu", esp_get_free_heap_size());
  }

  const uint32_t createStart = millis();
  bool createOk = runCreate();
  LOG_INF("ERS", "createSectionFile returned %d in %ums (free=%lu)", createOk ? 1 : 0, millis() - createStart,
          esp_get_free_heap_size());
  checkHeapIntegrity("after_createSectionFile");

  if (!createOk && inPlace) {
    // The conservative in-place gate was too optimistic. createSectionFile already reset its
    // build state on failure (Section::stepSectionBuild), so the retry starts clean; free the
    // buffer for the headroom the blocking foreground path has always relied on.
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
    if (!renderer.reallocSecondaryBuffer()) {
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
      if (!renderer.isX3()) pendingHalfRefreshAfterBufferRealloc_ = true;
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
    section = std::move(backgroundSection_);
    LOG_INF("ERS", "Adopting background section for spine %d (%s)", currentSpineIndex,
            resumeBackgroundBuild ? "resuming partial build" : "build complete");
  } else {
    section = std::make_unique<Section>(epub, currentSpineIndex, renderer);
  }
  resetBackgroundBuild();
  const unsigned long sectionStart = millis();

  if (resumeBackgroundBuild ||
      !section->loadSectionFile(getEffectiveReaderFontId(), getEffectiveReaderLineCompression(),
                                SETTINGS.extraParagraphSpacing, getEffectiveParagraphAlignment(), viewportWidth,
                                viewportHeight, getEffectiveHyphenation(), embeddedStyle, getEffectiveBionicReading(),
                                imageRendering)) {
    LOG_DBG("ERS", "Cache not found, building...");
    lastRenderStats.cacheRebuilt = true;

    const BuildOutcome outcome = compileSectionCache(layout, embeddedStyle, imageRendering);
    if (outcome == BuildOutcome::Restarting) {
      return false;  // fragmented-heap recovery reboot in progress
    }
    renderer.restoreFontMetadata();
    readerPhase_ = ReaderPhase::READING;
    if (outcome == BuildOutcome::Failed) {
      LOG_ERR("ERS", "Failed to persist page data to SD");
      section.reset();
      return false;
    }
  } else if (section->isEmbeddedStyleFallback()) {
    LOG_INF("ERS", "No-CSS fallback loaded; rebuilding with embedded CSS...");
    lastRenderStats.cacheRebuilt = true;

    const BuildOutcome outcome = compileSectionCache(layout, embeddedStyle, imageRendering);
    if (outcome == BuildOutcome::Restarting) {
      return false;  // fragmented-heap recovery reboot in progress
    }
    renderer.restoreFontMetadata();
    readerPhase_ = ReaderPhase::READING;
    if (outcome == BuildOutcome::Failed) {
      LOG_ERR("ERS", "Failed to rebuild CSS section cache; keeping fallback");
      section->loadSectionFile(getEffectiveReaderFontId(), getEffectiveReaderLineCompression(),
                               SETTINGS.extraParagraphSpacing, getEffectiveParagraphAlignment(), viewportWidth,
                               viewportHeight, getEffectiveHyphenation(), embeddedStyle, getEffectiveBionicReading(),
                               imageRendering);
    }
  } else {
    LOG_DBG("ERS", "Cache found, skipping build...");
  }
  lastRenderStats.sectionLoadMs = millis() - sectionStart;

  if (section->isTruncatedCache() && currentSpineIndex != lastWarnedTruncatedSpineIndex) {
    lastWarnedTruncatedSpineIndex = currentSpineIndex;
    truncatedSectionHintRendersRemaining = TRUNCATED_SECTION_HINT_RENDER_COUNT;
    LOG_INF("ERS", "Section %d is truncated; showing mitigation hint", currentSpineIndex);
  }

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
    LOG_DBG("ERS",
            "Page summary: spine=%d page=%d/%d prerendered=0 refresh=%s mode=0x%02X renderMs=%lu prewarmMs=%lu "
            "bwMs=%lu displayMs=%lu fontHits=%lu fontMisses=%lu fontHitPct=%lu glyphCalls=%lu glyphUs=%lu",
            currentSpineIndex, section->currentPage, section->pageCount, refreshModeName(renderer.getLastRefreshMode()),
            renderer.getLastDisplayModeByte(), lastRenderStats.requestRenderMs, lastRenderStats.phases.prewarmMs,
            lastRenderStats.phases.bwRenderMs, lastRenderStats.phases.displayMs, lastRenderStats.fontCacheHits,
            lastRenderStats.fontCacheMisses, fontHitRatePct, lastRenderStats.fontGetBitmapCalls,
            lastRenderStats.fontGetBitmapTimeUs);

    if (pendingScreenshot) {
      pendingScreenshot = false;
      ScreenshotUtil::takeScreenshot(renderer);
    }

    // Pre-render was already scheduled in renderContents() before the lock was
    // released, so the loop task could start it during the waveform wait.
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

  // Classify the pass, then consume the pre-render flags.
  const RenderPass pass = classifyRenderPass();
  pendingPreRender = false;
  usePreRenderedBuffer = false;
  // Discard the pre-render only when it is actually STALE — i.e. it no longer describes the
  // next page of the page currently displayed. A completed pre-render must survive an
  // intervening Normal render (periodic status-bar/clock update, deferred-AA-triggered
  // requestUpdate, etc.); the former "clear on any non-pre-render/non-buffer pass" rule threw
  // away a valid pre-render whenever such a render landed between the pre-render and the page
  // turn, turning a hit into a slow miss. Validity is keyed on (spineIndex, pageIndex) ==
  // (current spine, currentPage+1); the BufferDisplay/PreRender passes manage ready themselves.
  if (pass != RenderPass::PreRender && pass != RenderPass::BufferDisplay && preRenderedPage.ready) {
    const bool stillValid = section && preRenderedPage.spineIndex == currentSpineIndex &&
                            preRenderedPage.pageIndex == section->currentPage + 1;
    if (!stillValid) {
      preRenderedPage.ready = false;
    }
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
      renderNormalPass(lock, layout);
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

  const int page = (section ? section->currentPage : 0);
  const int pageCount = (section ? section->pageCount : 0);
  saveProgress(currentSpineIndex, page, pageCount);

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

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  const uint8_t percent = epubProgressPercentByte(*epub, spineIndex, currentPage, pageCount);
  if (!writeReaderProgressCache(epub->getCachePath(), spineIndex, currentPage, pageCount, percent)) {
    LOG_ERR("ERS", "Could not save progress!");
    return;
  }
  globalReadingSessionTracker().updateProgress(percent);
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
  if (page->hasUncachedImages(warmForceLoad, imageMonochrome) && renderer.hasSecondaryBuffer()) {
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
    if (!renderer.reallocSecondaryBuffer()) {
      LOG_ERR("ERS", "Failed to reallocate secondary buffer after image warm — display quality degraded");
      secondaryBufferDegraded_ = true;
    } else if (!renderer.isX3()) {
      // The realloc whitened the secondary buffer, so on X4 RED RAM no longer matches the panel.
      // Force this page through HALF_REFRESH (read just below) instead of a fast differential, which
      // would otherwise diff against a white baseline and ghost. Mirrors the section-change realloc.
      pendingHalfRefreshAfterBufferRealloc_ = true;
    }
  }
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

  bool forceHalfRefreshThisPage =
      (pendingHalfRefreshAfterImagePage && SETTINGS.halfRefreshAfterImagePage) || pendingHalfRefreshAfterBufferRealloc_;
  pendingHalfRefreshAfterImagePage = false;
  pendingHalfRefreshAfterBufferRealloc_ = false;
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

  // Trigger the display refresh — sends pixel data, issues CMD_DISPLAY_REFRESH,
  // swaps buffers, and returns immediately without waiting for the waveform.
  if (secondaryBufferDegraded_) {
    renderer.triggerDisplay(HalDisplay::FULL_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else if (forceRefreshModeNextRender_ >= 0) {
    // Manual force-refresh button: apply the requested mode for this one render.
    renderer.triggerDisplay(static_cast<HalDisplay::RefreshMode>(forceRefreshModeNextRender_));
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    forceRefreshModeNextRender_ = -1;
  } else if (forceHalfRefreshThisPage) {
    renderer.triggerDisplay(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    ReaderUtils::triggerWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Schedule a half-refresh on the next page turn after an image page to reduce ghosting.
  // Must be checked BEFORE page is moved into pendingGrayscale_ below.
  if (page->hasImages() && !page->allImagesArePlaceholders(effectiveForceLoad, imageMonochrome) &&
      getEffectiveImageRendering() != CrossPointSettings::IMAGES_SUPPRESS) {
    pendingHalfRefreshAfterImagePage = true;
  }

  // Deferred grayscale: store context before releasing the lock, so loop() can
  // run the AA pass. The page is kept alive via shared_ptr.
  if (aaEnabledForThisRender) {
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
  // imagePageWithAA never applies here. Two half-refresh requests can still carry over: the
  // image-page follow-up AND the post-buffer-realloc request. The realloc one is critical here:
  // after a section change / OOM recovery the secondary buffer is reallocated to white, so on X4
  // RED RAM no longer matches the panel. If this pre-rendered page is the first display after that
  // realloc and we let it run a fast differential, it diffs against a white baseline and ghosts
  // heavily. Honour (and clear) the flag exactly like renderContents() does, so the fast pre-render
  // path can't race ahead of the required baseline-restoring half-refresh.
  const bool forceHalfRefreshThisPage =
      (pendingHalfRefreshAfterImagePage && SETTINGS.halfRefreshAfterImagePage) || pendingHalfRefreshAfterBufferRealloc_;
  pendingHalfRefreshAfterImagePage = false;
  pendingHalfRefreshAfterBufferRealloc_ = false;
  if (secondaryBufferDegraded_) {
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else if (forceHalfRefreshThisPage) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }

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
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
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
  if (section) {
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
  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, isStarred, printedPageLabel);

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
  auto epub = std::make_shared<Epub>(filePath, "/.crosspoint");
  // Load CSS when embeddedStyle is enabled, as createSectionFile may need it to rebuild the cache.
  if (!epub->load(true, SETTINGS.embeddedStyle == 0)) {
    LOG_DBG("SLP", "EPUB: failed to load %s", filePath.c_str());
    return false;
  }

  epub->setupCacheDir();

  // Load saved spine index and page number
  int spineIndex = 0, pageNumber = 0;
  FsFile f;
  if (Storage.openFileForRead("SLP", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    if (f.read(data, 6) == 6) {
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
  const RecentBook currentBook = RECENT_BOOKS.getBookByPath(filePath);
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
                                SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                                SETTINGS.embeddedStyle, static_cast<bool>(SETTINGS.bionicReading),
                                SETTINGS.imageRendering)) {
    LOG_DBG("SLP", "EPUB: section cache not found for spine %d, rebuilding", spineIndex);
    if (!section->createSectionFile(effectiveFontId, effectiveLineCompression, SETTINGS.extraParagraphSpacing,
                                    SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                    SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                    static_cast<bool>(SETTINGS.bionicReading), SETTINGS.imageRendering, nullptr,
                                    /*skipEviction=*/false, Section::HeadingFonts{})) {
      LOG_ERR("SLP", "EPUB: failed to rebuild section cache for spine %d", spineIndex);
      return false;
    }
  }

  if (pageNumber < 0 || pageNumber >= section->pageCount) pageNumber = 0;
  section->currentPage = pageNumber;

  auto page = section->loadPageFromSectionFile();
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
  startActivityForResult(
      std::make_unique<QuickOverridesActivity>(
          renderer, mappedInput, bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
          bookSdFontFamilyOverride, bookFontSizeOverride, bookBionicReadingOverride, bookParagraphAlignmentOverride,
          bookTextAntiAliasingOverride, bookHyphenationOverride),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);
        applyBookReaderOverrides(menu.embeddedStyleOverride, menu.imageRenderingOverride, menu.fontFamilyOverride,
                                 menu.sdFontFamilyOverride, menu.fontSizeOverride,
                                 static_cast<int8_t>(menu.bionicReadingOverride), menu.paragraphAlignmentOverride,
                                 menu.textAntiAliasingOverride, menu.hyphenationOverride);
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
  // dialog can't address them anyway.
  const auto printedPageList = epub->loadPrintedPageList();
  const bool hasPrintedPages = std::any_of(printedPageList.begin(), printedPageList.end(), [](const auto& entry) {
    return parsePrintedPageLabel(entry.label).has_value();
  });

  ReaderUtils::enforceExitFullRefresh(renderer);
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(
          renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent, SETTINGS.orientation,
          !currentPageFootnotes.empty(), bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
          bookSdFontFamilyOverride, bookFontSizeOverride, SETTINGS.textDarkness, getEffectiveBionicReading(),
          bookParagraphAlignmentOverride, bookTextAntiAliasingOverride, bookHyphenationOverride,
          !bookmarkStore.isEmpty(), isCurrentPageStarred, hasPrintedPages),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);
        applyOrientation(menu.orientation);
        applyTextDarkness(menu.textDarkness);
        toggleAutoPageTurn(menu.pageTurnOption);
        applyBookReaderOverrides(menu.embeddedStyleOverride, menu.imageRenderingOverride, menu.fontFamilyOverride,
                                 menu.sdFontFamilyOverride, menu.fontSizeOverride,
                                 static_cast<bool>(menu.bionicReadingOverride), menu.paragraphAlignmentOverride,
                                 menu.textAntiAliasingOverride, menu.hyphenationOverride);
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
