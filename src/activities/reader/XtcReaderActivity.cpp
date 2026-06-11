/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
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
#include "components/UITheme.h"
#include "fontIds.h"

void XtcReaderActivity::onEnter() {
  Activity::onEnter();

  // See ReaderUtils::InputDrainGuard — prevents wake-up power-button hold from leaking into
  // the first detectPageTurn() call as a page turn or chapter skip.
  inputDrainGuard.arm();

  if (!xtc) {
    return;
  }

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  const std::string xtcSidecar = ReaderActivity::sidecarCoverPath(xtc->getPath());
  const std::string xtcCover = xtcSidecar.empty() ? xtc->getThumbBmpPath() : xtcSidecar;
  RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), "", xtcCover);

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

  bool delayedPrevTurn = false;
  bool delayedNextTurn = false;
  using BA = CrossPointSettings::BUTTON_ACTION;

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Confirm && ev.type == ButtonEventManager::PressType::Short) {
      if (xtc && xtc->hasChapters() && !xtc->getChapters().empty()) {
        ReaderUtils::enforceExitFullRefresh(renderer);
        startActivityForResult(
            std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, currentPage),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                currentPage = std::get<PageResult>(result.data).page;
              }
            });
        return;
      }
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
      return;
    }
    prevTriggered = delayedPrevTurn;
    nextTriggered = delayedNextTurn;
  }

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button opens the finished-book flow and back button returns to last page
  if (currentPage >= xtc->getPageCount()) {
    if (nextTriggered) {
      saveProgress();
      const std::string currentBookPath = xtc->getPath();
      const std::string nextBookPath =
          BookFinished::findNextBookInDirectory(currentBookPath, std::string(), std::string());
      startActivityForResult(
          std::make_unique<FinishedBookActivity>(renderer, mappedInput, currentBookPath, nextBookPath),
          [this, nextBookPath, currentBookPath](const ActivityResult& result) {
            if (result.isCancelled) {
              requestUpdate();
              return;
            }
            globalReadingSessionTracker().markFinished();
            const auto& menuResult = std::get<MenuResult>(result.data);
            if (menuResult.action == static_cast<int>(BookFinished::FinishedBookAction::GoHome)) {
              if (SETTINGS.moveFinishedBooksToCompleted) {
                std::string movedPath;
                BookFinished::moveFinishedBookToCompleted(currentBookPath, movedPath);
              }
              if (SETTINGS.removeFinishedBooksFromRecents) {
                RECENT_BOOKS.removeBook(currentBookPath);
              }
              activityManager.goHome();
              return;
            }
            if (menuResult.action == static_cast<int>(BookFinished::FinishedBookAction::OpenNextBook) &&
                !nextBookPath.empty()) {
              if (SETTINGS.moveFinishedBooksToCompleted) {
                std::string movedPath;
                BookFinished::moveFinishedBookToCompleted(currentBookPath, movedPath);
              }
              if (SETTINGS.removeFinishedBooksFromRecents) {
                RECENT_BOOKS.removeBook(currentBookPath);
              }
              activityManager.goToReader(nextBookPath);
              return;
            }
            requestUpdate();
          });
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

    // Display BW with conditional refresh based on pagesUntilFullRefresh
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

    // Pass 2: LSB buffer - mark DARK gray only (XTH value 1)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < maxSrcY; y++) {
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

  // XTC pages already have status bar pre-rendered, no need to add our own

  // Display with appropriate refresh
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  free(row);
  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", currentPage + 1, xtc->getPageCount(), bitDepth);
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
