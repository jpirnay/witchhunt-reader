// Book progress in millionths (lib/Epub/Epub/ProgressPpb.h) and the percents shown from it.
//
// Progress was a float fraction, and every screen rounded its own percent from it: half up, as printf's "%.0f" or
// "%.2f" does, or floored. The sweeps here run the old float formulas beside the integer ones over the books, pages
// and page counts a reader meets and require the same digits.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Epub/ProgressPpb.h"
#include "util/PrintfDouble.h"

namespace {

std::string format(const char* spec, const double value) {
  char buf[32];
  snprintf(buf, sizeof(buf), spec, value);
  return buf;
}

std::string hundredths(const uint32_t basisPoints) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%u.%02u", basisPoints / 100, basisPoints % 100);
  return buf;
}

// Epub::calculateProgress as it was.
float floatBookProgress(const uint32_t before, const uint32_t size, const uint32_t book, const float inChapter) {
  const float sectionProgSize = inChapter * static_cast<float>(size);
  const float totalProgress = static_cast<float>(before) + sectionProgSize;
  return totalProgress / static_cast<float>(book);
}

int clampPercent(const int percent) { return percent < 0 ? 0 : percent > 100 ? 100 : percent; }

// Chapter sizes in inflated XHTML bytes, from a fixed generator: short front matter, long chapters, a few odd
// sizes. Different books, so that the boundaries fall on different bytes.
std::vector<std::vector<uint32_t>> books() {
  std::vector<std::vector<uint32_t>> out;
  uint32_t state = 12345;
  auto next = [&state] {
    state = state * 1103515245u + 12345u;
    return state >> 8;
  };
  for (int b = 0; b < 12; ++b) {
    std::vector<uint32_t> chapters;
    const int count = 3 + static_cast<int>(next() % 40);
    for (int c = 0; c < count; ++c) {
      const uint32_t kind = next() % 10;
      chapters.push_back(kind == 0 ? 200 + next() % 3000 : kind < 8 ? 8000 + next() % 60000 : 100000 + next() % 400000);
    }
    out.push_back(chapters);
  }
  out.push_back({1000, 1000});            // halves fall on whole numbers
  out.push_back({125, 375, 500});         // eighths
  out.push_back({1, 199, 800});           // a sliver of a chapter
  out.push_back({600000, 600000, 1500});  // large chapters, over a megabyte in all
  return out;
}

// What a sweep found: how many values each screen showed differently, and that every one of them is a float
// rounding artefact. `exact` is the value in the screen's unit (percent, hundredths of a percent); `boundary` the
// value its rounding turns at nearest to it. Float carries 24 bits, so a value closer to a boundary than its error
// rounded either way; anything further must match.
struct Mismatches {
  int count = 0;
  std::string first;
  void note(const std::string& what, const long double exact, const long double tolerance) {
    if (count++ == 0) first = what;
    const long double halfway = std::floor(exact) + 0.5L;
    const long double whole = std::nearbyint(exact);
    const long double off = std::min(std::fabs(exact - halfway), std::fabs(exact - whole));
    EXPECT_LE(off, tolerance) << what << ": " << static_cast<double>(exact) << " is not at a boundary";
  }
};

}  // namespace

TEST(ProgressPpb, OfFloorsAndClamps) {
  EXPECT_EQ(ProgressPpb::of(3, 10).ppb(), 300000000u);
  EXPECT_EQ(ProgressPpb::of(1, 3).ppb(), 333333333u);
  EXPECT_EQ(ProgressPpb::of(2, 3).ppb(), 666666666u);
  EXPECT_EQ(ProgressPpb::of(0, 10).ppb(), 0u);
  EXPECT_EQ(ProgressPpb::of(10, 10).ppb(), ProgressPpb::kWhole);
  EXPECT_EQ(ProgressPpb::of(11, 10).ppb(), ProgressPpb::kWhole);
  EXPECT_EQ(ProgressPpb::of(-1, 10).ppb(), 0u);
  EXPECT_EQ(ProgressPpb::of(5, 0).ppb(), 0u);
  EXPECT_EQ(ProgressPpb::of(5, -3).ppb(), 0u);
  // A byte count of a large book.
  EXPECT_EQ(ProgressPpb::of(1500000000, 3000000000).ppb(), 500000000u);
  EXPECT_EQ(ProgressPpb::of(4294967294, 4294967295).ppb(), 999999999u);
  EXPECT_EQ(ProgressPpb(1500000000).ppb(), ProgressPpb::kWhole);
}

TEST(ProgressPpb, PartOfFloorsAndRoundedPartOfRoundsHalfUp) {
  const ProgressPpb half(500000000);
  EXPECT_EQ(half.partOf(39), 19u);
  EXPECT_EQ(half.roundedPartOf(39), 20u);
  EXPECT_EQ(ProgressPpb(ProgressPpb::kWhole).partOf(39), 39u);
  EXPECT_EQ(ProgressPpb().roundedPartOf(39), 0u);
  EXPECT_EQ(half.partOf(4000000000u), 2000000000u);
  EXPECT_EQ(ProgressPpb(250000000).roundedPartOf(4000000002u), 1000000001u);  // 1000000000.5 rounds up
  EXPECT_EQ(ProgressPpb(ProgressPpb::kWhole).partOf(4294967295u), 4294967295u);
}

TEST(ProgressPpb, AcrossWeighsTheChapterByItsSize) {
  EXPECT_EQ(ProgressPpb::across(0, 1000, 2000, ProgressPpb(500000000)).ppb(), 250000000u);
  EXPECT_EQ(ProgressPpb::across(1000, 1000, 2000, ProgressPpb()).ppb(), 500000000u);
  EXPECT_EQ(ProgressPpb::across(1000, 1000, 2000, ProgressPpb(ProgressPpb::kWhole)).ppb(), ProgressPpb::kWhole);
  EXPECT_EQ(ProgressPpb::across(1, 2, 0, ProgressPpb()).ppb(), 0u);
  // The largest sizes stay exact: 4 GB of book, a quarter of it before the chapter.
  EXPECT_EQ(ProgressPpb::across(1073741824u, 3221225471u, 4294967295u, ProgressPpb()).ppb(), 250000000u);
  EXPECT_EQ(ProgressPpb::across(4294967295u, 4294967295u, 4294967295u, ProgressPpb(ProgressPpb::kWhole)).ppb(),
            ProgressPpb::kWhole);
}

TEST(ProgressPpb, PercentRoundings) {
  EXPECT_EQ(ProgressPpb(124999999).percentHalfUp(), 12);
  EXPECT_EQ(ProgressPpb(125000000).percentHalfUp(), 13);
  EXPECT_EQ(ProgressPpb(125000000).percentHalfEven(), 12);  // printf's "%.0f" of 12.5
  EXPECT_EQ(ProgressPpb(135000000).percentHalfEven(), 14);
  EXPECT_EQ(ProgressPpb(125000001).percentHalfEven(), 13);
  EXPECT_EQ(ProgressPpb(129999999).percentFloor(), 12);
  EXPECT_EQ(ProgressPpb(ProgressPpb::kWhole).percentHalfUp(), 100);
  EXPECT_EQ(ProgressPpb(ProgressPpb::kWhole).percentHalfEven(), 100);
  EXPECT_EQ(ProgressPpb(123450000).basisPointsHalfEven(), 1234u);
  EXPECT_EQ(ProgressPpb(123550000).basisPointsHalfEven(), 1236u);
  EXPECT_EQ(ProgressPpb(123450001).basisPointsHalfEven(), 1235u);
  EXPECT_EQ(ProgressPpb(123499999).basisPoints(), 1234u);
  EXPECT_EQ(ProgressPpb(453236500).millionths(), 453237u);
  EXPECT_EQ(ProgressPpb(453236499).millionths(), 453236u);
  EXPECT_EQ(distance(ProgressPpb(10), ProgressPpb(25)), 15u);
  EXPECT_EQ(distance(ProgressPpb(25), ProgressPpb(10)), 15u);
}

// Every percent the EPUB reader shows, over a dozen generated books and some built for ties: the float formula of
// each screen beside the integer one. The status bar and the debug state ("%.0f"), the sleep overlay ("%.0f"), the
// progress byte, the reader menu, the bars and the search rows (+0.5f), the book progress bar (lround), the
// KOReader sync screen ("%.2f") and the plugin events (floored to the hundredth and to the percent).
TEST(ProgressPpb, EpubPercentsMatchTheFloatFormulasOffTheirBoundaries) {
  Mismatches halfUp, halfEven, bar, syncScreen, pluginTop, pluginBottom;
  long compared = 0;
  for (const auto& chapters : books()) {
    uint32_t book = 0;
    for (const uint32_t size : chapters) book += size;
    uint32_t before = 0;
    for (size_t spine = 0; spine < chapters.size(); ++spine) {
      const uint32_t size = chapters[spine];
      for (const int pageCount : {1, 2, 3, 4, 7, 8, 10, 13, 40, 100, 333}) {
        for (int page = 0; page <= pageCount; ++page) {
          const float f = floatBookProgress(before, size, book, static_cast<float>(page) / pageCount);
          const ProgressPpb p = ProgressPpb::across(before, size, book, ProgressPpb::of(page, pageCount));
          // The exact fraction, (before + size * page / pageCount) / book.
          const long double exact =
              (static_cast<long double>(before) * pageCount + static_cast<long double>(size) * page) /
              (static_cast<long double>(book) * pageCount);
          const std::string where = std::to_string(page) + "/" + std::to_string(pageCount) + " of spine " +
                                    std::to_string(spine) + " (" + std::to_string(p.ppb()) + " ppb)";
          ++compared;
          if (clampPercent(static_cast<int>(f * 100.0f + 0.5f)) != p.percentHalfUp()) {
            halfUp.note(where, exact * 100, 1e-4L);
          }
          if (format("%.0f", f * 100) != std::to_string(p.percentHalfEven())) halfEven.note(where, exact * 100, 1e-4L);
          if (std::clamp(static_cast<int>(std::lround(f * 100)), 0, 100) != p.percentHalfUp()) {
            bar.note(where, exact * 100, 1e-4L);
          }
          if (format("%.2f", f * 100) != hundredths(p.basisPointsHalfEven())) {
            syncScreen.note(where, exact * 10000, 1e-2L);
          }
          if (static_cast<int>(f * 10000.0f) != static_cast<int>(p.basisPoints())) {
            pluginTop.note(where, exact * 10000, 1e-2L);
          }
          if (static_cast<int>(f * 100.0f) != p.percentFloor()) pluginBottom.note(where, exact * 100, 1e-4L);
        }
      }
      before += size;
    }
  }
  EXPECT_GT(compared, 50000);
  // Recorded so a change shows: values the float rounded off its exact side, out of `compared`.
  RecordProperty("compared", static_cast<int>(compared));
  RecordProperty("halfUp", halfUp.count);
  RecordProperty("halfEven", halfEven.count);
  RecordProperty("syncScreen", syncScreen.count);
  EXPECT_LT(halfUp.count + halfEven.count + bar.count + syncScreen.count + pluginTop.count + pluginBottom.count,
            compared / 100);
}

// The ties pinned: a book exactly an eighth, a half and so on through, as each screen rounded it.
TEST(ProgressPpb, EpubTiesRoundAsTheyDid) {
  // Chapters of 125, 375 and 500 bytes: the second starts 12.5 % into the book, a float lands on it exactly.
  const ProgressPpb eighth = ProgressPpb::across(125, 375, 1000, ProgressPpb());
  const float f = floatBookProgress(125, 375, 1000, 0.0f);
  EXPECT_EQ(eighth.percentHalfUp(), static_cast<int>(f * 100.0f + 0.5f));  // 13: progress byte, menu, bars, search
  EXPECT_EQ(std::to_string(eighth.percentHalfEven()), format("%.0f", f * 100));  // 12: status bar, sleep overlay
  EXPECT_EQ(eighth.percentHalfEven(), 12);
  EXPECT_EQ(hundredths(eighth.basisPointsHalfEven()), format("%.2f", f * 100));  // 12.50
  // 14.5 %: the float lands on the half too, and "%.0f" printed 14.
  const ProgressPpb p145 = ProgressPpb::across(29, 1, 200, ProgressPpb());
  EXPECT_EQ(std::to_string(p145.percentHalfEven()), format("%.0f", floatBookProgress(29, 1, 200, 0.0f) * 100));
  EXPECT_EQ(p145.percentHalfEven(), 14);
  EXPECT_EQ(p145.percentHalfUp(), 15);
  // A value just past a half is no tie: 12.500001 % shows 13 everywhere.
  const ProgressPpb past = ProgressPpb::across(125000010, 1, 1000000000, ProgressPpb());
  EXPECT_EQ(past.percentHalfEven(), 13);
  EXPECT_EQ(format("%.0f", floatBookProgress(125000010, 1, 1000000000, 0.0f) * 100), "13");
}

// The text and Markdown readers' status bar: "%.0f" of `(page + 1) * 100.0f / total`, and the chapter bar,
// `(int)(page / total * 100)`, for every page of every book up to 3000 pages. The text matches everywhere; the bar
// differs only where the float landed just under a whole percent (53/100 was 52.999998) and floored to the one below.
TEST(ProgressPpb, PagePercentsMatchTheFloatFormulas) {
  Mismatches text, chapterBar;
  for (int total = 1; total <= 3000; ++total) {
    for (int page = 0; page < total; ++page) {
      const std::string where = std::to_string(page + 1) + "/" + std::to_string(total);
      const long double exact = static_cast<long double>(page + 1) * 100 / total;
      if (format("%.0f", (page + 1) * 100.0f / total) !=
          std::to_string(ProgressPpb::of(page + 1, total).percentHalfEven())) {
        text.note(where, exact, 0);
      }
      const int floatBar = std::clamp(static_cast<int>((static_cast<float>(page + 1) / total) * 100), 0, 100);
      if (floatBar != ProgressPpb::of(page + 1, total).percentFloor()) {
        chapterBar.note(where, exact, 0);  // only on an exact whole percent
        EXPECT_EQ(floatBar + 1, ProgressPpb::of(page + 1, total).percentFloor()) << where;
      }
    }
  }
  EXPECT_EQ(text.count, 0) << text.first;
}

// The doubles handed to the translated "%.0f" and "%.2f" print back the digits they were made from.
TEST(PrintfDouble, PrintsBackItsDigits) {
  for (uint32_t n = 0; n <= 200000; ++n) {
    ASSERT_EQ(format("%.0f", printfDouble(n)), std::to_string(n));
  }
  for (const uint32_t n : {4194304u, 9007199u, 123456789u, 4294967295u}) {
    EXPECT_EQ(format("%.0f", printfDouble(n)), std::to_string(n));
  }
  for (uint32_t bp = 0; bp <= 10000; ++bp) {
    ASSERT_EQ(format("%.2f", printfDouble(bp, 100)), hundredths(bp));
  }
  EXPECT_EQ(printfDouble(7), 7.0);
  EXPECT_EQ(printfDouble(0), 0.0);
}
