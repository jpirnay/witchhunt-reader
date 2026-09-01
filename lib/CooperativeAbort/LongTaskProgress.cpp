#include "LongTaskProgress.h"

#include <Arduino.h>

namespace LongTaskProgress {
namespace {
Handler gHandler = nullptr;
const char* gStage = nullptr;
uint32_t gLastAliveMs = 0;
bool gEverAlive = false;
}  // namespace

void setHandler(const Handler handler) { gHandler = handler; }

void step(const char* stage) {
  if (stage != nullptr) {
    gStage = stage;
  }
  if (gHandler != nullptr && stage != nullptr) {
    gHandler(stage);
  }
}

void noteAlive() {
  // One millis() read per yield. The JPEG decoder polls per MCU block, so this runs
  // thousands of times in a decode — millis() is an esp_timer read of about a microsecond,
  // which is noise next to the block it is measuring.
  gLastAliveMs = millis();
  gEverAlive = true;
}

uint32_t msSinceAlive() { return gEverAlive ? millis() - gLastAliveMs : 0; }

const char* currentStage() { return gStage; }

}  // namespace LongTaskProgress
