#include "BookProgressPresentation.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

#include "KOReaderDocumentId.h"
#include "ReadingStats.h"
#include "fontIds.h"

namespace BookProgressPresentation {
namespace {
// Compact "hours/minutes" formatter for progress status lines, e.g. "1h 20m"
// or "45m". Never shows "0m" — a sub-minute estimate rounds up to "1m".
std::string formatEtaShort(uint32_t totalSeconds) {
  const uint32_t h = totalSeconds / 3600;
  const uint32_t m = (totalSeconds % 3600) / 60;
  char buf[16];
  if (h > 0) {
    snprintf(buf, sizeof(buf), "%uh %02um", h, m);
  } else {
    snprintf(buf, sizeof(buf), "%um", m > 0 ? m : 1u);
  }
  return buf;
}

// Pace-based "time to finish" suffix for a book, e.g. "~45m". Empty when the
// book is finished, has no progress data, or has too little history to estimate.
std::string bookEtaSuffix(const RecentBook& book, int progressPercent) {
  if (progressPercent < 0 || progressPercent >= 100) {
    return {};
  }
  const std::string docId = KOReaderDocumentId::calculateFromFilename(book.path);
  const uint32_t etaSeconds =
      READING_STATS.estimateRemainingSeconds(docId, 100.0f - static_cast<float>(progressPercent));
  if (etaSeconds == 0) {
    return {};
  }
  return "~" + formatEtaShort(etaSeconds);
}
}  // namespace

int readPercent(const RecentBook& book) {
  if (book.path.empty()) {
    return -1;
  }

  std::string cachePath;
  int percentByteOffset = 0;  // byte index of the percent field in progress.bin

  if (FsHelpers::hasEpubExtension(book.path)) {
    cachePath = Epub(book.path, "/.crosspoint").getCachePath();
    percentByteOffset = 6;  // epub: [spineIdx(2), page(2), chapterPageCount(2), percent(1)]
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    cachePath = Xtc(book.path, "/.crosspoint").getCachePath();
    percentByteOffset = 4;  // xtc: [page(4), percent(1)]
  } else if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    cachePath = Txt(book.path, "/.crosspoint").getCachePath();
    percentByteOffset = 6;  // txt: [page(2), offset(4), percent(1)]
  } else {
    return -1;
  }

  FsFile progressFile;
  if (!Storage.openFileForRead("UIT", cachePath + "/progress.bin", progressFile)) {
    return -1;
  }

  uint8_t data[7];
  const int dataSize = progressFile.read(data, 7);
  progressFile.close();

  if (dataSize < percentByteOffset + 1) {
    return -1;  // old format (or pre-render placeholder) without the percent byte
  }

  int percent = static_cast<int>(data[percentByteOffset]);
  if (percent > 100) percent = 100;
  return percent;
}

void drawIndicator(const GfxRenderer& renderer, Rect coverRect, int progressPercent) {
  if (progressPercent <= 0) {
    return;  // unread or no data — no indicator
  }

  if (progressPercent >= 100) {
    // Finished: a folded top-right corner. White backing wedge first so the
    // fold reads over dark art, then the solid black fold on top.
    constexpr int size = 18;
    const int right = coverRect.x + coverRect.width - 2;  // stay inside the 1px cover border
    const int top = coverRect.y + 1;
    for (int i = 0; i <= size; ++i) {
      const int haloW = size - i + 2;
      renderer.fillRect(right - haloW + 1, top + i, haloW, 1, false);
    }
    for (int i = 0; i <= size; ++i) {
      const int w = size - i;
      if (w > 0) {
        renderer.fillRect(right - w + 1, top + i, w, 1, true);
      }
    }
    return;
  }

  // In progress: a bar along the bottom edge, black outline + black fill on a
  // white halo so the empty portion stays visible over dark art.
  constexpr int barH = 5;
  const int barX = coverRect.x + 3;
  const int barW = coverRect.width - 6;
  const int barY = coverRect.y + coverRect.height - barH - 3;
  if (barW < 6) {
    return;
  }
  renderer.fillRect(barX - 1, barY - 1, barW + 2, barH + 2, false);
  renderer.drawRect(barX, barY, barW, barH, true);
  const int fillW = (barW - 2) * progressPercent / 100;
  if (fillW > 0) {
    renderer.fillRect(barX + 1, barY + 1, fillW, barH - 2, true);
  }
}

std::string formatStatus(const RecentBook& book, int progressPercent) {
  if (progressPercent < 0) {
    return {};
  }
  std::string line = std::to_string(progressPercent) + "%";
  const std::string eta = bookEtaSuffix(book, progressPercent);
  if (!eta.empty()) {
    line += " · " + eta;
  }
  return line;
}

void drawBadge(const GfxRenderer& renderer, Rect coverRect, const RecentBook& book, int progressPercent) {
  if (progressPercent < 0) {
    return;
  }
  // Stacked pill: percent on the first line, pace-based ETA on the second (when
  // available). Two short lines read narrower than one long "62% · ~45m" string.
  const std::string line1 = std::to_string(progressPercent) + "%";
  const std::string line2 = bookEtaSuffix(book, progressPercent);

  constexpr int inset = 6;  // clear the cover's rounded corner + selection ring
  constexpr int padX = 6;
  constexpr int padY = 3;
  constexpr int lineGap = 1;
  const int textH = renderer.getLineHeight(SMALL_FONT_ID);
  const int w1 = renderer.getTextWidth(SMALL_FONT_ID, line1.c_str());
  const int w2 = line2.empty() ? 0 : renderer.getTextWidth(SMALL_FONT_ID, line2.c_str());
  const int lineCount = line2.empty() ? 1 : 2;
  const int badgeW = std::max(w1, w2) + 2 * padX;
  const int badgeH = textH * lineCount + lineGap * (lineCount - 1) + 2 * padY;
  const int badgeX = coverRect.x + coverRect.width - badgeW - inset;
  const int badgeY = coverRect.y + inset;

  // White fill + black frame + black text: self-contained contrast on any cover
  // corner (the white background guarantees the black text stays legible, the
  // frame separates the pill from light artwork).
  renderer.fillRoundedRect(badgeX, badgeY, badgeW, badgeH, 4, Color::White);
  renderer.drawRoundedRect(badgeX, badgeY, badgeW, badgeH, 1, 4, true);
  renderer.drawText(SMALL_FONT_ID, badgeX + (badgeW - w1) / 2, badgeY + padY, line1.c_str(), true);
  if (!line2.empty()) {
    renderer.drawText(SMALL_FONT_ID, badgeX + (badgeW - w2) / 2, badgeY + padY + textH + lineGap, line2.c_str(), true);
  }
}

std::string formatReadingDuration(uint32_t totalSeconds) {
  const uint32_t h = totalSeconds / 3600;
  const uint32_t m = (totalSeconds % 3600) / 60;
  const uint32_t s = totalSeconds % 60;
  char buf[24];
  if (h > 0) {
    snprintf(buf, sizeof(buf), "%uh %02um", h, m);
  } else if (m > 0) {
    snprintf(buf, sizeof(buf), "%um %02us", m, s);
  } else {
    snprintf(buf, sizeof(buf), "%us", s);
  }
  return buf;
}

std::string formatLastRead(time_t epoch) {
  if (epoch == 0 || !HalClock::isSynced()) {
    return {};
  }
  const time_t now = HalClock::now();
  if (now <= epoch) {
    return "just now";
  }
  const uint32_t delta = static_cast<uint32_t>(now - epoch);
  char buf[24];
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

std::string historyLine(const RecentBook& book) {
  const BookReadingStats* stats = READING_STATS.findBook(KOReaderDocumentId::calculateFromFilename(book.path));
  if (stats == nullptr || stats->totalSeconds == 0) {
    return {};
  }

  char buf[48];
  snprintf(buf, sizeof(buf), tr(STR_STATS_READ_FORMAT), formatReadingDuration(stats->totalSeconds).c_str());
  std::string line = buf;

  // Days the book was opened on, not sittings. Sessions counts every open, so a glance inflates
  // it and "69 sittings" says nothing; days says how long you have been living with the book,
  // which is the thing you can feel.
  //
  // dayIndex 0 is the bucket for sessions recorded while the clock was not wall-anchored. Those
  // are real reading but belong to no known day, so they are excluded -- and on a device whose
  // clock has never synced that leaves nothing to say, so the clause is dropped rather than
  // claiming zero days.
  size_t knownDays = 0;
  for (const auto& day : stats->days) {
    if (day.dayIndex != 0) ++knownDays;
  }
  if (knownDays > 0) {
    snprintf(buf, sizeof(buf), knownDays == 1 ? tr(STR_STATS_DAY_ONE) : tr(STR_STATS_DAYS_FORMAT),
             static_cast<unsigned>(knownDays));
    line += " ";
    line += buf;
  }
  // Dropped rather than shown as unknown: a clock that has never synced would otherwise put
  // "last read: never" under a book finished yesterday.
  const std::string last = formatLastRead(stats->lastReadEpoch);
  if (!last.empty()) {
    snprintf(buf, sizeof(buf), tr(STR_STATS_LAST_FORMAT), last.c_str());
    line += " - ";
    line += buf;
  }
  return line;
}

std::string historyLineCompact(const RecentBook& book) {
  const BookReadingStats* stats = READING_STATS.findBook(KOReaderDocumentId::calculateFromFilename(book.path));
  if (stats == nullptr || stats->totalSeconds == 0) {
    return {};
  }

  std::string line = formatReadingDuration(stats->totalSeconds);
  size_t knownDays = 0;
  for (const auto& day : stats->days) {
    if (day.dayIndex != 0) ++knownDays;
  }
  if (knownDays > 0) {
    char buf[16];
    snprintf(buf, sizeof(buf), " · %ud", static_cast<unsigned>(knownDays));
    line += buf;
  }
  return line;
}

}  // namespace BookProgressPresentation
