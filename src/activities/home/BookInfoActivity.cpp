#include "BookInfoActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <Xtc.h>

#include <cstdio>
#include <ctime>

#include "ReadingStats.h"
#include "components/TapZones.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

std::string formatReadingDuration(uint32_t totalSeconds) {
  const uint32_t h = totalSeconds / 3600;
  const uint32_t m = (totalSeconds % 3600) / 60;
  char buf[24];
  if (h > 0) {
    snprintf(buf, sizeof(buf), "%uh %02um", h, m);
  } else {
    snprintf(buf, sizeof(buf), "%um", m);
  }
  return buf;
}

// "N days ago" / "Nh ago" style, or an absolute date past a month. Empty when
// the epoch is unknown or the clock isn't synced (caller then skips the row).
std::string formatLastRead(time_t epoch) {
  if (epoch == 0 || !HalClock::isSynced()) {
    return {};
  }
  const time_t now = HalClock::now();
  char buf[24];
  if (now <= epoch) {
    return "just now";
  }
  const uint32_t delta = static_cast<uint32_t>(now - epoch);
  if (delta < 60) {
    return "just now";
  }
  if (delta < 3600) {
    snprintf(buf, sizeof(buf), "%um ago", delta / 60);
    return buf;
  }
  if (delta < 86400) {
    snprintf(buf, sizeof(buf), "%uh ago", delta / 3600);
    return buf;
  }
  const uint32_t days = delta / 86400;
  if (days < 30) {
    snprintf(buf, sizeof(buf), "%ud ago", days);
    return buf;
  }
  struct tm t{};
  localtime_r(&epoch, &t);
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return buf;
}

}  // namespace

std::string BookInfoActivity::formatFileSize(const size_t bytes) {
  char buf[16];
  if (bytes < 1024) {
    snprintf(buf, sizeof(buf), "%u B", static_cast<unsigned>(bytes));
  } else if (bytes < 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0f);
  } else {
    snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0f * 1024.0f));
  }
  return buf;
}

void BookInfoActivity::renderLoading() {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_INFO));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  renderer.drawText(UI_12_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop, tr(STR_LOADING));

  renderer.displayBuffer();
}

void BookInfoActivity::loadData() {
  loadSucceeded = false;
  loadError.clear();

  // Get file size
  HalFile f = Storage.open(filePath.c_str());
  if (f) {
    fileSizeBytes = f.fileSize();
    f.close();
  }

  // Load epub metadata — builds cache if missing, which also gives us cover
  if (FsHelpers::hasEpubExtension(filePath)) {
    Epub epub(filePath, "/.crosspoint");
    if (!epub.load(true, true)) {
      loadError = tr(STR_LOAD_EPUB_FAILED);
      LOG_ERR("BookInfo", "Failed to load EPUB metadata: %s", filePath.c_str());
      return;
    }

    title = epub.getTitle();
    author = epub.getAuthor();
    series = epub.getSeries();
    seriesIndex = epub.getSeriesIndex();
    description = epub.getDescription();

    // Generate thumbnail if not present yet, then record its path
    const auto& metrics = UITheme::getInstance().getMetrics();
    const std::string thumbPath = epub.getThumbBmpPath(metrics.homeCoverHeight);
    if (!Storage.exists(thumbPath.c_str())) {
      epub.generateThumbBmp(metrics.homeCoverHeight);
    }
    coverBmpPath = Storage.exists(thumbPath.c_str()) ? thumbPath : "";
    loadSucceeded = true;
  } else if (FsHelpers::hasXtcExtension(filePath)) {
    Xtc xtc(filePath, "/.crosspoint");
    if (!xtc.load()) {
      loadError = tr(STR_LOAD_XTC_FAILED);
      LOG_ERR("BookInfo", "Failed to load XTC metadata: %s", filePath.c_str());
      return;
    }
    title = xtc.getTitle();
    author = xtc.getAuthor();

    const auto& metrics = UITheme::getInstance().getMetrics();
    const std::string thumbPath = xtc.getThumbBmpPath(metrics.homeCoverHeight);
    if (!Storage.exists(thumbPath.c_str())) {
      xtc.generateThumbBmp(metrics.homeCoverHeight);
    }
    coverBmpPath = Storage.exists(thumbPath.c_str()) ? thumbPath : "";
    loadSucceeded = true;
  } else {
    loadError = tr(STR_ERROR_GENERAL_FAILURE);
    LOG_ERR("BookInfo", "Unsupported file type for book info: %s", filePath.c_str());
  }

  // Pull any recorded reading history for this book. Keyed by the same
  // filename-based document id the reader uses when recording sessions.
  if (loadSucceeded) {
    const std::string docId = KOReaderDocumentId::calculateFromFilename(filePath);
    if (const BookReadingStats* stats = READING_STATS.findBook(docId)) {
      hasReadingStats = stats->totalSeconds > 0 || stats->sessions > 0;
      statTotalSeconds = stats->totalSeconds;
      statProgress = stats->progress;
      statLastReadEpoch = stats->lastReadEpoch;
    }
  }
}

void BookInfoActivity::onEnter() {
  Activity::onEnter();
  fullRenderDone = false;
  {
    RenderLock lock(*this);
    renderLoading();
  }
  loadData();
  requestUpdate(true);
}

// Prev/Next are labelled on the front strip and this screen draws no side hints, so they stay there
// whichever way the device is held — only which of the two comes first on screen moves, and
// frontStripPrevious/Next answer that exactly the way mapLabels() does.
void BookInfoActivity::loop() {
  const auto goPrev = [this] {
    if (descPage > 0) {
      descPage--;
      requestUpdate(true);
    }
  };
  const auto goNext = [this] {
    if (descPage + 1 < descTotalPages) {
      descPage++;
      requestUpdate(true);
    }
  };

  // A description too long for one screen turns by tapping its two halves, the way a book page
  // does. Bounded to the description band rather than using the reader's full-height zones: this
  // screen draws a hint strip, and full-height zones would claim the taps meant for it.
  //
  // Only while there is somewhere to go. With a one-page description the band means nothing, and
  // consuming taps over it would be a hole in the screen for no benefit.
  int tapX = 0;
  int tapY = 0;
  if (mappedInput.hasTouch() && mappedInput.wasScreenTapped(tapX, tapY)) {
    // Copy the band under the seqlock before deciding anything with it. A torn read answers
    // "nothing here" and the tap falls through to the hint strip -- the safe way to be wrong.
    //
    // descTotalPages is read plainly: a single aligned int is atomic on this part, and the worst
    // a one-frame-stale count can do is let a tap through to goNext()/goPrev(), which guard.
    DescBand band;
    uint32_t before = 0;
    bool consistent = false;
    if (descBandSeq.beginRead(before)) {
      band = descBand;
      consistent = descBandSeq.endRead(before);
    }
    if (consistent && descTotalPages > 1) {
      switch (TapZones::halfOfBand(tapX, tapY, band.x, band.y, band.width, band.height)) {
        case TapZones::Half::Previous:
          goPrev();
          return;
        case TapZones::Half::Next:
          goNext();
          return;
        case TapZones::Half::None:
          break;
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  } else if (mappedInput.wasReleased(MappedInputManager::frontStripPrevious())) {
    goPrev();
  } else if (mappedInput.wasReleased(MappedInputManager::frontStripNext())) {
    goNext();
  }
}

void BookInfoActivity::render(RenderLock&&) {
  // Fast path: only the description page / button hints changed. Reuse the
  // already-rendered header/cover/meta by clearing just those two bands. Cap
  // consecutive partial renders to bound e-ink ghosting buildup.
  //
  // Requires Portrait orientation: the hints-gutter location is derived below
  // assuming the gutter sits at the bottom, which is only true in Portrait
  // (PortraitInverted puts it at the top; landscape orientations put it on a
  // side). In non-Portrait we fall through to a full render to avoid leaving
  // stale button labels in the real gutter.
  if (fullRenderDone && loadSucceeded && !description.empty() && partialRenderCount < MAX_PARTIAL_RENDERS &&
      renderer.getOrientation() == GfxRenderer::Portrait) {
    // The write framebuffer holds the frame from two refreshes ago (displayBuffer() swaps
    // buffers), so on alternating page turns it still contains the "Loading" screen or an
    // older page rather than the current cover/meta. Resync it to the displayed frame before
    // patching just the description/hints bands; without this the stale top section (cover or
    // "Loading") ships back to the panel and the cover appears to flip in and out.
    renderer.syncWriteBufferFromDisplayed();
    renderer.fillRect(descBand.x, descBand.y, descBand.width, descBand.height, false);
    renderer.fillRect(0, hintsBandY, renderer.getScreenWidth(), hintsBandHeight, false);
    // No seqlock bracket here: renderDescriptionAndHints() only READS descBand (it writes the
    // page count and the wrapped lines). Bumping the counter around it would make a concurrent
    // tap see a torn read and drop itself for no reason.
    renderDescriptionAndHints();
    ++partialRenderCount;
    renderer.displayBuffer();
    return;
  }

  partialRenderCount = 0;
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  // Header
  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_INFO));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = contentRect.y + contentRect.height - metrics.verticalSpacing;
  const int textX = contentRect.x + metrics.contentSidePadding;
  const int textWidth = contentRect.width - metrics.contentSidePadding * 2;
  const int lineHeightSmall = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineHeightLarge = renderer.getLineHeight(UI_12_FONT_ID);

  if (!loadSucceeded) {
    const auto lastSlash = filePath.find_last_of('/');
    const std::string fileName = (lastSlash == std::string::npos) ? filePath : filePath.substr(lastSlash + 1);
    const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, fileName.c_str(), textWidth, 2);
    const auto errorLines = renderer.wrappedText(
        UI_10_FONT_ID, loadError.empty() ? tr(STR_ERROR_GENERAL_FAILURE) : loadError.c_str(), textWidth, 3);

    int y = contentTop + metrics.verticalSpacing;
    for (const auto& line : titleLines) {
      if (y + lineHeightLarge > contentBottom) break;
      renderer.drawText(UI_12_FONT_ID, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += lineHeightLarge;
    }
    y += metrics.verticalSpacing;
    for (const auto& line : errorLines) {
      if (y + lineHeightSmall > contentBottom) break;
      renderer.drawText(UI_10_FONT_ID, textX, y, line.c_str());
      y += lineHeightSmall;
    }

    if (fileSizeBytes > 0 && y + lineHeightSmall <= contentBottom) {
      y += metrics.verticalSpacing;
      const std::string sizeLine = std::string(tr(STR_FILE_SIZE)) + ": " + formatFileSize(fileSizeBytes);
      renderer.drawText(UI_10_FONT_ID, textX, y, sizeLine.c_str());
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Reserve space so description always gets at least a few lines below the cover block
  const int descReserve = description.empty() ? 0 : (3 * lineHeightSmall + 2 * metrics.verticalSpacing);
  const int topSectionMaxH = contentBottom - contentTop - descReserve;

  // --- Top section: cover (left) + title/author/series (right) ---
  int topSectionBottom = contentTop;
  int metaX = textX;
  int metaWidth = textWidth;

  if (!coverBmpPath.empty()) {
    HalFile coverFile = Storage.open(coverBmpPath.c_str());
    if (coverFile) {
      Bitmap bmp(coverFile);
      if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
        // Natural aspect ratio, capped to available height
        int coverDisplayH = std::min(bmp.getHeight(), topSectionMaxH);
        int coverDisplayW = bmp.getWidth() * coverDisplayH / bmp.getHeight();
        // Also cap width at half the text area
        if (coverDisplayW > textWidth / 2) {
          coverDisplayW = textWidth / 2;
          coverDisplayH = bmp.getHeight() * coverDisplayW / bmp.getWidth();
        }
        renderer.drawBitmap(bmp, textX, contentTop, coverDisplayW, coverDisplayH);
        topSectionBottom = contentTop + coverDisplayH;

        const int gap = metrics.contentSidePadding;
        metaX = textX + coverDisplayW + gap;
        metaWidth = textWidth - coverDisplayW - gap;
      }
      coverFile.close();
    }
  }

  // Title/author/series in right column (or full width when no cover)
  int metaY = contentTop;

  if (!title.empty()) {
    const auto lines = renderer.wrappedText(UI_12_FONT_ID, title.c_str(), metaWidth, 4);
    for (const auto& line : lines) {
      if (metaY + lineHeightLarge > contentBottom) break;
      renderer.drawText(UI_12_FONT_ID, metaX, metaY, line.c_str(), true, EpdFontFamily::BOLD);
      metaY += lineHeightLarge;
    }
    metaY += 4;
  }

  if (!author.empty()) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, author.c_str(), metaWidth, 2);
    for (const auto& line : lines) {
      if (metaY + lineHeightSmall > contentBottom) break;
      renderer.drawText(UI_10_FONT_ID, metaX, metaY, line.c_str());
      metaY += lineHeightSmall;
    }
    metaY += 2;
  }

  if (!series.empty()) {
    std::string seriesLine = series;
    if (!seriesIndex.empty()) seriesLine += " #" + seriesIndex;
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, seriesLine.c_str(), metaWidth, 2);
    for (const auto& line : lines) {
      if (metaY + lineHeightSmall > contentBottom) break;
      renderer.drawText(UI_10_FONT_ID, metaX, metaY, line.c_str());
      metaY += lineHeightSmall;
    }
    metaY += 2;
  }

  if (fileSizeBytes > 0) {
    const std::string sizeStr = formatFileSize(fileSizeBytes);
    metaY += metrics.verticalSpacing;
    if (metaY + lineHeightSmall <= contentBottom) {
      renderer.drawText(UI_10_FONT_ID, metaX, metaY, sizeStr.c_str());
      metaY += lineHeightSmall;
    }
  }

  // Reading stats: a few compact "label: value" rows below the file size, shown
  // only for books with recorded history.
  if (hasReadingStats) {
    metaY += metrics.verticalSpacing;
    const auto drawStatRow = [&](const char* label, const std::string& value) {
      if (value.empty() || metaY + lineHeightSmall > contentBottom) return;
      const std::string line = std::string(label) + ": " + value;
      renderer.drawText(UI_10_FONT_ID, metaX, metaY, line.c_str());
      metaY += lineHeightSmall;
    };

    char pctBuf[8];
    snprintf(pctBuf, sizeof(pctBuf), "%u%%", statProgress);
    drawStatRow(tr(STR_READING_STATS_PROGRESS), pctBuf);
    drawStatRow(tr(STR_READING_STATS_TOTAL_TIME), formatReadingDuration(statTotalSeconds));
    drawStatRow(tr(STR_READING_STATS_LAST_READ), formatLastRead(statLastReadEpoch));
  }

  topSectionBottom = std::max(topSectionBottom, metaY);

  // --- Description: full width below the top section, paged via Left/Right ---
  // Bracketed because loop() reads this band to resolve a tap, and it runs on the other task:
  // a half-written rectangle would page the description the wrong way, or page one that is not
  // on screen. Same bar the touch recorders set for the same reason.
  descBandSeq.beginWrite();
  descTotalPages = 0;
  descLinesPerPage = 0;
  descBand.x = textX;
  descBand.width = textWidth;
  descBand.y = contentBottom;
  descBand.height = 0;
  if (!description.empty()) {
    int y = topSectionBottom + metrics.verticalSpacing;
    if (y + lineHeightSmall + 4 < contentBottom) {
      renderer.drawLine(textX, y, contentRect.x + contentRect.width - metrics.contentSidePadding, y);
      y += 4;

      // Record the description band (below the separator) for partial redraws.
      descBand.y = y;
      descBand.height = contentBottom - y;
    }
  }

  // Hints band spans from the bottom of the content area to the screen bottom.
  hintsBandY = contentRect.y + contentRect.height;
  hintsBandHeight = renderer.getScreenHeight() - hintsBandY;

  renderDescriptionAndHints();
  descBandSeq.endWrite();

  fullRenderDone = true;
  renderer.displayBuffer();
}

void BookInfoActivity::renderDescriptionAndHints() {
  const int lineHeightSmall = renderer.getLineHeight(UI_10_FONT_ID);

  descTotalPages = 0;
  descLinesPerPage = 0;

  if (!description.empty() && descBand.height > 0) {
    descLinesPerPage = descBand.height / lineHeightSmall;
    if (descLinesPerPage > 0) {
      if (descLines.empty() || descWrappedWidth != descBand.width) {
        descLines = renderer.wrappedText(UI_10_FONT_ID, description.c_str(), descBand.width, 1000);
        descWrappedWidth = descBand.width;
      }
      const int totalLines = static_cast<int>(descLines.size());
      descTotalPages = (totalLines + descLinesPerPage - 1) / descLinesPerPage;
      if (descPage >= descTotalPages) descPage = std::max(0, descTotalPages - 1);

      const int startLine = descPage * descLinesPerPage;
      const int endLine = std::min(totalLines, startLine + descLinesPerPage);
      int y = descBand.y;
      const int bandBottom = descBand.y + descBand.height;
      for (int i = startLine; i < endLine; ++i) {
        if (y + lineHeightSmall > bandBottom) break;
        renderer.drawText(UI_10_FONT_ID, descBand.x, y, descLines[i].c_str());
        y += lineHeightSmall;
      }
    }
  }

  const char* prevLabel = (descPage > 0) ? tr(STR_PREV) : "";
  const char* nextLabel = (descPage + 1 < descTotalPages) ? tr(STR_NEXT) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", prevLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
