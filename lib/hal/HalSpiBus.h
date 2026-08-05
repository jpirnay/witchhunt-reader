#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Serializes access to the SPI bus shared by the e-ink panel and the SD card.
//
// The two devices share the physical bus (see SPI_MISO in HalGPIO.h, "shared
// between SD card and display"). HalStorage already serializes SD access with
// storageMutex, but HalDisplay took no lock at all, and the two run on
// different tasks: the render task drives the panel from
// ActivityManager::renderTaskLoop(), while the main task runs
// currentActivity->loop() *without* the render lock (deliberately - see the
// note in ActivityManager::loop()). Many activities do SD I/O from loop().
//
// A display refresh interleaving with a concurrent SD transfer can therefore
// confuse SdSpiCard's unsynchronised m_spiActive state machine, surfacing as
// corrupted reads or a FreeRTOS panic - a signature easily misfiled as heap
// corruption.
//
// Lock ordering is SPI-outer, storage-inner: HalStorage::StorageLock acquires
// this lock as its outermost member (construction order guarantees it), and
// display code takes only this lock, so the global order is consistent and
// deadlock-free.
//
// The mutex is recursive so a storage path that re-enters the bus lock cannot
// self-deadlock. Note this does NOT make HalStorage's own storageMutex
// recursive - that one stays plain, because no storage path nests it today
// (~FsFile calls SdFat's close() directly, not the locking HalFile::close()).
class HalSpiBus {
 public:
  class Lock {
   public:
    Lock();
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

   private:
    bool acquired = false;
  };

  static HalSpiBus& getInstance();

  // Create the mutex up front. Call once from setup() before the first display
  // or storage access. Lock construction falls back to lazy creation, but doing
  // it explicitly keeps mutex allocation out of the first refresh path.
  static void begin();

 private:
  HalSpiBus();

  SemaphoreHandle_t mutex = nullptr;

  friend class Lock;
};
