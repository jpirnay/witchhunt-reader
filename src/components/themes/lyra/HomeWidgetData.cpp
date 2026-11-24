#include "HomeWidgetData.h"

#include <FsHelpers.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WeatherClient.h>
#include <WeatherSettingsStore.h>
#include <esp_system.h>

#include <cstring>
#include <ctime>
#include <string_view>

#include "CrossPointSettings.h"
#include "ReadingStats.h"

namespace {

using HomeTilesLayout::Widget;

bool uses(const HomeTilesLayout::Config& config, const Widget widget) {
  for (int r = 0; r < config.rowCount; ++r) {
    for (int c = 0; c < config.rows[r].count; ++c) {
      if (config.rows[r].cells[c].widget == widget) return true;
    }
  }
  return false;
}

// Whether a Picture widget of `config` shows picture `slot`. A slot no widget shows is not looked
// up: every layout names a source for both, and each costs SD opens on every return to Home.
bool showsPicture(const HomeTilesLayout::Config& config, const int slot) {
  for (int r = 0; r < config.rowCount; ++r) {
    for (int c = 0; c < config.rows[r].count; ++c) {
      const HomeTilesLayout::Cell& cell = config.rows[r].cells[c];
      if (cell.widget == Widget::Picture && cell.option == slot) return true;
    }
  }
  return false;
}

// The weekday (tm_wday) the setting starts the week on.
int weekStart() {
  switch (SETTINGS.firstDayOfWeek) {
    case CrossPointSettings::WEEK_STARTS_SUNDAY:
      return 0;
    case CrossPointSettings::WEEK_STARTS_SATURDAY:
      return 6;
    default:
      return 1;
  }
}

int daysIn(const int year, const int month) {
  static constexpr uint8_t kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return month == 1 && leap ? 29 : kDays[month];
}

bool isPicture(const std::string_view name) {
  return FsHelpers::hasBmpExtension(name) || FsHelpers::hasJpgExtension(name) || FsHelpers::hasPngExtension(name);
}

uint16_t minutesOf(const uint32_t seconds) {
  return static_cast<uint16_t>(seconds > 65535u * 60u ? 65535u : seconds / 60);
}

// Whether `file` sits directly in `folder`: chosen from it, rather than from the folder a layout named before.
bool inFolder(const std::string& file, const std::string& folder) {
  const size_t n = folder.size() - (folder.back() == '/' ? 1 : 0);
  return file.size() > n + 1 && file.compare(0, n, folder, 0, n) == 0 && file[n] == '/' &&
         file.find('/', n + 1) == std::string::npos;
}

// One picture of `folder`, each with the same chance, in one pass and without a list: the k-th kept with
// probability 1/k. Empty when the folder has none.
std::string pickFrom(const std::string& folder) {
  auto dir = Storage.open(folder.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return {};
  }
  char chosen[256] = "";
  uint32_t seen = 0;
  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool candidate = !file.isDirectory();
    if (candidate) file.getName(name, sizeof(name));
    file.close();
    if (!candidate || name[0] == '.' || !isPicture(name)) continue;
    ++seen;
    if (esp_random() % seen == 0) memcpy(chosen, name, sizeof(chosen));
  }
  dir.close();
  if (chosen[0] == '\0') return {};
  std::string path = folder;
  if (path.back() != '/') path += '/';
  return path += chosen;
}

}  // namespace

std::string homePictureTarget(const std::string& source, const uint32_t sourceBytes, const int width,
                              const int height) {
  // FNV-1a over the path and the size.
  uint32_t hash = 2166136261u;
  for (const char c : source) hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
  for (int i = 0; i < 4; ++i) hash = (hash ^ ((sourceBytes >> (8 * i)) & 0xFF)) * 16777619u;
  char name[64];
  snprintf(name, sizeof(name), "%s/pic_%08x_%dx%d.bmp", kHomePictureCache, static_cast<unsigned>(hash), width, height);
  return name;
}

void HomeWidgetData::load(const HomeTilesLayout::Config& config, const std::string* pictureSources) {
  const bool wantsDate = uses(config, Widget::Calendar);
  const bool wantsReading = uses(config, Widget::Summary) || wantsDate;

  // The date: the clock set, and not merely restored from a stale backup (HalClock::isStaleRestore).
  dateKnown = false;
  readingKnown = false;
  const bool clockSet = HalClock::isSynced() && !HalClock::isStaleRestore();
  struct tm now{};
  if (clockSet) {
    const time_t epoch = HalClock::now();
    localtime_r(&epoch, &now);
    weekStartsOn = weekStart();
    year = now.tm_year + 1900;
    month = now.tm_mon;
    today = now.tm_mday;
    daysInMonth = daysIn(year, month);
    const int firstWeekday = ((now.tm_wday - (today - 1)) % 7 + 7) % 7;
    firstColumn = (firstWeekday - weekStartsOn + 7) % 7;
    dateKnown = true;
  }

  if (wantsReading && dateKnown) {
    const uint16_t todayIndex = currentLocalDayIndex();
    ReadingStatsStore::Summary summary;
    if (todayIndex != 0 && READING_STATS.querySummary(summary) == ReadingStatsStore::ReadResult::Ok) {
      const int intoWeek = (now.tm_wday - weekStartsOn + 7) % 7;
      weekSeconds = 0;
      for (int i = 0; i < 7; ++i) {
        const int day = static_cast<int>(todayIndex) - intoWeek + i;
        const uint32_t seconds =
            day > 0 && i <= intoWeek ? ReadingStatsStore::secondsOn(summary.globalDays, static_cast<uint16_t>(day)) : 0;
        weekMinutes[i] = minutesOf(seconds);
        weekSeconds += seconds;
      }
      for (int d = 1; d <= daysInMonth; ++d) {
        const int day = static_cast<int>(todayIndex) - (today - d);
        monthMinutes[d - 1] =
            d <= today && day > 0
                ? minutesOf(ReadingStatsStore::secondsOn(summary.globalDays, static_cast<uint16_t>(day)))
                : 0;
      }
      streak = ReadingStatsStore::currentStreakIn(summary.globalDays, todayIndex);
      finished = summary.finishedBookCount;
      reading = summary.bookCount > summary.finishedBookCount ? summary.bookCount - summary.finishedBookCount : 0;
      readingKnown = true;
    } else {
      LOG_DBG("HOME", "Widgets: no reading stats");
    }
  }

  weatherKnown = false;
  if (uses(config, Widget::Weather)) {
    // From the cache only: Home never goes online for a widget (getWeather does not fetch unless forced).
    const WeatherData data = WeatherClient::getWeather(WEATHER_SETTINGS, false);
    if (data.valid) {
      temperature = data.current.temperature;
      weatherCode = data.current.weatherCode;
      isDay = data.current.isDay;
      if (!data.daily.empty()) {
        high = data.daily[0].tempMax;
        low = data.daily[0].tempMin;
      } else {
        high = low = temperature;
      }
      weatherKnown = true;
    }
  }

  for (int i = 0; i < HomeTilesLayout::kMaxPictures; ++i) {
    const std::string& source = pictureSources[i];
    pictureSizes[i] = 0;
    // A slot no widget shows keeps its folder's choice for when a layout shows it again.
    if (source.empty() || !showsPicture(config, i)) continue;
    if (isPicture(source)) {
      pictures[i] = source;
    } else if (!inFolder(pictures[i], source) || !Storage.exists(pictures[i].c_str())) {
      // A folder: one of its pictures, kept while it is on the card, so Home does not change it on
      // every return. Chosen again when the layout names another folder or the file has gone; an
      // empty folder is looked at again on each return, so a picture added to it shows.
      pictures[i] = pickFrom(source);
    }
    if (!pictures[i].empty()) {
      auto file = Storage.open(pictures[i].c_str());
      if (file) {
        pictureSizes[i] = static_cast<uint32_t>(file.size());
        file.close();
      }
    }
  }
}
