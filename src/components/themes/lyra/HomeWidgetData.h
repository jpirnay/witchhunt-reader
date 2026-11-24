#pragma once

#include <cstdint>
#include <string>

#include "components/themes/lyra/HomeTilesLayout.h"

// What Home's widgets show that is not the book list: the week's and the month's reading, the last
// forecast and the pictures. Read once when Home opens (LyraTilesTheme::prepareHome), on the loop
// task, and only what the layout uses; the draw calls on the render task only read it. The clock
// widget reads the time when it draws.
struct HomeWidgetData {
  // ---- Reading stats. A day is a day of the clock, so they are known only once it is set. ----
  bool readingKnown = false;
  uint16_t weekMinutes[7]{};  // from the first day of the week (CrossPointSettings::firstDayOfWeek)
  uint32_t weekSeconds = 0;
  uint16_t streak = 0;
  uint32_t finished = 0;
  uint32_t reading = 0;  // started and not finished

  // ---- The month the calendar shows ----
  bool dateKnown = false;
  int year = 0;
  int month = 0;        // 0 to 11
  int today = 0;        // day of the month, 1 to 31
  int firstColumn = 0;  // the column of the 1st, counted from the first day of the week
  int daysInMonth = 0;
  int weekStartsOn = 1;  // tm_wday of the first column
  uint16_t monthMinutes[31]{};

  // ---- The last forecast the Weather screen fetched ----
  bool weatherKnown = false;
  int temperature = 0;  // tenths of a degree, in the unit the weather settings name
  int high = 0;
  int low = 0;
  int weatherCode = 0;
  bool isDay = true;

  // ---- The pictures: the file each Picture widget shows, a folder's chosen once per start ----
  std::string pictures[HomeTilesLayout::kMaxPictures];
  uint32_t pictureSizes[HomeTilesLayout::kMaxPictures]{};  // bytes, so a replaced file is made again

  // Reads what `config` uses. `pictureSources` names each picture's file or folder; a folder is
  // read once per start (`pickedSources` keeps the choice, so a return to Home does not change it).
  void load(const HomeTilesLayout::Config& config, const std::string* pictureSources, std::string* pickedSources,
            std::string* pickedFiles);
};

// The folder Home's ready-made layouts take their pictures from.
inline constexpr const char* kHomePicturesFolder = "/home";
// Where Home keeps the pictures it made from them, each at a widget's size.
inline constexpr const char* kHomePictureCache = "/.crosspoint/home";

// The file a picture is made into for a `width` x `height` widget: named by the source's path and
// size, so a replaced picture is made again and two widget sizes keep a file each.
std::string homePictureTarget(const std::string& source, uint32_t sourceBytes, int width, int height);
