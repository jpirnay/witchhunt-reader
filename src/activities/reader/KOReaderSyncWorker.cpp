#include "KOReaderSyncWorker.h"

#if CROSSPOINT_KOREADER_AUTOSYNC

#include <Arduino.h>
#include <HalClock.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <utility>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"

namespace {
constexpr unsigned long AUTO_CONNECT_TIMEOUT_MS = 10000;

constexpr uint32_t WORKER_IDLE_EXIT_MS = KOReaderAutoSync::MIN_INTERVAL_PUSH_GAP_MS / 2;

SemaphoreHandle_t slotMutex = nullptr;
TaskHandle_t workerTask = nullptr;
KOReaderSyncJob slot;
bool slotBusy = false;
bool jobDone = false;
uint64_t activeSeq = 0;
uint64_t nextSeq = 1;

std::atomic<bool> cachedBusy{false};

void updateCachedBusy() { cachedBusy.store(slotBusy && !jobDone, std::memory_order_relaxed); }

void logInternalHeap(const char* stage) {
  constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  LOG_DBG("KOSyncWorker", "Internal heap[%s]: free=%lu contig=%lu", stage,
          static_cast<unsigned long>(heap_caps_get_free_size(caps)),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(caps)));
}

class SlotLock {
 public:
  SlotLock() { xSemaphoreTake(slotMutex, portMAX_DELAY); }
  ~SlotLock() { xSemaphoreGive(slotMutex); }
  SlotLock(const SlotLock&) = delete;
  SlotLock& operator=(const SlotLock&) = delete;
};

bool connectToSavedNetwork(const unsigned long timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    return true;
  }

  if (const wifi_mode_t mode = WiFi.getMode(); mode != WIFI_MODE_NULL && mode != WIFI_MODE_STA) {
    LOG_DBG("KOSyncWorker", "Radio is in AP mode. Skipping connection");
    return false;
  }

  const auto& store = WIFI_STORE;
  const std::string last = store.getLastConnectedSsid();
  const WifiCredential* credential = last.empty() ? nullptr : store.findCredential(last);
  if (credential == nullptr && !store.getCredentials().empty()) {
    credential = &store.getCredentials().front();
  }
  if (credential == nullptr) {
    LOG_DBG("KOSyncWorker", "No saved WiFi network to auto-connect");
    return false;
  }

  const std::string ssid = credential->ssid;
  const std::string password = credential->password;

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("KOSyncWorker", "Auto-connect to %s timed out", ssid.c_str());
    WiFi.disconnect(false);
    return false;
  }

  WiFi.setSleep(false);
  LOG_DBG("KOSyncWorker", "Auto-connected to %s", ssid.c_str());
  return true;
}

bool radioStillOursToTearDown() { return WiFi.getMode() == WIFI_MODE_STA && !activityManager.wifiActivityOnStack(); }

void runJob(KOReaderSyncJob& job) {
  if (activityManager.wifiActivityOnStack()) {
    LOG_DBG("KOSyncWorker", "A network activity owns the radio. Skipping this job");
    job.result = KOReaderSyncClient::NETWORK_ERROR;
    return;
  }
  logInternalHeap("before_radio_up");
  const bool startedRadio = WiFi.getMode() == WIFI_MODE_NULL;
  const unsigned long connectTimeout = job.connectTimeoutMs > 0 ? job.connectTimeoutMs : AUTO_CONNECT_TIMEOUT_MS;
  if (!connectToSavedNetwork(connectTimeout)) {
    job.result = KOReaderSyncClient::NETWORK_ERROR;
    if (startedRadio && radioStillOursToTearDown()) {
      WiFi.mode(WIFI_OFF);
    }
    return;
  }

  logInternalHeap("after_assoc");

  KOReaderSyncClient::setSkipTlsValidation(SETTINGS.skipHttpsValidation != 0);

  HalClock::ensureUsableForTls(SETTINGS.ntpServer);

  switch (job.kind) {
    case KOReaderSyncJob::FETCH_PROGRESS:
      job.result = KOReaderSyncClient::getProgress(job.primaryHash, job.remote);
      if (job.smartProbe && !job.altHash.empty() && job.altHash != job.primaryHash) {
        job.altResult = KOReaderSyncClient::getProgress(job.altHash, job.altRemote);
      }
      break;
    case KOReaderSyncJob::SILENT_PUSH:
      job.result = KOReaderSyncClient::updateProgress(job.progress);
      break;
  }
  logInternalHeap("after_request");

  if (startedRadio && radioStillOursToTearDown()) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
}

void syncWorkerTrampoline(void*) {
  for (;;) {
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WORKER_IDLE_EXIT_MS)) == 0) {
      bool exiting = false;
      {
        const SlotLock lock;
        if (!slotBusy || jobDone) {
          workerTask = nullptr;
          exiting = true;
        }
      }
      if (exiting) {
        LOG_DBG("KOSyncWorker", "Idle, freeing the worker task (stack headroom was %u bytes)",
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        vTaskDelete(nullptr);
      }
      continue;
    }
    KOReaderSyncJob job;
    {
      const SlotLock lock;
      if (!slotBusy) {
        continue;
      }
      job = std::move(slot);
    }
    runJob(job);
    {
      const SlotLock lock;
      slot = std::move(job);
      jobDone = true;
      updateCachedBusy();
    }
  }
}

// caller has to hold slotMutex
void ensureTask() {
  if (workerTask != nullptr) {
    return;
  }
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t workerCore = 1;
#else
  constexpr BaseType_t workerCore = 0;
#endif
  if (xTaskCreatePinnedToCore(&syncWorkerTrampoline, "KOSyncWorker", KOReaderSyncWorker::WORKER_STACK_BYTES, nullptr, 1,
                              &workerTask, workerCore) != pdPASS) {
    LOG_ERR("KOSyncWorker", "Failed to create sync worker task");
    workerTask = nullptr;
  }
}

bool ensureMutex() {
  if (slotMutex == nullptr) {
    slotMutex = xSemaphoreCreateMutex();
  }
  return slotMutex != nullptr;
}

}  // namespace

namespace KOReaderSyncWorker {
bool isBusy() { return cachedBusy.load(std::memory_order_relaxed); }

bool post(KOReaderSyncJob&& job, uint64_t* seqOut) {
  if (!ensureMutex()) {
    return false;
  }
  uint64_t seq = 0;
  TaskHandle_t target = nullptr;
  {
    const SlotLock lock;
    if (slotBusy && !jobDone) {
      return false;
    }
    ensureTask();
    if (workerTask == nullptr) {
      return false;
    }
    slot = std::move(job);
    slotBusy = true;
    jobDone = false;
    activeSeq = nextSeq++;
    seq = activeSeq;
    target = workerTask;
    updateCachedBusy();
  }
  xTaskNotifyGive(target);
  if (seqOut != nullptr) {
    *seqOut = seq;
  }
  return true;
}

bool isDone(const uint64_t seq) {
  if (slotMutex == nullptr) return true;
  const SlotLock lock;
  return !(slotBusy && activeSeq == seq && !jobDone);
}

bool waitFor(const uint64_t seq, const unsigned long timeoutMs) {
  const unsigned long start = millis();
  while (!isDone(seq)) {
    if (millis() - start >= timeoutMs) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return true;
}

KOReaderSyncJob consume(const uint64_t seq) {
  KOReaderSyncJob out;
  out.result = KOReaderSyncClient::NETWORK_ERROR;
  if (slotMutex == nullptr) return out;
  const SlotLock lock;
  if (slotBusy && activeSeq == seq && jobDone) {
    out = std::move(slot);
    slot = KOReaderSyncJob{};
    slotBusy = false;
    updateCachedBusy();
  }
  return out;
}

bool drain(const unsigned long timeoutMs) {
  const unsigned long start = millis();
  while (isBusy()) {
    if (millis() - start >= timeoutMs) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return true;
}

}  // namespace KOReaderSyncWorker

#endif
