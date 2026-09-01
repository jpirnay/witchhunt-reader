#include "HalSpiBus.h"

#include <Arduino.h>
#include <Logging.h>
#include <freertos/task.h>

#include <cstdlib>

namespace {

struct TrackedActivity {
  HalSpiBus::Operation operation = HalSpiBus::Operation::None;
  HalSpiBus::OperationState state = HalSpiBus::OperationState::Idle;
  uint32_t startedMs = 0;
  TaskHandle_t owner = nullptr;
  uint16_t depth = 0;
};

portMUX_TYPE s_activityMux = portMUX_INITIALIZER_UNLOCKED;
TrackedActivity s_activeActivity;
TrackedActivity s_waitingActivity;

}  // namespace

HalSpiBus::HalSpiBus() {
  mutex = xSemaphoreCreateRecursiveMutex();
  if (mutex == nullptr) {
    LOG_ERR("SPI", "Failed to create SPI bus mutex - display/SD access is unserialized");
  }
}

HalSpiBus& HalSpiBus::getInstance() {
  static HalSpiBus spiBus;
  return spiBus;
}

void HalSpiBus::begin() { (void)getInstance(); }

HalSpiBus::ActivitySnapshot HalSpiBus::activitySnapshot() const {
  TrackedActivity activity;
  portENTER_CRITICAL(&s_activityMux);
  activity = s_activeActivity.operation != Operation::None ? s_activeActivity : s_waitingActivity;
  portEXIT_CRITICAL(&s_activityMux);

  ActivitySnapshot snapshot;
  snapshot.operation = activity.operation;
  snapshot.state = activity.state;
  snapshot.elapsedMs = activity.operation == Operation::None ? 0 : millis() - activity.startedMs;
  return snapshot;
}

const char* HalSpiBus::operationName(Operation operation) {
  switch (operation) {
    case Operation::None:
      return "idle";
    case Operation::Sd:
      return "storage";
    case Operation::DisplayInit:
      return "display-init";
    case Operation::DisplayRefresh:
      return "display-refresh";
    case Operation::DisplaySleep:
      return "display-sleep";
    case Operation::DisplayBuffer:
      return "display-buffer";
    case Operation::Count:
      break;
  }
  return "?";
}

HalSpiBus::Lock::Lock(Operation operation) : owner(xTaskGetCurrentTaskHandle()) {
  auto& bus = HalSpiBus::getInstance();
  if (bus.mutex == nullptr) {
    LOG_ERR("SPI", "SPI bus mutex not initialized; refusing unsafe bus access");
    abort();
  }

  portENTER_CRITICAL(&s_activityMux);
  s_waitingActivity = {operation, OperationState::Waiting, millis(), owner, 0};
  portEXIT_CRITICAL(&s_activityMux);

  if (xSemaphoreTakeRecursive(bus.mutex, portMAX_DELAY) != pdTRUE) {
    LOG_ERR("SPI", "SPI bus mutex acquisition failed; refusing unsafe bus access");
    abort();
  }
  acquired = true;

  portENTER_CRITICAL(&s_activityMux);
  if (s_waitingActivity.owner == owner) {
    s_waitingActivity = {};
  }
  if (s_activeActivity.owner == owner) {
    s_activeActivity.depth++;
  } else {
    s_activeActivity = {operation, OperationState::Active, millis(), owner, 1};
  }
  portEXIT_CRITICAL(&s_activityMux);
}

HalSpiBus::Lock::~Lock() {
  if (!acquired) return;
  xSemaphoreGiveRecursive(HalSpiBus::getInstance().mutex);

  portENTER_CRITICAL(&s_activityMux);
  if (s_activeActivity.owner == owner) {
    if (s_activeActivity.depth > 1) {
      s_activeActivity.depth--;
    } else {
      s_activeActivity = {};
    }
  }
  portEXIT_CRITICAL(&s_activityMux);
}
