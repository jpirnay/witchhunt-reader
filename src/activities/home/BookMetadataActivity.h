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
  SemaphoreHandle_t renderingMutex = nullptr;

  BookMetadata metadata;
  bool updateRequired = false;
  bool initialRenderDone = false;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

  // Extract metadata from book file
  static BookMetadata extractMetadata(const std::string& filePath);

 public:
  explicit BookMetadataActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);
  void onEnter() override;
  void onExit() override;
  void loop() override;
};