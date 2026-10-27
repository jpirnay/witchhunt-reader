#include "ReadingStatsBookDetailActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <utility>

#include "MappedInputManager.h"
#include "ReadingSessionTracker.h"
#include "ReadingStats.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/BookProgressPresentation.h"
#include "components/CardLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

using BookProgressPresentation::formatReadingDuration;

// "today", "yesterday", "N days ago", or "YYYY-MM-DD" beyond a month.
// Returns "—" when epoch is 0 or the clock isn't synced.
std::string formatDateOrRelative(time_t epoch) {
  // Same wording as everywhere else; this screen differs only in that an unknown date is worth
  // saying out loud in a table of dates, where on the home screen it is worth saying nothing.
  const std::string relative = BookProgressPresentation::formatLastRead(epoch);
  return relative.empty() ? std::string(tr(STR_READING_STATS_UNKNOWN)) : relative;
}

std::string formatPagesPerMin(uint32_t pages, uint32_t seconds) {
  if (seconds < 60 || pages == 0) {
    return tr(STR_READING_STATS_UNKNOWN);
  }
  const float ppm = (pages * 60.0f) / seconds;
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", ppm);
  return buf;
}

uint32_t secondsForDayIn(const std::vector<DayBucket>& days, uint16_t dayIndex) {
  if (dayIndex == 0) return 0;
  auto it = std::lower_bound(days.begin(), days.end(), dayIndex,
                             [](const DayBucket& b, uint16_t v) { return b.dayIndex < v; });
  if (it != days.end() && it->dayIndex == dayIndex) return it->seconds;
  return 0;
}

}  // namespace

void ReadingStatsBookDetailActivity::onEnter() {
  Activity::onEnter();
  ReadingStatsStore::BookQuery query;
  if (READING_STATS.queryBook(docId, query) != ReadingStatsStore::ReadResult::Ok) query = {};
  {
    RenderLock lock(*this);
    query_ = std::move(query);
  }
  requestUpdate();
}

void ReadingStatsBookDetailActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && canRemove()) {
    confirmRemove();
  }
}

// Not while this book's session is still open (this screen opened from the reader's menu, or
// settings reached over an open book): the session lands in the store when the reader closes and
// would bring the entry straight back.
bool ReadingStatsBookDetailActivity::canRemove() const {
  if (!query_.found) return false;
  const auto& tracker = globalReadingSessionTracker();
  return !(tracker.isActive() && tracker.getDocId() == docId);
}

void ReadingStatsBookDetailActivity::confirmRemove() {
  const std::string title = query_.book.title.empty() ? docId : query_.book.title;
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_READING_STATS_REMOVE_BOOK), title),
      [this](const ActivityResult& res) {
        if (res.isCancelled) {
          requestUpdate();
          return;
        }
        if (READING_STATS.removeBook(docId) != ReadingStatsStore::WriteResult::Done) {
          LOG_ERR("RST", "remove failed doc=%s", docId.c_str());
          requestUpdate();
          return;
        }
        // Nothing left to show; the list underneath rebuilds when it regains the screen.
        finish();
      });
}

void ReadingStatsBookDetailActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/false);
  const BookReadingStats* book = query_.found ? &query_.book : nullptr;

  renderer.clearScreen();

  // Header — title (truncated) and author. Title fallback to docId so we
  // still produce a usable screen if the book's metadata was never recorded.
  std::string headerTitle = (book && !book->title.empty()) ? book->title : docId;
  {
    constexpr size_t kMaxChars = 28;
    const auto* p = reinterpret_cast<const unsigned char*>(headerTitle.c_str());
    const auto* start = p;
    size_t chars = 0;
    while (*p != 0 && chars < kMaxChars) {
      utf8NextCodepoint(&p);
      ++chars;
    }
    if (*p != 0) {
      headerTitle.resize(static_cast<size_t>(p - start));
      headerTitle += "…";
    }
  }
  GUI.drawHeader(renderer,
                 Rect{contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight},
                 headerTitle.c_str(), book && !book->author.empty() ? book->author.c_str() : nullptr);

  // Above the line Base keeps for the author subtitle. The cards are not cut to it; the optional
  // last one is left out when it would run past it (see the sparkline below).
  const Rect body = UITheme::getBodyRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/false,
                                         book ? book->author.c_str() : nullptr);
  const int bodyBottom = body.y + body.height;
  CardLayout::Config cfg;
  cfg.outerMarginX = metrics.verticalSpacing * 2;
  CardLayout layout(renderer, contentRect, body.y, cfg);

  if (!book) {
    // No history for this book: opened from the reader's menu before its
    // first session has been recorded, or the history failed to load.
    // Show a placeholder rather than crash on a null deref.
    layout.card(nullptr, [](CardLayout::Body& b) { b.centeredMessage(tr(STR_READING_STATS_NO_DATA)); });
  } else {
    // ---- Summary card: 4-cell grid (sessions / pages / avg / progress) ----
    const std::string avgValue = book->sessions > 0 ? formatReadingDuration(book->totalSeconds / book->sessions)
                                                    : std::string(tr(STR_READING_STATS_UNKNOWN));
    char pctBuf[8];
    snprintf(pctBuf, sizeof(pctBuf), "%u%%", book->progress);
    const std::string pctStr(pctBuf);
    layout.card(nullptr, [&](CardLayout::Body& b) {
      b.statGrid({{{std::to_string(book->sessions), tr(STR_READING_STATS_SESSIONS)},
                   {std::to_string(book->pagesTurned), tr(STR_READING_STATS_PAGES)},
                   {avgValue, tr(STR_READING_STATS_AVG_SESSION)},
                   {pctStr, tr(STR_READING_STATS_PROGRESS)}}});
    });

    // ---- Time card ----
    layout.card(tr(STR_READING_STATS_TOTAL_TIME), [&](CardLayout::Body& b) {
      b.rowLR(tr(STR_READING_STATS_TOTAL_TIME), formatReadingDuration(book->totalSeconds));
      b.rowLR(tr(STR_READING_STATS_PAGES_PER_MIN), formatPagesPerMin(book->pagesTurned, book->totalSeconds));
      // Time-to-finish estimate based on the user's pace. We display it on the
      // detail screen so the reader sees the projection even when not actively
      // reading the book; the reader's status bar can pick this up live.
      if (book->progress < 100) {
        const float remainingPercent = 100.0f - static_cast<float>(book->progress);
        const float own = ReadingStatsStore::ownSecondsPerPercent(book->totalSeconds, book->progress);
        const uint32_t etaSeconds =
            ReadingStatsStore::etaSeconds(own > 0.0f ? own : query_.pooledPace, remainingPercent);
        b.rowLR(tr(STR_READING_STATS_ETA),
                etaSeconds > 0 ? formatReadingDuration(etaSeconds) : std::string(tr(STR_READING_STATS_UNKNOWN)));
      }
    });

    // ---- History card ----
    layout.card(tr(STR_READING_STATS_HISTORY), [&](CardLayout::Body& b) {
      b.rowLR(tr(STR_READING_STATS_FIRST_READ), formatDateOrRelative(book->firstReadEpoch));
      b.rowLR(tr(STR_READING_STATS_LAST_READ), formatDateOrRelative(book->lastReadEpoch));
      if (book->finishedCount > 0) {
        // Pretty-print as just the date for a single finish, or "Nx — date"
        // for re-reads so the count and recency are both visible.
        std::string val = formatDateOrRelative(book->lastFinishedEpoch);
        if (book->finishedCount > 1) {
          char buf[16];
          snprintf(buf, sizeof(buf), "%ux — ", book->finishedCount);
          val = std::string(buf) + val;
        }
        b.rowLR(tr(STR_READING_STATS_FINISHED), val);
      }
    });

    // ---- Per-book sparkline (only when clock-anchored data exists, and it fits above the body's
    // foot: in landscape the cards above already take the height) ----
    constexpr int kSparkHeight = 32;
    const uint16_t today = currentLocalDayIndex();
    const bool sparkFits = layout.cursorY() + layout.cardHeight(true, kSparkHeight + 2) <= bodyBottom;
    if (today != 0 && !book->days.empty() && sparkFits) {
      layout.card(tr(STR_READING_STATS_LAST_30D), [&](CardLayout::Body& b) {
        constexpr int kSparkDays = 30;
        constexpr int kBarGap = 1;
        const int innerWidth = b.innerWidth();
        const int innerLeft = b.innerLeft();
        const int barWidth = std::max(2, (innerWidth - (kSparkDays - 1) * kBarGap) / kSparkDays);
        const int totalSpan = barWidth * kSparkDays + kBarGap * (kSparkDays - 1);
        const int sparkOriginX = innerLeft + (innerWidth - totalSpan) / 2;
        const int sparkOriginY = b.currentY();

        uint32_t maxSeconds = 1;
        for (int i = 0; i < kSparkDays; ++i) {
          const uint16_t d = (today > static_cast<uint16_t>(kSparkDays - 1 - i))
                                 ? static_cast<uint16_t>(today - (kSparkDays - 1 - i))
                                 : 0;
          const uint32_t s = secondsForDayIn(book->days, d);
          if (s > maxSeconds) maxSeconds = s;
        }

        renderer.drawLine(sparkOriginX, sparkOriginY + kSparkHeight, sparkOriginX + totalSpan,
                          sparkOriginY + kSparkHeight, true);
        for (int i = 0; i < kSparkDays; ++i) {
          const uint16_t d = (today > static_cast<uint16_t>(kSparkDays - 1 - i))
                                 ? static_cast<uint16_t>(today - (kSparkDays - 1 - i))
                                 : 0;
          const uint32_t s = secondsForDayIn(book->days, d);
          const int barX = sparkOriginX + i * (barWidth + kBarGap);
          const int h = s == 0 ? 1 : std::max<int>(2, (s * kSparkHeight) / maxSeconds);
          renderer.fillRect(barX, sparkOriginY + kSparkHeight - h, barWidth, h, true);
        }
        b.advance(kSparkHeight + 2);
      });
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), canRemove() ? tr(STR_REMOVE) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
