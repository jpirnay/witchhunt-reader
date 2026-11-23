#pragma once

#include <string>

// What the screen on top tells a screenshot to name its file after (screenshot_name::build). The default, no title,
// keeps the plain screenshot-<millis>.bmp: only the readers fill it in (Activity::screenshotInfo).
//
// Ported from crosspoint-reader PR #1589 (Jon Stieglitz).
struct ScreenshotInfo {
  std::string title;  // the open book's title as the book gives it; made safe for FAT when the name is built
  int chapter = 0;    // 1-based: the EPUB spine item on screen; 0 for a format without chapters
  int page = 0;       // 1-based: the page in that chapter (EPUB) or in the book; 0 when unknown
  int percent = -1;   // whole-book progress, 0..100; -1 when unknown
};
