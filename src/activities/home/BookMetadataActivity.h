#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <string>

#include "../ActivityWithSubactivity.h"

struct BookMetadata {
  std::string title;
  std::string author;
  std::string series;
  std::string seriesIndex;
  std::string size;
  std::string description;
  std::string coverBmpPath;
  std::string filePath;
};

class BookMetadataActivity final : public ActivityWithSubactivity {
 private:
  TaskHandle_t displayTaskHandle = nullptr;

  BookMetadata metadata;
  bool updateRequired = false;
  bool initialRenderDone = false;
  volatile bool shouldExit = false;  // visible to display task

  // Simple watchdog state
  volatile unsigned long lastRenderStartMs = 0;
  volatile unsigned long lastRenderEndMs = 0;
  volatile bool renderInProgress = false;
  volatile bool forceDeleteWanted = false;
  static constexpr unsigned long RENDER_WATCHDOG_MS = 5000;  // ms before considering render stuck

  // Ignore input for a short time after entering to avoid responding to stale events
  unsigned long inputIgnoreUntilMs = 0;  // millis() value until which input is ignored

  static void taskTrampoline(void* param);
  void displayTaskLoop();
  void render() const;

  // Extract metadata from book file
  static BookMetadata extractMetadata(const std::string& filePath);

 public:
  explicit BookMetadataActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool isReadyForReset() const override { return displayTaskHandle == nullptr; }
};