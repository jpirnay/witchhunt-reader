#include "BookMetadataActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "Xtc.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

static void log(const char* tag, const char* message) {
  Serial.printf("[%lu] [%s] %s\n", millis(), tag, message);
}

std::vector<std::string> splitText(const std::string& text, size_t maxLen) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.length()) {
    size_t end = start + maxLen;
    if (end >= text.length()) {
      lines.push_back(text.substr(start));
      break;
    }
    // Find last space before end
    size_t space = text.rfind(' ', end);
    if (space != std::string::npos && space > start) {
      lines.push_back(text.substr(start, space - start));
      start = space + 1;
    } else {
      lines.push_back(text.substr(start, maxLen));
      start += maxLen;
    }
  }
  return lines;
}

namespace {
constexpr int COVER_WIDTH = 200;
constexpr int COVER_HEIGHT = 300;
constexpr int THUMB_HEIGHT = 300;
constexpr int TEXT_START_Y = 50;
constexpr int LINE_HEIGHT = 35;
}  // namespace

void BookMetadataActivity::taskTrampoline(void* param) {
  auto* self = static_cast<BookMetadataActivity*>(param);
  self->displayTaskLoop();
}

BookMetadata BookMetadataActivity::extractMetadata(const std::string& filePath) {
  BookMetadata metadata;
  metadata.filePath = filePath;

  // Get file size
  auto file = Storage.open(filePath.c_str());
  if (file) {
    size_t fileSize = file.size();
    file.close();

    // Format file size
    if (fileSize < 1024) {
      metadata.size = std::to_string(fileSize) + " B";
    } else if (fileSize < 1024 * 1024) {
      metadata.size = std::to_string(fileSize / 1024) + " KB";
    } else {
      metadata.size = std::to_string(fileSize / (1024 * 1024)) + " MB";
    }
  } else {
    metadata.size = "Unknown";
  }

  // Extract filename for fallback
  std::string filename = filePath;
  const size_t lastSlash = filename.find_last_of('/');
  if (lastSlash != std::string::npos) {
    filename = filename.substr(lastSlash + 1);
  }

  // Try to load metadata based on file type
  if (StringUtils::checkFileExtension(filename, ".epub")) {
    Epub epub(filePath, "/.crosspoint");
    if (epub.load(false)) {  // Don't load CSS for metadata only
      metadata.title = epub.getTitle();
      metadata.author = epub.getAuthor();
      epub.generateThumbBmp(THUMB_HEIGHT);
      metadata.coverBmpPath = epub.getThumbBmpPath(THUMB_HEIGHT);
    }
  } else if (StringUtils::checkFileExtension(filename, ".xtch") ||
             StringUtils::checkFileExtension(filename, ".xtc")) {
    Xtc xtc(filePath, "/.crosspoint");
    if (xtc.load()) {
      metadata.title = xtc.getTitle();
      metadata.author = xtc.getAuthor();
      xtc.generateThumbBmp(THUMB_HEIGHT);
      metadata.coverBmpPath = xtc.getThumbBmpPath(THUMB_HEIGHT);
    }
  }

  // Fallback to filename if no title
  if (metadata.title.empty()) {
    // Remove extension
    size_t dotPos = filename.find_last_of('.');
    if (dotPos != std::string::npos) {
      metadata.title = filename.substr(0, dotPos);
    } else {
      metadata.title = filename;
    }
  }

  // Set defaults for missing fields
  if (metadata.author.empty()) {
    metadata.author = "Unknown";
  }
  if (metadata.series.empty()) {
    metadata.series = "N/A";
  }
  if (metadata.seriesIndex.empty()) {
    metadata.seriesIndex = "N/A";
  }
  if (metadata.description.empty()) {
    metadata.description = "No description available";
  }

  return metadata;
}

BookMetadataActivity::BookMetadataActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath)
    : ActivityWithSubactivity("BookMetadata", renderer, mappedInput), metadata(extractMetadata(filePath)), initialRenderDone(false) {}

void BookMetadataActivity::onEnter() {
  log("BMA", "onEnter called");
  ActivityWithSubactivity::onEnter();

  shouldExit = false;
  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;
  initialRenderDone = false;

  xTaskCreate(&BookMetadataActivity::taskTrampoline, "BookMetadataActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void BookMetadataActivity::onExit() {
  ActivityWithSubactivity::onExit();

  shouldExit = true;
  vTaskDelay(20 / portTICK_PERIOD_MS);  // Give time for task to exit loop

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void BookMetadataActivity::loop() {
  if (!initialRenderDone) {
    initialRenderDone = true;
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    render();
    xSemaphoreGive(renderingMutex);
    log("BMA", "Initial render done");
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    exitActivity();
    return;
  }
}

void BookMetadataActivity::displayTaskLoop() {
  while (!shouldExit) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void BookMetadataActivity::render() const {
  log("BMA", "Starting render");
  renderer.clearScreen();
  log("BMA", "Screen cleared");
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  log("BMA", "Clear buffer displayed");

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  // Draw header
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Book Information");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentLeft = metrics.contentSidePadding;
  int maxWidth = pageWidth - COVER_WIDTH - 10 - contentLeft;

  // Draw cover if available
  if (!metadata.coverBmpPath.empty()) {
    log("BMA", ("Loading cover: " + metadata.coverBmpPath).c_str());
    FsFile file;
    if (Storage.openFileForRead("BMA", metadata.coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        int bookX = pageWidth - COVER_WIDTH - 10;  // 10px margin from right
        int bookY = contentTop + 10;
        int bookWidth = COVER_WIDTH;
        int bookHeight = COVER_HEIGHT;

        // Calculate position to center image within the book card
        int coverX, coverY;

        if (bitmap.getWidth() > bookWidth || bitmap.getHeight() > bookHeight) {
          const float imgRatio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
          const float boxRatio = static_cast<float>(bookWidth) / static_cast<float>(bookHeight);

          if (imgRatio > boxRatio) {
            coverX = bookX;
            coverY = bookY + (bookHeight - static_cast<int>(bookWidth / imgRatio)) / 2;
          } else {
            coverX = bookX + (bookWidth - static_cast<int>(bookHeight * imgRatio)) / 2;
            coverY = bookY;
          }
        } else {
          coverX = bookX + (bookWidth - bitmap.getWidth()) / 2;
          coverY = bookY + (bookHeight - bitmap.getHeight()) / 2;
        }

        // Draw the cover image centered within the book card
        renderer.drawBitmap(bitmap, coverX, coverY, bookWidth, bookHeight);
        log("BMA", "Cover rendered");
      } else {
        log("BMA", "BMP parse failed");
      }
      file.close();
    } else {
      log("BMA", "Cover file not open");
    }
  }

  int currentY = contentTop + COVER_HEIGHT + 20;  // Start text below the cover

  // Draw text information
  const int textX = contentLeft;
  std::string fullTitle = "Title: " + metadata.title;
  std::string truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, fullTitle.c_str(), maxWidth, EpdFontFamily::REGULAR);
  renderer.drawText(UI_12_FONT_ID, textX, currentY, truncatedTitle.c_str());
  currentY += LINE_HEIGHT;

  // Author
  std::string fullAuthor = "Author: " + metadata.author;
  std::string truncatedAuthor = renderer.truncatedText(UI_12_FONT_ID, fullAuthor.c_str(), maxWidth, EpdFontFamily::REGULAR);
  renderer.drawText(UI_12_FONT_ID, textX, currentY, truncatedAuthor.c_str());
  currentY += LINE_HEIGHT;

  // Series
  if (metadata.series != "N/A") {
    std::string fullSeries = "Series: " + metadata.series;
    std::string truncatedSeries = renderer.truncatedText(UI_12_FONT_ID, fullSeries.c_str(), maxWidth, EpdFontFamily::REGULAR);
    renderer.drawText(UI_12_FONT_ID, textX, currentY, truncatedSeries.c_str());
    currentY += LINE_HEIGHT;

    // Series Index
    if (metadata.seriesIndex != "N/A") {
      std::string fullSeriesIndex = "Series Index: " + metadata.seriesIndex;
      std::string truncatedSeriesIndex = renderer.truncatedText(UI_12_FONT_ID, fullSeriesIndex.c_str(), maxWidth, EpdFontFamily::REGULAR);
      renderer.drawText(UI_12_FONT_ID, textX, currentY, truncatedSeriesIndex.c_str());
      currentY += LINE_HEIGHT;
    }
  }

  // Size
  std::string fullSize = "Size: " + metadata.size;
  std::string truncatedSize = renderer.truncatedText(UI_12_FONT_ID, fullSize.c_str(), maxWidth, EpdFontFamily::REGULAR);
  renderer.drawText(UI_12_FONT_ID, textX, currentY, truncatedSize.c_str());
  currentY += LINE_HEIGHT * 2;  // Extra space before description

  // Description
  if (!metadata.description.empty()) {
    renderer.drawText(UI_10_FONT_ID, textX, currentY, "Description:");
    currentY += LINE_HEIGHT;
    auto lines = splitText(metadata.description, 60);
    for (size_t i = 0; i < lines.size() && i < 3; ++i) {
      std::string truncatedLine = renderer.truncatedText(UI_10_FONT_ID, lines[i].c_str(), maxWidth, EpdFontFamily::REGULAR);
      renderer.drawText(UI_10_FONT_ID, textX, currentY, truncatedLine.c_str());
      currentY += LINE_HEIGHT;
    }
  }

  // Help text
  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  log("BMA", "Displaying buffer");
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  log("BMA", "Render complete");
}