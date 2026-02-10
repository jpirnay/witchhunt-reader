#include "BookMetadataActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <string>
#include <vector>

struct BmpData {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> data;
};

BmpData loadBmp(const std::string& path) {
  BmpData bmp;
  FsFile file = Storage.open(path.c_str(), O_RDONLY);
  if (!file) return bmp;
  uint8_t header[54];
  if (file.read(header, 54) != 54) {
    file.close();
    return bmp;
  }
  if (header[0] != 'B' || header[1] != 'M') {
    file.close();
    return bmp;
  }
  uint32_t dataOffset = *(uint32_t*)&header[10];
  bmp.width = *(int32_t*)&header[18];
  bmp.height = *(int32_t*)&header[22];
  uint16_t bitsPerPixel = *(uint16_t*)&header[28];
  if (bitsPerPixel != 1) {
    file.close();
    return bmp;
  }
  file.seek(dataOffset);
  size_t rowSize = (bmp.width + 7) / 8;
  size_t dataSize = rowSize * bmp.height;
  bmp.data.resize(dataSize);
  if (file.read(bmp.data.data(), dataSize) != dataSize) {
    bmp.data.clear();
  }
  file.close();
  return bmp;
}

#include "MappedInputManager.h"

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
#include "Xtc.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr int COVER_HEIGHT = 300;
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
      metadata.coverBmpPath = epub.getThumbBmpPath(COVER_HEIGHT);
    }
  } else if (StringUtils::checkFileExtension(filename, ".xtch") ||
             StringUtils::checkFileExtension(filename, ".xtc")) {
    Xtc xtc(filePath, "/.crosspoint");
    if (xtc.load()) {
      metadata.title = xtc.getTitle();
      metadata.author = xtc.getAuthor();
      metadata.coverBmpPath = xtc.getThumbBmpPath(COVER_HEIGHT);
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
  Serial.printf("[%lu] [BMA] onEnter called\n", millis());
  ActivityWithSubactivity::onEnter();

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
    Serial.printf("[%lu] [BMA] Initial render done\n", millis());
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    exitActivity();
    return;
  }
}

void BookMetadataActivity::displayTaskLoop() {
  while (true) {
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
  Serial.printf("[%lu] [BMA] Starting render\n", millis());
  renderer.clearScreen();
  Serial.printf("[%lu] [BMA] Screen cleared\n", millis());
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  Serial.printf("[%lu] [BMA] Clear buffer displayed\n", millis());

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  // Draw header
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Book Information");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentLeft = metrics.contentSidePadding;

  // Draw cover if available
  if (!metadata.coverBmpPath.empty()) {
    auto bmp = loadBmp(metadata.coverBmpPath);
    if (!bmp.data.empty()) {
      int coverX = pageWidth - bmp.width - 10;  // Adjust position based on actual width
      int coverY = contentTop + 10;
      renderer.drawImage(bmp.data.data(), coverX, coverY, bmp.width, bmp.height);
    }
  }

  int currentY = contentTop;

  // Draw text information
  const int textX = contentLeft;
  currentY = contentTop;

  // Title
  std::string titleStr = "Title: " + metadata.title;
  if (titleStr.length() > 60) titleStr = titleStr.substr(0, 57) + "...";
  renderer.drawText(UI_12_FONT_ID, textX, currentY, titleStr.c_str());
  currentY += LINE_HEIGHT;

  // Author
  std::string authorStr = "Author: " + metadata.author;
  if (authorStr.length() > 60) authorStr = authorStr.substr(0, 57) + "...";
  renderer.drawText(UI_12_FONT_ID, textX, currentY, authorStr.c_str());
  currentY += LINE_HEIGHT;

  // Series
  if (!metadata.series.empty()) {
    std::string seriesStr = "Series: " + metadata.series;
    if (seriesStr.length() > 60) seriesStr = seriesStr.substr(0, 57) + "...";
    renderer.drawText(UI_12_FONT_ID, textX, currentY, seriesStr.c_str());
    currentY += LINE_HEIGHT;

    // Series Index
    if (!metadata.seriesIndex.empty()) {
      std::string seriesIndexStr = "Series Index: " + metadata.seriesIndex;
      if (seriesIndexStr.length() > 20) seriesIndexStr = seriesIndexStr.substr(0, 17) + "...";
      renderer.drawText(UI_12_FONT_ID, textX, currentY, seriesIndexStr.c_str());
      currentY += LINE_HEIGHT;
    }
  }

  // Size
  std::string sizeStr = "Size: " + metadata.size;
  if (sizeStr.length() > 30) sizeStr = sizeStr.substr(0, 27) + "...";
  renderer.drawText(UI_12_FONT_ID, textX, currentY, sizeStr.c_str());
  currentY += LINE_HEIGHT * 2;  // Extra space before description

  // Description
  if (!metadata.description.empty()) {
    renderer.drawText(UI_10_FONT_ID, textX, currentY, "Description:");
    currentY += LINE_HEIGHT;
    auto lines = splitText(metadata.description, 60);
    for (size_t i = 0; i < lines.size() && i < 3; ++i) {
      renderer.drawText(UI_10_FONT_ID, textX, currentY, lines[i].c_str());
      currentY += LINE_HEIGHT;
    }
  }

  // Help text
  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  Serial.printf("[%lu] [BMA] Displaying buffer\n", millis());
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  Serial.printf("[%lu] [BMA] Render complete\n", millis());
}