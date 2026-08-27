/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FinishedBookActivity.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "ReadingSessionTracker.h"
#include "RecentBooksStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "activities/SliderPickerActivity.h"
#include "activities/home/BookInfoActivity.h"
#include "activities/settings/ReadingStatsBookDetailActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void XtcReaderActivity::onEnter() {
  Activity::onEnter();

  // See ReaderUtils::InputDrainGuard — prevents wake-up power-button hold from leaking into
  // the first page-turn check as a page turn or chapter skip.
  inputDrainGuard.arm();

  if (!xtc) {
    return;
  }

  // XTC pages are pre-rendered for the panel and are written straight to it by
  // writePhysicalPortraitPackedRow(), which bypasses the reader orientation. Pin the renderer to
  // portrait so the status-bar overlay — drawn in logical coordinates — lands on the same axes as
  // the page instead of across it when the global orientation is landscape.
  {
    RenderLock lock(*this);
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  }

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), "",
                       ReaderActivity::coverThumbPlaceholder(xtc->getPath()));

  // Start the reading-stats session. XTC has real title/author from the
  // file header so the per-book screen will look nicer than TXT/MD.
  globalReadingSessionTracker().begin(KOReaderDocumentId::calculateFromFilename(xtc->getPath()), xtc->getTitle(),
                                      xtc->getAuthor());

  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  Activity::onExit();

  // Flush stats session before tearing down the XTC reader.
  globalReadingSessionTracker().end();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  UITheme::getInstance().getMutableTheme().onBookWillClose(xtc ? xtc->getPath() : "", nullptr, xtc.get(), nullptr);
  xtc.reset();
}

void XtcReaderActivity::loop() {
  if (inputDrainGuard.shouldDrain(mappedInput)) {
    buttonEvents.drain();
    return;
  }

  bool buttonPrevTurn = false;
  bool buttonNextTurn = false;
  using BA = CrossPointSettings::BUTTON_ACTION;

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    // Confirm opens the reader menu, as it does in the EPUB reader. Chapter selection lives
    // inside it rather than on this button, so the gesture means the same thing in every reader.
    if (ev.button == MappedInputManager::Button::Confirm && ev.type == ButtonEventManager::PressType::Short) {
      ReaderUtils::enforceExitFullRefresh(renderer);
      openReaderMenu();
      return;
    }

    if (ev.button == MappedInputManager::Button::Back) {
      if (ev.type == ButtonEventManager::PressType::Long) {
        ReaderUtils::enforceExitFullRefresh(renderer);
        onGoHome();
        return;
      }
      if (ev.type == ButtonEventManager::PressType::Short) {
        ReaderUtils::enforceExitFullRefresh(renderer);
        finish();
        return;
      }
    }

    // Built-in default for a long-press on a page-turn button is chapter skip.
    // onButtonAction() no-ops when the XTC has no chapters. The FSM emits no Short for a
    // press that already produced a Long, so the release cannot also turn the page.
    // (Mirrors EpubReaderActivity; non-default long actions are dispatched by main.cpp.)
    if (ev.type == ButtonEventManager::PressType::Long) {
      const bool prevChapter =
          (ev.button == MappedInputManager::Button::PageBack && SETTINGS.btnLongPageBack == BA::BTN_DEFAULT) ||
          (ev.button == MappedInputManager::Button::Left && SETTINGS.btnLongLeft == BA::BTN_DEFAULT);
      const bool nextChapter =
          (ev.button == MappedInputManager::Button::PageForward && SETTINGS.btnLongPageForward == BA::BTN_DEFAULT) ||
          (ev.button == MappedInputManager::Button::Right && SETTINGS.btnLongRight == BA::BTN_DEFAULT);
      if (prevChapter || nextChapter) {
        onButtonAction(nextChapter ? BA::BTN_NEXT_SECTION : BA::BTN_PREV_SECTION);
        return;
      }
    }

    // Page turns for all four navigation buttons come from the event queue (see
    // EpubReaderActivity::loop for why). A non-default short action never arrives here —
    // main.cpp dispatches it globally — but the setting is re-checked so a future caller
    // that pushes events directly cannot turn a remapped button into a page turn.
    if (ev.type == ButtonEventManager::PressType::Short) {
      if ((ev.button == MappedInputManager::Button::PageBack && SETTINGS.btnShortPageBack == BA::BTN_DEFAULT) ||
          (ev.button == MappedInputManager::Button::Left && SETTINGS.btnShortLeft == BA::BTN_DEFAULT)) {
        buttonPrevTurn = true;
        continue;
      }
      if ((ev.button == MappedInputManager::Button::PageForward && SETTINGS.btnShortPageForward == BA::BTN_DEFAULT) ||
          (ev.button == MappedInputManager::Button::Right && SETTINGS.btnShortRight == BA::BTN_DEFAULT)) {
        buttonNextTurn = true;
        continue;
      }
    }
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectTiltPageTurn();
  if (!prevTriggered && !nextTriggered) {
    if (!buttonPrevTurn && !buttonNextTurn) {
      return;
    }
    prevTriggered = buttonPrevTurn;
    nextTriggered = buttonNextTurn;
  }

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button opens the finished-book flow and back button returns to last page
  if (currentPage >= xtc->getPageCount()) {
    if (nextTriggered) {
      saveProgress();
      BookFinished::launchFinishedBookFlow(*this, renderer, mappedInput, xtc->getPath(), std::string(), std::string(),
                                           xtc->getAuthor());
    } else {
      currentPage = xtc->getPageCount() - 1;
      requestUpdate();
    }
    return;
  }

  if (prevTriggered) {
    if (currentPage > 0) {
      currentPage--;
      globalReadingSessionTracker().onPageTurn();
      requestUpdate();
    }
  } else if (nextTriggered) {
    currentPage++;
    globalReadingSessionTracker().onPageTurn();
    requestUpdate();
  }
}

void XtcReaderActivity::openReaderMenu() {
  if (!xtc) {
    return;
  }
  startActivityForResult(
      std::make_unique<XtcReaderMenuActivity>(renderer, mappedInput, xtc->getTitle(), static_cast<int>(currentPage) + 1,
                                              static_cast<int>(xtc->getPageCount()),
                                              xtc->hasChapters() && !xtc->getChapters().empty()),
      [this](const ActivityResult& result) {
        // Back leaves the result variant empty. std::get on the wrong alternative throws, and the
        // device is built -fno-exceptions, so that would be an abort() rather than a no-op.
        if (result.isCancelled || !std::holds_alternative<MenuResult>(result.data)) {
          return;
        }
        onReaderMenuConfirm(static_cast<XtcReaderMenuActivity::MenuAction>(std::get<MenuResult>(result.data).action));
      });
}

void XtcReaderActivity::onReaderMenuConfirm(const XtcReaderMenuActivity::MenuAction action) {
  using MenuAction = XtcReaderMenuActivity::MenuAction;
  const int pageCount = static_cast<int>(xtc->getPageCount());

  switch (action) {
    case MenuAction::SELECT_CHAPTER:
      startActivityForResult(
          std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, currentPage),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              currentPage = std::get<PageResult>(result.data).page;
            }
          });
      break;

    case MenuAction::GO_TO_PERCENT: {
      // Both jumps land exactly: an XTC page count is baked into the container and never
      // repaginates, so there is no estimate here of the kind the EPUB reader has to make.
      const int initialPercent =
          pageCount > 0
              ? static_cast<int>(ReaderUtils::pageProgressPercentByte(static_cast<int>(currentPage), pageCount))
              : 0;
      startActivityForResult(std::make_unique<SliderPickerActivity>(renderer, mappedInput,
                                                                    SliderPickerActivity::Config{
                                                                        .titleId = StrId::STR_GO_TO_PERCENT,
                                                                        .hintId = StrId::STR_PERCENT_STEP_HINT,
                                                                        .minValue = 0,
                                                                        .maxValue = 100,
                                                                        .initialValue = initialPercent,
                                                                        .suffix = "%",
                                                                    }),
                             [this, pageCount](const ActivityResult& result) {
                               if (result.isCancelled) {
                                 return;
                               }
                               const int percent = std::get<PercentResult>(result.data).percent;
                               const int page = pageCount > 0 ? percent * pageCount / 100 : 0;
                               currentPage =
                                   static_cast<uint32_t>(std::min(std::max(page, 0), std::max(pageCount - 1, 0)));
                             });
      break;
    }

    case MenuAction::GO_TO_PAGE: {
      if (pageCount <= 0) {
        break;
      }
      startActivityForResult(
          std::make_unique<SliderPickerActivity>(renderer, mappedInput,
                                                 SliderPickerActivity::Config{
                                                     .titleId = StrId::STR_GO_TO_PAGE,
                                                     .hintId = StrId::STR_SLIDER_STEP_HINT,
                                                     .minValue = 1,
                                                     .maxValue = pageCount,
                                                     .initialValue = static_cast<int>(currentPage) + 1,
                                                 }),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              currentPage = static_cast<uint32_t>(std::get<PercentResult>(result.data).percent - 1);
            }
          });
      break;
    }

    case MenuAction::BOOK_INFO:
      startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, xtc->getPath()),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;

    case MenuAction::READING_STATS_FOR_BOOK:
      // Same filename-hash docId the session was opened with. Time from the session still running
      // lands in the store only on reader exit, so a first-ever session shows "no data" — accurate.
      startActivityForResult(std::make_unique<ReadingStatsBookDetailActivity>(
                                 renderer, mappedInput, KOReaderDocumentId::calculateFromFilename(xtc->getPath())),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;

    case MenuAction::MARK_AS_READ:
      if (pageCount > 0) {
        currentPage = static_cast<uint32_t>(pageCount - 1);
        saveProgress();
      }
      BookFinished::launchFinishedBookFlow(*this, renderer, mappedInput, xtc->getPath(), std::string(), std::string(),
                                           xtc->getAuthor());
      return;

    case MenuAction::DELETE_CACHE:
      // Drops the transposed page planes as well as the metadata, which for an XTH book is the
      // bulk of the cache. Progress is written back straight away so the position survives.
      xtc->clearCache();
      xtc->setupCacheDir();
      saveProgress();
      ReaderUtils::enforceExitFullRefresh(renderer);
      onGoHome();
      return;

    case MenuAction::GO_HOME:
      ReaderUtils::enforceExitFullRefresh(renderer);
      onGoHome();
      return;

    case MenuAction::NONE:
      break;
  }
}

void XtcReaderActivity::render(RenderLock&&) {
  if (!xtc) {
    return;
  }

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    // Show end of book screen
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderPage();
  saveProgress();
}

void XtcReaderActivity::renderPage() {
  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  // Stream the page row-major instead of allocating the whole page in heap. A
  // full XTH page (e.g. 96000 bytes) does not fit the fragmented ESP32-C3 heap
  // and previously failed to render; the stream keeps the working set to a few
  // small bounded buffers (XTH is transposed once to a row-major temp file).
  xtc::XtcPageRowStream pageStream;
  if (!xtc->openPageRowStream(pageStream, currentPage)) {
    LOG_ERR("XTR", "Failed to open page stream for page %lu", currentPage);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Clear screen first
  renderer.clearScreen();

  // XTC/XTCH pages are pre-rendered for the device. Ignore logical reader
  // orientation and map file pixels directly to the physical panel, cropping
  // if the encoded page is larger than this device (X4: 480x800, X3: 528x792).
  const uint16_t maxSrcX = std::min(pageWidth, renderer.getDisplayHeight());
  const uint16_t maxSrcY = std::min(pageHeight, renderer.getDisplayWidth());
  const size_t rowBytes = (static_cast<size_t>(pageWidth) + 7) / 8;
  uint8_t* row = static_cast<uint8_t*>(malloc(rowBytes));  // freed before return
  if (!row) {
    LOG_ERR("XTR", "Failed to allocate XTC row buffer (%u bytes)", static_cast<unsigned int>(rowBytes));
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (bitDepth == 2) {
    // XTH 2-bit mode (grayscale: 0=White, 1=Dark Grey, 2=Light Grey, 3=Black).
    // The stream splits the page into a cheap 1-bit ink plane (BW passes) and a
    // 2-bit gray plane (LSB/MSB passes), so the two identical BW passes only
    // touch the small plane. Right-to-left column order and bit-plane decoding
    // are handled inside the stream.

    // Pass 1: BW buffer - draw all ink (non-white) pixels as black.
    for (uint16_t y = 0; y < maxSrcY; y++) {
      bool invertBits = false;
      if (!pageStream.readBwRow(y, row, rowBytes, &invertBits)) {
        free(row);
        return;
      }
      renderer.writePhysicalPortraitPackedRow(y, row, maxSrcX, invertBits);
    }
    renderStatusBar();

    // Display BW with conditional refresh based on pagesUntilFullRefresh
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

    // Pass 2: LSB buffer - mark DARK gray only (XTH value 1)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    // Status-bar rows are left at the cleared 0x00 the pass starts from, which is what a purely
    // black-and-white region produces anyway — the overlay is BW text and must not pick up gray.
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < maxSrcY; y++) {
      if (isStatusBarRow(y)) continue;
      if (!pageStream.readGrayMaskRow(y, xtc::XtcPageRowStream::GrayMask::DarkOnly, row, rowBytes, maxSrcX)) {
        free(row);
        return;
      }
      renderer.writePhysicalPortraitPackedRow(y, row, maxSrcX);
    }
    renderer.copyGrayscaleLsbBuffers();

    // Pass 3: MSB buffer - mark LIGHT AND DARK gray (XTH value 1 or 2)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < maxSrcY; y++) {
      if (isStatusBarRow(y)) continue;
      if (!pageStream.readGrayMaskRow(y, xtc::XtcPageRowStream::GrayMask::LightOrDark, row, rowBytes, maxSrcX)) {
        free(row);
        return;
      }
      renderer.writePhysicalPortraitPackedRow(y, row, maxSrcX);
    }
    renderer.copyGrayscaleMsbBuffers();

    // Display grayscale overlay
    renderer.displayGrayBuffer();

    // Pass 4: Re-render BW to framebuffer (restore for next frame, instead of restoreBwBuffer)
    renderer.clearScreen();
    for (uint16_t y = 0; y < maxSrcY; y++) {
      bool invertBits = false;
      if (!pageStream.readBwRow(y, row, rowBytes, &invertBits)) {
        free(row);
        return;
      }
      renderer.writePhysicalPortraitPackedRow(y, row, maxSrcX, invertBits);
    }
    renderStatusBar();

    // Cleanup grayscale buffers with current frame buffer
    renderer.cleanupGrayscaleWithFrameBuffer();

    free(row);
    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale)", currentPage + 1, xtc->getPageCount());
    return;
  } else {
    // 1-bit mode: ink (non-white) pixels drawn black.
    for (uint16_t y = 0; y < maxSrcY; y++) {
      bool invertBits = false;
      if (!pageStream.readBwRow(y, row, rowBytes, &invertBits)) {
        free(row);
        return;
      }
      renderer.writePhysicalPortraitPackedRow(y, row, maxSrcX, invertBits);
    }
  }
  // White pixels are already cleared by clearScreen()

  renderStatusBar();

  // Display with appropriate refresh
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  free(row);
  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", currentPage + 1, xtc->getPageCount(), bitDepth);
}

bool XtcReaderActivity::isStatusBarRow(const int y) const {
  const int topHeight = UITheme::getStatusBarTopHeight();
  if (topHeight > 0 && y < topHeight) {
    return true;
  }
  const int bottomHeight = UITheme::getStatusBarBottomHeight();
  return bottomHeight > 0 && y >= renderer.getScreenHeight() - bottomHeight;
}

void XtcReaderActivity::renderStatusBar() const {
  const int topHeight = UITheme::getStatusBarTopHeight();
  const int bottomHeight = UITheme::getStatusBarBottomHeight();
  if (topHeight <= 0 && bottomHeight <= 0) {
    return;  // every status item is switched off — leave the page exactly as the file rendered it
  }

  // The reflowed readers keep the bar clear of the text by shrinking the viewport. A pre-rendered
  // page has no viewport to shrink and its ink runs to the panel edge, so the band is punched out
  // to white first and the bar drawn on top of the page.
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  if (topHeight > 0) {
    renderer.fillRect(0, 0, screenWidth, topHeight, false);
  }
  if (bottomHeight > 0) {
    renderer.fillRect(0, screenHeight - bottomHeight, screenWidth, bottomHeight, false);
  }

  const int pageCount = static_cast<int>(xtc->getPageCount());
  const int page = static_cast<int>(currentPage) + 1;
  const float progress = pageCount > 0 ? page * 100.0f / static_cast<float>(pageCount) : 0.0f;

  std::string title;
  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    for (const auto& chapter : xtc->getChapters()) {
      if (currentPage >= chapter.startPage && currentPage <= chapter.endPage) {
        title = chapter.name;
        break;
      }
    }
    // Chapters are optional in the container (and absent from plenty of real files). Falling back
    // to the book title beats showing "Unnamed" on every page of a book that simply has no ToC.
    if (title.empty()) {
      title = xtc->getTitle();
    }
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = xtc->getTitle();
  }

  GUI.drawStatusBar(renderer, progress, page, pageCount, title);

  lastStatusBarPage = page;
  lastStatusBarBattery = SETTINGS.statusBarBattery ? static_cast<int>(powerManager.getBatteryPercentage()) : -1;
  if (SETTINGS.useClock && SETTINGS.statusBarClock && HalClock::isSynced()) {
    const time_t now = HalClock::now();
    lastStatusBarClockMinute = now > 0 ? static_cast<int>(now / 60) : -1;
  } else {
    lastStatusBarClockMinute = -1;
  }
}

bool XtcReaderActivity::shouldSkipPeriodicUpdate() const {
  if (lastStatusBarPage < 0) return false;  // no baseline yet — let the first render happen
  if (static_cast<int>(currentPage) + 1 != lastStatusBarPage) return false;
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

void XtcReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    const uint8_t percent =
        ReaderUtils::pageProgressPercentByte(static_cast<int>(currentPage), static_cast<int>(xtc->getPageCount()));
    uint8_t data[5];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = (currentPage >> 16) & 0xFF;
    data[3] = (currentPage >> 24) & 0xFF;
    data[4] = percent;
    f.write(data, 5);
    f.close();
    globalReadingSessionTracker().updateProgress(percent);
  }
}

void XtcReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      LOG_DBG("XTR", "Loaded progress: page %lu", currentPage);

      // Validate page number
      if (currentPage >= xtc->getPageCount()) {
        currentPage = 0;
      }
    }
    f.close();
  }
}

bool XtcReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  Xtc xtc(filePath, "/.crosspoint");
  if (!xtc.load()) {
    LOG_DBG("SLP", "XTC: failed to load %s", filePath.c_str());
    return false;
  }

  // Load saved page number
  uint32_t savedPage = 0;
  FsFile f;
  if (Storage.openFileForRead("SLP", xtc.getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      savedPage = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    }
    f.close();
  }
  if (savedPage >= xtc.getPageCount()) savedPage = 0;

  const uint16_t pageWidth = xtc.getPageWidth();
  const uint16_t pageHeight = xtc.getPageHeight();
  const uint8_t bitDepth = xtc.getBitDepth();

  // Stream the page (BW pass only; grayscale is not needed under the overlay)
  // instead of allocating the whole page, which can fail on the fragmented heap.
  (void)bitDepth;
  xtc::XtcPageRowStream pageStream;
  if (!xtc.openPageRowStream(pageStream, savedPage, /*persist=*/false)) {
    LOG_ERR("SLP", "XTC: failed to open page stream for page %lu", savedPage);
    return false;
  }

  renderer.clearScreen();

  const uint16_t maxX = std::min(pageWidth, renderer.getDisplayHeight());
  const uint16_t maxY = std::min(pageHeight, renderer.getDisplayWidth());
  const size_t rowBytes = (static_cast<size_t>(pageWidth) + 7) / 8;
  uint8_t* row = static_cast<uint8_t*>(malloc(rowBytes));  // freed before return
  if (!row) {
    LOG_ERR("SLP", "XTC: failed to allocate row buffer (%u bytes)", static_cast<unsigned int>(rowBytes));
    return false;
  }

  // Both formats: stream.isInk() means a non-white pixel -> draw black.
  for (uint16_t y = 0; y < maxY; y++) {
    bool invertBits = false;
    if (!pageStream.readBwRow(y, row, rowBytes, &invertBits)) {
      free(row);
      return false;
    }
    renderer.writePhysicalPortraitPackedRow(y, row, maxX, invertBits);
  }

  free(row);
  return true;
}

void XtcReaderActivity::onButtonAction(const CrossPointSettings::BUTTON_ACTION action) {
  using BA = CrossPointSettings::BUTTON_ACTION;
  if (!xtc) return;
  const uint32_t pageCount = xtc->getPageCount();
  switch (action) {
    case BA::BTN_PAGE_FORWARD:
      if (currentPage + 1 < pageCount) {
        currentPage++;
        globalReadingSessionTracker().onPageTurn();
        requestUpdate();
      }
      break;
    case BA::BTN_PAGE_BACK:
      if (currentPage > 0) {
        currentPage--;
        globalReadingSessionTracker().onPageTurn();
        requestUpdate();
      }
      break;
    case BA::BTN_PAGE_FORWARD_10: {
      const uint32_t prevPage = currentPage;
      currentPage = (currentPage + 10 < pageCount) ? currentPage + 10 : pageCount - 1;
      if (currentPage != prevPage) {
        globalReadingSessionTracker().onPageTurn();
      }
      requestUpdate();
      break;
    }
    case BA::BTN_PAGE_BACK_10: {
      const uint32_t prevPage = currentPage;
      currentPage = (currentPage >= 10) ? currentPage - 10 : 0;
      if (currentPage != prevPage) {
        globalReadingSessionTracker().onPageTurn();
      }
      requestUpdate();
      break;
    }
    case BA::BTN_NEXT_SECTION:
      if (xtc->hasChapters()) {
        const auto& chapters = xtc->getChapters();
        const auto it = std::find_if(chapters.begin(), chapters.end(),
                                     [this](const auto& ch) { return ch.startPage > currentPage; });
        if (it != chapters.end()) {
          currentPage = it->startPage;
          globalReadingSessionTracker().onPageTurn();
          requestUpdate();
        }
      }
      break;
    case BA::BTN_PREV_SECTION:
      if (xtc->hasChapters()) {
        const auto& chapters = xtc->getChapters();
        const auto prevChapter = std::find_if(chapters.rbegin(), chapters.rend(),
                                              [this](const auto& ch) { return ch.startPage < currentPage; });

        if (prevChapter != chapters.rend()) {
          currentPage = prevChapter->startPage;
          globalReadingSessionTracker().onPageTurn();
          requestUpdate();
        }
      }
      break;
    case BA::BTN_EXIT_READER:
      ReaderUtils::enforceExitFullRefresh(renderer);
      finish();
      break;
    default:
      break;
  }
}
