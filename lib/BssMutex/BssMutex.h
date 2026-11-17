#pragma once

// A mutex for a lib's shared state that more than one task reaches: a FreeRTOS mutex whose storage
// sits in .bss on the device, std::mutex on the host. Use it with std::lock_guard.
//
// Not a std::mutex on the device: ESP-IDF makes a std::mutex's pthread mutex and its semaphore on
// the heap at its first lock (pthread_mutex_init_if_static) and frees neither, which lands
// mid-session wherever the first lock happens (MiniBidi's deviation 1 in BidiUtils.cpp).
// Define one at namespace scope: a function-local static takes a guard on first use.

#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class BssMutex {
 public:
  BssMutex() : handle_(xSemaphoreCreateMutexStatic(&storage_)) {}
  BssMutex(const BssMutex&) = delete;
  BssMutex& operator=(const BssMutex&) = delete;
  void lock() { xSemaphoreTake(handle_, portMAX_DELAY); }
  void unlock() { xSemaphoreGive(handle_); }

 private:
  StaticSemaphore_t storage_{};  // declared before handle_, which is made in it
  SemaphoreHandle_t handle_;
};
#else
#include <mutex>
using BssMutex = std::mutex;
#endif
