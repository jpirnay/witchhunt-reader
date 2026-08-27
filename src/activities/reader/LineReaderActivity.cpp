#include "LineReaderActivity.h"

#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>

#include <algorithm>
#include <variant>

#include "CrossPointState.h"
#include "FinishedBookActivity.h"
#include "KOReaderDocumentId.h"
#include "ReaderActivity.h"
#include "ReadingSessionTracker.h"
#include "RecentBooksStore.h"
#include "activities/ActivityResult.h"
#include "activities/SliderPickerActivity.h"
#include "activities/home/BookInfoActivity.h"
#include "activities/settings/ReadingStatsBookDetailActivity.h"
#include "components/UITheme.h"

void LineReaderActivity::onEnter() {
  Activity::onEnter();

  // See ReaderUtils::InputDrainGuard — prevents wake-up power-button hold from leaking into
  // the first page-turn check as a page turn or chapter skip.
  inputDrainGuard.arm();

  if (!txt) {
    return;
  }

  {
    RenderLock lock(*this);
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  }

  txt->setupCacheDir();
  onReaderEnter();

  // Save as last opened file and add to recent books
  const std::string filePath = txt->getPath();
  const std::string fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "", ReaderActivity::coverThumbPlaceholder(filePath));

  // Start reading-stats session. Same filename-hash policy as EPUB so renamed
  // files start fresh; author is unknown for plain text formats.
  globalReadingSessionTracker().begin(KOReaderDocumentId::calculateFromFilename(filePath), fileName, "");

  // Trigger first update
  requestUpdate();
}

void LineReaderActivity::onExit() {
  Activity::onExit();

  // Flush the stats session before tearing down the document — same pattern as
  // EpubReaderActivity::onExit().
  globalReadingSessionTracker().end();

  onReaderExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  UITheme::getInstance().getMutableTheme().onBookWillClose(txt ? txt->getPath() : "", nullptr, nullptr, txt.get());
  txt.reset();
}

void LineReaderActivity::loop() {
  const bool drainActive = inputDrainGuard.shouldDrain(mappedInput);
  if (!drainActive && drainWasActive) {
    halTiltSensor.clearPendingEvents();
  }
  drainWasActive = drainActive;

  if (drainActive) {
    buttonEvents.drain();
    return;
  }

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
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

    // Confirm opens the reader menu, as it does in the EPUB and XTC readers. The overlays that
    // used to sit on this button (TXT starred pages, MD table of contents) are its first entries,
    // and both remain one press away on a remapped button (BTN_OPEN_BOOKMARKS / BTN_OPEN_TOC).
    if (ev.button == MappedInputManager::Button::Confirm && ev.type == ButtonEventManager::PressType::Short) {
      ReaderUtils::enforceExitFullRefresh(renderer);
      openReaderMenu();
      return;
    }

    if ((ev.button == MappedInputManager::Button::PageBack || ev.button == MappedInputManager::Button::Left) &&
        ev.type == ButtonEventManager::PressType::Short) {
      goToPreviousPage();
      return;
    }

    if ((ev.button == MappedInputManager::Button::PageForward || ev.button == MappedInputManager::Button::Right) &&
        ev.type == ButtonEventManager::PressType::Short) {
      goToNextPage();
      return;
    }
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectTiltPageTurn();
  if (prevTriggered) {
    goToPreviousPage();
  } else if (nextTriggered) {
    goToNextPage();
  }
}

void LineReaderActivity::openReaderMenu() {
  if (!txt) {
    return;
  }
  SimpleReaderMenuActivity::Options options;
  options.title = txt->getTitle();
  options.currentPage = currentPage + 1;
  options.totalPages = totalPages;
  fillMenuOptions(options);

  startActivityForResult(std::make_unique<SimpleReaderMenuActivity>(renderer, mappedInput, std::move(options)),
                         [this](const ActivityResult& result) {
                           // Back leaves the result variant empty. std::get on the wrong
                           // alternative throws, and the device is built -fno-exceptions, so that
                           // would be an abort() rather than a no-op.
                           if (result.isCancelled || !std::holds_alternative<MenuResult>(result.data)) {
                             return;
                           }
                           onReaderMenuConfirm(static_cast<SimpleReaderMenuActivity::MenuAction>(
                               std::get<MenuResult>(result.data).action));
                         });
}

void LineReaderActivity::onReaderMenuConfirm(const SimpleReaderMenuActivity::MenuAction action) {
  using MenuAction = SimpleReaderMenuActivity::MenuAction;

  // The format-specific entries first: the subclass owns the chapter list and the bookmark store.
  if (onReaderMenuAction(action)) {
    return;
  }

  switch (action) {
    case MenuAction::GO_TO_PERCENT: {
      // Both jumps land exactly. These formats paginate the whole document up front, so unlike the
      // EPUB reader there is no per-chapter estimate to go through.
      const int initialPercent = static_cast<int>(ReaderUtils::pageProgressPercentByte(currentPage, totalPages));
      startActivityForResult(std::make_unique<SliderPickerActivity>(renderer, mappedInput,
                                                                    SliderPickerActivity::Config{
                                                                        .titleId = StrId::STR_GO_TO_PERCENT,
                                                                        .hintId = StrId::STR_PERCENT_STEP_HINT,
                                                                        .minValue = 0,
                                                                        .maxValue = 100,
                                                                        .initialValue = initialPercent,
                                                                        .suffix = "%",
                                                                    }),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled) {
                                 return;
                               }
                               const int percent = std::get<PercentResult>(result.data).percent;
                               const int page = totalPages > 0 ? percent * totalPages / 100 : 0;
                               currentPage = std::min(std::max(page, 0), std::max(totalPages - 1, 0));
                               onPageChanged();
                             });
      break;
    }

    case MenuAction::GO_TO_PAGE: {
      if (totalPages <= 0) {
        break;
      }
      startActivityForResult(std::make_unique<SliderPickerActivity>(renderer, mappedInput,
                                                                    SliderPickerActivity::Config{
                                                                        .titleId = StrId::STR_GO_TO_PAGE,
                                                                        .hintId = StrId::STR_SLIDER_STEP_HINT,
                                                                        .minValue = 1,
                                                                        .maxValue = totalPages,
                                                                        .initialValue = currentPage + 1,
                                                                    }),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled) {
                                 return;
                               }
                               currentPage = std::get<PercentResult>(result.data).percent - 1;
                               onPageChanged();
                             });
      break;
    }

    case MenuAction::BOOK_INFO:
      startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, txt->getPath()),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;

    case MenuAction::READING_STATS_FOR_BOOK:
      // Same filename-hash docId the session was opened with. Time from the session still running
      // lands in the store only on reader exit, so a first-ever session shows "no data" — accurate.
      startActivityForResult(std::make_unique<ReadingStatsBookDetailActivity>(
                                 renderer, mappedInput, KOReaderDocumentId::calculateFromFilename(txt->getPath())),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;

    case MenuAction::MARK_AS_READ:
      if (totalPages > 0) {
        currentPage = totalPages - 1;
        onPageChanged();
      }
      launchFinishedBookFlow();  // saves progress before handing over
      break;

    case MenuAction::GO_HOME:
      ReaderUtils::enforceExitFullRefresh(renderer);
      onGoHome();
      break;

    // Handled by the subclass above, or never offered to these formats
    // (DELETE_CACHE: a page index is a few KB, there is nothing worth recovering).
    case MenuAction::SELECT_CHAPTER:
    case MenuAction::STAR_PAGE:
    case MenuAction::STARRED_PAGES:
    case MenuAction::DELETE_CACHE:
    case MenuAction::NONE:
      break;
  }
}

void LineReaderActivity::goToPreviousPage() {
  if (currentPage > 0) {
    currentPage--;
    onPageChanged();
    globalReadingSessionTracker().onPageTurn();
    requestUpdate();
  }
}

void LineReaderActivity::goToNextPage() {
  if (currentPage < totalPages - 1) {
    currentPage++;
    onPageChanged();
    globalReadingSessionTracker().onPageTurn();
    requestUpdate();
  } else {
    launchFinishedBookFlow();
  }
}

void LineReaderActivity::launchFinishedBookFlow() {
  saveProgress();
  BookFinished::launchFinishedBookFlow(*this, renderer, mappedInput, txt ? txt->getPath() : std::string(),
                                       std::string(), std::string());
}

LineReaderActivity::TextLayout LineReaderActivity::computeTextLayout(const GfxRenderer& renderer, const int fontId,
                                                                     const uint8_t screenMargin) {
  TextLayout layout;
  renderer.getOrientedViewableTRBL(&layout.marginTop, &layout.marginRight, &layout.marginBottom, &layout.marginLeft);
  layout.marginTop += std::max(static_cast<int>(screenMargin), UITheme::getStatusBarTopHeight());
  layout.marginLeft += screenMargin;
  layout.marginRight += screenMargin;
  layout.marginBottom += std::max(static_cast<int>(screenMargin), UITheme::getStatusBarBottomHeight());
  layout.viewportWidth = renderer.getScreenWidth() - layout.marginLeft - layout.marginRight;
  layout.viewportHeight = renderer.getScreenHeight() - layout.marginTop - layout.marginBottom;
  layout.linesPerPage = std::max(1, layout.viewportHeight / renderer.getLineHeight(fontId));
  return layout;
}

void LineReaderActivity::computeViewportLayout() {
  // Store current settings for index-cache validation
  cachedFontId = SETTINGS.getTxtReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  const TextLayout layout = computeTextLayout(renderer, cachedFontId, cachedScreenMargin);
  cachedOrientedMarginTop = layout.marginTop;
  cachedOrientedMarginRight = layout.marginRight;
  cachedOrientedMarginBottom = layout.marginBottom;
  cachedOrientedMarginLeft = layout.marginLeft;
  viewportWidth = layout.viewportWidth;
  linesPerPage = layout.linesPerPage;

  LOG_DBG(logTag, "Viewport: %dx%d, lines per page: %d", viewportWidth, layout.viewportHeight, linesPerPage);
}

void LineReaderActivity::noteStatusBarRendered() const {
  lastStatusBarPage = currentPage + 1;
  lastStatusBarBattery = SETTINGS.statusBarBattery ? static_cast<int>(powerManager.getBatteryPercentage()) : -1;
  if (SETTINGS.useClock && SETTINGS.statusBarClock && HalClock::isSynced()) {
    const time_t now = HalClock::now();
    lastStatusBarClockMinute = now > 0 ? static_cast<int>(now / 60) : -1;
  } else {
    lastStatusBarClockMinute = -1;
  }
}

bool LineReaderActivity::shouldSkipPeriodicUpdate() const {
  if (lastStatusBarPage < 0) return false;
  if (currentPage + 1 != lastStatusBarPage) return false;
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

void LineReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite(logTag, txt->getCachePath() + "/progress.bin", f)) {
    const size_t offset =
        (currentPage >= 0 && currentPage < static_cast<int>(pageOffsets.size())) ? pageOffsets[currentPage] : 0;
    const uint8_t percent = ReaderUtils::pageProgressPercentByte(currentPage, totalPages);
    uint8_t data[7];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = offset & 0xFF;
    data[3] = (offset >> 8) & 0xFF;
    data[4] = (offset >> 16) & 0xFF;
    data[5] = (offset >> 24) & 0xFF;
    data[6] = percent;
    f.write(data, 7);
    f.close();
    globalReadingSessionTracker().updateProgress(percent);
  }
}

void LineReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead(logTag, txt->getCachePath() + "/progress.bin", f)) {
    // Page sits in bytes 0-1 in both the legacy 4-byte format and the current
    // 7-byte format (page counts stay well under 65536).
    uint8_t data[2];
    if (f.read(data, 2) == 2) {
      const int loadedPage = data[0] + (data[1] << 8);
      if (totalPages <= 0) {
        currentPage = 0;
      } else {
        currentPage = std::min(loadedPage, totalPages - 1);
      }
      LOG_DBG(logTag, "Loaded progress: page %d/%d", currentPage, totalPages);
    }
    f.close();
  }
}
