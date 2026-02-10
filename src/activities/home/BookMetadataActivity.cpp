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

  Serial.printf("[%lu] [BMA] extractMetadata: filePath=%s\n", millis(), filePath.c_str());

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
  Serial.printf("[%lu] [BMA] extractMetadata: filename=%s\n", millis(), filename.c_str());

  // Try to load metadata based on file type
  if (StringUtils::checkFileExtension(filename, ".epub")) {
    Serial.printf("[%lu] [BMA] Detected EPUB\n", millis());
    Epub epub(filePath, "/.crosspoint");
    if (epub.load(false)) {  // Don't load CSS for metadata only
      Serial.printf("[%lu] [BMA] Epub load OK\n", millis());
      metadata.title = epub.getTitle();
      metadata.author = epub.getAuthor();
      // Generate thumbnail only if needed and verify it exists
      const std::string thumbPath = epub.getThumbBmpPath(THUMB_HEIGHT);
      Serial.printf("[%lu] [BMA] Expected thumb path: %s\n", millis(), thumbPath.c_str());
      if (!Storage.exists(thumbPath.c_str())) {
        Serial.printf("[%lu] [BMA] Thumbnail missing; attempting generation\n", millis());
        if (epub.generateThumbBmp(THUMB_HEIGHT)) {
          Serial.printf("[%lu] [BMA] Thumbnail generation succeeded\n", millis());
        } else {
          Serial.printf("[%lu] [BMA] Thumbnail generation failed\n", millis());
        }
      }

      if (Storage.exists(thumbPath.c_str())) {
        metadata.coverBmpPath = thumbPath;
        Serial.printf("[%lu] [BMA] Using thumbnail: %s\n", millis(), thumbPath.c_str());
      }
    } else {
      Serial.printf("[%lu] [BMA] Epub load failed\n", millis());
    }
  } else if (StringUtils::checkFileExtension(filename, ".xtch") ||
             StringUtils::checkFileExtension(filename, ".xtc")) {
    Serial.printf("[%lu] [BMA] Detected XTC\n", millis());
    Xtc xtc(filePath, "/.crosspoint");
    if (xtc.load()) {
      Serial.printf("[%lu] [BMA] XTC load OK\n", millis());
      metadata.title = xtc.getTitle();
      metadata.author = xtc.getAuthor();
      const std::string thumbPath = xtc.getThumbBmpPath(THUMB_HEIGHT);
      Serial.printf("[%lu] [BMA] Expected XTC thumb path: %s\n", millis(), thumbPath.c_str());
      if (!Storage.exists(thumbPath.c_str())) {
        Serial.printf("[%lu] [BMA] XTC thumbnail missing; attempting generation\n", millis());
        if (xtc.generateThumbBmp(THUMB_HEIGHT)) {
          Serial.printf("[%lu] [BMA] XTC thumbnail generation succeeded\n", millis());
        } else {
          Serial.printf("[%lu] [BMA] XTC thumbnail generation failed\n", millis());
        }
      }

      if (Storage.exists(thumbPath.c_str())) {
        metadata.coverBmpPath = thumbPath;
        Serial.printf("[%lu] [BMA] Using XTC thumbnail: %s\n", millis(), thumbPath.c_str());
      }
    } else {
      Serial.printf("[%lu] [BMA] XTC load failed\n", millis());
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
  updateRequired = false;  // don't trigger until task is ready
  initialRenderDone = false;

  // Ignore input for a short time to avoid stale button events
  inputIgnoreUntilMs = millis() + 250;

  xTaskCreate(&BookMetadataActivity::taskTrampoline, "BookMetadataActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Ensure an update is requested after task creation in case of a scheduling race
  updateRequired = true;
  Serial.printf("[%lu] [BMA] Display task created: handle=%p; input ignored until %lu\n", millis(), (void*)displayTaskHandle,
                inputIgnoreUntilMs);
}

void BookMetadataActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Signal the display task to exit
  shouldExit = true;

  // Wait up to 3 seconds for the display task to clear its handle and exit
  const unsigned long start = millis();
  while (displayTaskHandle != nullptr && (millis() - start) < 3000) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  if (displayTaskHandle != nullptr) {
    // As a last resort, force-delete (risky) but log for diagnostics
    Serial.printf("[%lu] [BMA] WARNING: Display task didn't exit within 3s; forcing delete handle=%p\n", millis(), (void*)displayTaskHandle);
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  } else {
    Serial.printf("[%lu] [BMA] Display task exited cleanly\n", millis());
  }

  // Nothing else to delete — simplified design avoids mutexes/semaphores
}

void BookMetadataActivity::loop() {
  // Ignore early input to avoid stale button releases from parent activity
  if (millis() < inputIgnoreUntilMs) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      Serial.printf("[%lu] [BMA] Ignored Back release until %lu\n", millis(), inputIgnoreUntilMs);
    }
    return;
  }

  // Watchdog: detect stuck render and request exit
  if (renderInProgress && (millis() - lastRenderStartMs) > RENDER_WATCHDOG_MS) {
    if (!forceDeleteWanted) {
      forceDeleteWanted = true;
      Serial.printf("[%lu] [BMA] Watchdog: render stuck for %lu ms, requesting exit\n", millis(), millis() - lastRenderStartMs);
      // Trigger activity exit to recover; onExit will eventually force-delete if needed
      exitActivity();
      return;  // don't run further logic after requesting exit
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    Serial.printf("[%lu] [BMA] Back released -> exitActivity()\n", millis());
    exitActivity();
    return;
  }
}

void BookMetadataActivity::displayTaskLoop() {
  Serial.printf("[%lu] [BMA] Display task started\n", millis());
  while (!shouldExit) {
    if (updateRequired) {
      updateRequired = false;
      Serial.printf("[%lu] [BMA] Running render (updateRequired)\n", millis());
      // Watchdog: mark render start
      renderInProgress = true;
      lastRenderStartMs = millis();
      render();
      // Watchdog: mark render end
      lastRenderEndMs = millis();
      renderInProgress = false;

      // Mark that initial render has completed so the main loop doesn't do a duplicate
      if (!initialRenderDone) {
        initialRenderDone = true;
        log("BMA", "Initial render done");
      }
      Serial.printf("[%lu] [BMA] Render finished (duration %lu ms)\n", millis(), lastRenderEndMs - lastRenderStartMs);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  // Before exiting, clear the task handle so the owner can detect exit
  displayTaskHandle = nullptr;
  Serial.printf("[%lu] [BMA] Display task exiting and will self-delete\n", millis());
  // Self-delete to avoid races with forced deletion from onExit
  vTaskDelete(NULL);
}

void BookMetadataActivity::render() const {
  log("BMA", "Starting render");

  // Ensure we render in BW mode for UI screens (readers may set grayscale modes)
  renderer.setRenderMode(GfxRenderer::BW);
  Serial.printf("[%lu] [BMA] Forcing renderer to BW mode for metadata screen\n", millis());

  renderer.clearScreen();
  log("BMA", "Screen cleared");

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  // If this is the very first render, prefer to perform a full refresh to avoid ghosting
  const bool firstRender = !initialRenderDone;


  // Draw header
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Book Information");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentLeft = metrics.contentSidePadding;
  int maxWidth = pageWidth - 2 * contentLeft;


  // Draw cover if available
  if (!metadata.coverBmpPath.empty()) {
    log("BMA", ("Loading cover: " + metadata.coverBmpPath).c_str());

    // Check existence explicitly to provide clearer logs and a visual fallback
    if (!Storage.exists(metadata.coverBmpPath.c_str())) {
      Serial.printf("[%lu] [BMA] Cover file missing on disk: %s\n", millis(), metadata.coverBmpPath.c_str());
      // Draw a placeholder book card so users know a cover was expected
      int bookX = pageWidth - COVER_WIDTH - 10;  // 10px margin from right
      int bookY = contentTop + 10;
      renderer.fillRoundedRect(bookX, bookY, COVER_WIDTH, COVER_HEIGHT, 8, Color::LightGray);
      renderer.drawCenteredText(UI_12_FONT_ID, bookY + COVER_HEIGHT / 2, "No cover");
    } else {
      FsFile file;
      if (Storage.openFileForRead("BMA", metadata.coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          // Defensive checks on bitmap size
          const int bmpW = bitmap.getWidth();
          const int bmpH = bitmap.getHeight();
          Serial.printf("[%lu] [BMA] Bitmap size: %dx%d\n", millis(), bmpW, bmpH);
          // Log bit-depth to detect if we're handling 1-bit vs grayscale BMPs
          Serial.printf("[%lu] [BMA] Bitmap bpp=%d is1Bit=%s hasGreyscale=%s\n", millis(), bitmap.getBpp(),
                        bitmap.is1Bit() ? "true" : "false", bitmap.hasGreyscale() ? "true" : "false");
          if (bmpW <= 0 || bmpH <= 0) {
            Serial.printf("[%lu] [BMA] Invalid bitmap size, skipping cover\n", millis());
          } else {
            int bookX = pageWidth - COVER_WIDTH - 10;  // 10px margin from right
            int bookY = contentTop + 10;
            int bookWidth = COVER_WIDTH;
            int bookHeight = COVER_HEIGHT;

            // Calculate position to center image within the book card
            int coverX = bookX;
            int coverY = bookY;

            if (bmpW > bookWidth || bmpH > bookHeight) {
              const float imgRatio = static_cast<float>(bmpW) / static_cast<float>(bmpH);
              const float boxRatio = static_cast<float>(bookWidth) / static_cast<float>(bookHeight);

              if (imgRatio > boxRatio) {
                coverX = bookX;
                coverY = bookY + (bookHeight - static_cast<int>(bookWidth / imgRatio)) / 2;
              } else {
                coverX = bookX + (bookWidth - static_cast<int>(bookHeight * imgRatio)) / 2;
                coverY = bookY;
              }
            } else {
              coverX = bookX + (bookWidth - bmpW) / 2;
              coverY = bookY + (bookHeight - bmpH) / 2;
            }

            // Clamp coordinates to page bounds
            coverX = std::max(bookX, std::min(coverX, pageWidth - bookWidth));
            coverY = std::max(bookY, std::min(coverY, pageHeight - bookHeight));

            Serial.printf("[%lu] [BMA] Drawing bitmap at %d,%d size %dx%d\n", millis(), coverX, coverY, bookWidth, bookHeight);

            // Draw the cover image centered within the book card
            renderer.drawBitmap(bitmap, coverX, coverY, bookWidth, bookHeight);
            log("BMA", "Cover rendered");
          }
        } else {
          log("BMA", "BMP parse failed");
        }
        file.close();
      } else {
        log("BMA", "Cover file not open");
      }
    }
  }

  int currentY;
  if (!metadata.coverBmpPath.empty()) {
    currentY = contentTop + COVER_HEIGHT + 20;  // Start text below the cover
  } else {
    currentY = contentTop + 20;
  }

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
    if (currentY + LINE_HEIGHT < pageHeight - metrics.buttonHintsHeight) {
      renderer.drawText(UI_10_FONT_ID, textX, currentY, "Description:");
      currentY += LINE_HEIGHT;
      auto lines = splitText(metadata.description, 60);
      for (size_t i = 0; i < lines.size() && i < 3; ++i) {
        if (currentY + LINE_HEIGHT >= pageHeight - metrics.buttonHintsHeight) {
          Serial.printf("[%lu] [BMA] Skipping description line, out of space at Y=%d\n", millis(), currentY);
          break;
        }
        std::string truncatedLine = renderer.truncatedText(UI_10_FONT_ID, lines[i].c_str(), maxWidth, EpdFontFamily::REGULAR);
        renderer.drawText(UI_10_FONT_ID, textX, currentY, truncatedLine.c_str());
        currentY += LINE_HEIGHT;
      }
    } else {
      Serial.printf("[%lu] [BMA] Not enough space for description header at Y=%d\n", millis(), currentY);
    }
  }

  // Help text
  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  log("BMA", "Displaying buffer");
  // Use full refresh on first render to clear leftover artifacts; subsequent renders use fast refresh
  renderer.displayBuffer(firstRender ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  log("BMA", "Render complete");
}