#define HAL_STORAGE_IMPL
#include "HalStorage.h"

#include <Arduino.h>
#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <HalClock.h>
#include <Logging.h>
#include <SDCardManager.h>
#include <SdFat.h>
#include <freertos/task.h>

#include <cassert>
#include <ctime>
#include <new>

#include "HalSpiBus.h"

#define SDCard SDCardManager::getInstance()

HalStorage HalStorage::instance;

namespace {

struct TrackedActivity {
  HalStorage::Operation operation = HalStorage::Operation::None;
  HalStorage::OperationState state = HalStorage::OperationState::Idle;
  uint32_t startedMs = 0;
  TaskHandle_t owner = nullptr;
};

portMUX_TYPE s_activityMux = portMUX_INITIALIZER_UNLOCKED;
TrackedActivity s_activeActivity;
TrackedActivity s_waitingActivity;

class StorageActivityScope {
 public:
  explicit StorageActivityScope(HalStorage::Operation operation)
      : operation(operation), owner(xTaskGetCurrentTaskHandle()), startedMs(millis()) {
    portENTER_CRITICAL(&s_activityMux);
    s_waitingActivity = {operation, HalStorage::OperationState::WaitingForSpi, startedMs, owner};
    portEXIT_CRITICAL(&s_activityMux);
  }

  ~StorageActivityScope() {
    portENTER_CRITICAL(&s_activityMux);
    if (s_activeActivity.owner == owner) {
      s_activeActivity = {};
    }
    if (s_waitingActivity.owner == owner) {
      s_waitingActivity = {};
    }
    portEXIT_CRITICAL(&s_activityMux);
  }

  void waitingForStorage() {
    portENTER_CRITICAL(&s_activityMux);
    if (s_waitingActivity.owner == owner) {
      s_waitingActivity.state = HalStorage::OperationState::WaitingForStorage;
    }
    portEXIT_CRITICAL(&s_activityMux);
  }

  void active() {
    portENTER_CRITICAL(&s_activityMux);
    if (s_waitingActivity.owner == owner) {
      s_waitingActivity = {};
    }
    s_activeActivity = {operation, HalStorage::OperationState::Active, startedMs, owner};
    portEXIT_CRITICAL(&s_activityMux);
  }

 private:
  HalStorage::Operation operation;
  TaskHandle_t owner;
  uint32_t startedMs;
};

}  // namespace

HalStorage::HalStorage() {}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() {
  // Create the mutex here rather than in the constructor: HalStorage::instance
  // is a global, and its constructor runs before the FreeRTOS scheduler starts.
  // Calling xSemaphoreCreateMutex() that early corrupts the TLSF heap metadata.
  if (!storageMutex) {
    storageMutex = xSemaphoreCreateMutex();
    assert(storageMutex != nullptr);
  }
  {
    // SD init drives the shared bus, so it must be serialized against the
    // display too - the render task is already running by this point.
    HalSpiBus::Lock spiLock(HalSpiBus::Operation::Sd);
    if (!SDCard.begin()) return false;
  }
  FsDateTime::setCallback([](uint16_t* date, uint16_t* time) {
    if (!HalClock::isSynced()) {
      *date = FS_DATE(1980, 1, 1);
      *time = FS_TIME(0, 0, 0);
      return;
    }
    const time_t t = HalClock::now();
    const struct tm* tm = localtime(&t);
    if (!tm) {
      *date = FS_DATE(1980, 1, 1);
      *time = FS_TIME(0, 0, 0);
      return;
    }
    *date = FS_DATE(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    *time = FS_TIME(tm->tm_hour, tm->tm_min, tm->tm_sec);
  });
  return true;
}

bool HalStorage::ready() const { return SDCard.ready(); }

HalStorage::ActivitySnapshot HalStorage::activitySnapshot() const {
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

const char* HalStorage::operationName(Operation operation) {
  switch (operation) {
    case Operation::None:
      return "idle";
    case Operation::Open:
      return "open";
    case Operation::Read:
      return "read";
    case Operation::Write:
      return "write";
    case Operation::Seek:
      return "seek";
    case Operation::Metadata:
      return "meta";
    case Operation::Directory:
      return "dir";
    case Operation::Modify:
      return "modify";
    case Operation::Close:
      return "close";
    case Operation::Count:
      break;
  }
  return "?";
}

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  explicit StorageLock(Operation operation) : activity(operation) {
    activity.waitingForStorage();
    xSemaphoreTake(HalStorage::getInstance().storageMutex, portMAX_DELAY);
    activity.active();
  }
  ~StorageLock() { xSemaphoreGive(HalStorage::getInstance().storageMutex); }

 private:
  // Activity is declared first so a wait on either lock remains observable.
  StorageActivityScope activity;
  // Acquired before storageMutex and released after it:
  // the bus stays locked for the whole SD operation, and the lock order is
  // always SPI-outer/storage-inner, matching display code (which takes only the
  // SPI lock). Do not reorder this below any other member.
  HalSpiBus::Lock spiLock{HalSpiBus::Operation::Sd};
};

#define HAL_STORAGE_WRAPPED_CALL(operation, method, ...) \
  HalStorage::StorageLock lock(operation);               \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(Operation::Directory, listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(Operation::Read, readFile, path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(Operation::Read, readFileToStream, path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  HAL_STORAGE_WRAPPED_CALL(Operation::Read, readFileToBuffer, path, buffer, bufferSize, maxBytes);
}

// Ported verbatim from crosspoint-reader PR #2734 by Justin Mitchell
// (@itsthisjustin).
//
// Composed of already-locked HalStorage/HalFile operations, so it takes no
// StorageLock of its own - doing so would deadlock on the non-recursive mutex.
bool HalStorage::readFileToString(const char* moduleName, const std::string& path, size_t cap, std::string& out) {
  out.clear();
  HalFile file;
  if (!openFileForRead(moduleName, path, file)) return false;
  if (file.isDirectory()) return false;
  const size_t size = file.fileSize();
  if (size == 0 || size > cap) return false;
  out.resize(size);
  return file.read(out.data(), size) == static_cast<int>(size);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  HAL_STORAGE_WRAPPED_CALL(Operation::Write, writeFile, path, content);
}

bool HalStorage::ensureDirectoryExists(const char* path) {
  HAL_STORAGE_WRAPPED_CALL(Operation::Directory, ensureDirectoryExists, path);
}

uint64_t HalStorage::sdTotalBytes() const {
  StorageLock lock(Operation::Metadata);
  return SDCard.sdTotalBytes();
}

uint64_t HalStorage::sdUsedBytes() {
  StorageLock lock(Operation::Metadata);
  return SDCard.sdUsedBytes();
}

uint64_t HalStorage::sdFreeBytes() {
  uint64_t total = sdTotalBytes();
  uint64_t used = sdUsedBytes();
  if (total <= used) return 0;
  return total - used;
}

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  FsFile file;
};

HalFile::HalFile() = default;

HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}

HalFile::~HalFile() = default;

HalFile::HalFile(HalFile&&) = default;

HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock(Operation::Open);  // ensure thread safety for the duration of this function
  return HalFile(std::make_unique<HalFile::Impl>(SDCard.open(path, oflag)));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) {
  HAL_STORAGE_WRAPPED_CALL(Operation::Directory, mkdir, path, pFlag);
}

bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(Operation::Metadata, exists, path); }

bool HalStorage::remove(const char* path) { HAL_STORAGE_WRAPPED_CALL(Operation::Modify, remove, path); }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  HAL_STORAGE_WRAPPED_CALL(Operation::Modify, rename, oldPath, newPath);
}

bool HalStorage::rmdir(const char* path) { HAL_STORAGE_WRAPPED_CALL(Operation::Modify, rmdir, path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock(Operation::Open);  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForRead(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock(Operation::Open);  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForUpdate(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock(Operation::Open);  // ensure thread safety for the duration of this function
  FsFile fsFile = SDCard.open(path, O_RDWR);
  const bool ok = static_cast<bool>(fsFile);
  if (!ok) {
    LOG_ERR(moduleName, "Failed to open %s for update", path);
  }
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForUpdate(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForUpdate(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { HAL_STORAGE_WRAPPED_CALL(Operation::Modify, removeDir, path); }

bool HalStorage::copyFile(const char* moduleName, const std::string& srcPath, const char* dstPath) {
  HalFile src, dst;
  if (!openFileForRead(moduleName, srcPath, src)) return false;
  if (!openFileForWrite(moduleName, dstPath, dst)) {
    src.close();
    return false;
  }
  constexpr size_t BUF_SIZE = 4096;
  auto* buf = new (std::nothrow) uint8_t[BUF_SIZE];
  if (!buf) {
    dst.close();
    src.close();
    return false;
  }
  bool ok = true;
  while (src.available()) {
    const auto bytesRead = src.read(buf, BUF_SIZE);
    if (bytesRead <= 0) break;
    if (dst.write(buf, bytesRead) != static_cast<size_t>(bytesRead)) {
      ok = false;
      break;
    }
  }
  delete[] buf;
  dst.close();
  src.close();
  return ok;
}

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(operation, method, ...) \
  HalStorage::StorageLock lock(operation);            \
  assert(impl != nullptr);                            \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, ...) \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Write, flush, ); }
size_t HalFile::getName(char* name, size_t len) {
  HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Metadata, getName, name, len);
}
size_t HalFile::size() {
  assert(impl != nullptr);
  return static_cast<size_t>(impl->file.size());
}
size_t HalFile::fileSize() {
  assert(impl != nullptr);
  return static_cast<size_t>(impl->file.fileSize());
}
bool HalFile::seek(size_t pos) { HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Seek, seekSet, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Seek, seekCur, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Seek, seekSet, offset); }
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Metadata, available, ); }
size_t HalFile::position() const {
  assert(impl != nullptr);
  return static_cast<size_t>(impl->file.position());
}
int HalFile::read(void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Read, read, buf, count); }
int HalFile::read() { HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Read, read, ); }
size_t HalFile::write(const void* buf, size_t count) {
  HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Write, write, buf, count);
}
size_t HalFile::write(uint8_t b) { HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Write, write, b); }
bool HalFile::rename(const char* newPath) { HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Modify, rename, newPath); }
bool HalFile::getModifyDateTime(uint16_t* pdate, uint16_t* ptime) {
  HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Metadata, getModifyDateTime, pdate, ptime);
}
bool HalFile::getCreateDateTime(uint16_t* pdate, uint16_t* ptime) {
  HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Metadata, getCreateDateTime, pdate, ptime);
}
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Directory, rewindDirectory, ); }
bool HalFile::close() { HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Close, close, ); }
uint64_t HalFile::size64() { HAL_FILE_FORWARD_CALL(size, ); }
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }
bool HalFile::seek64(uint64_t pos) { HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Seek, seekSet, pos); }
bool HalFile::seekSet64(uint64_t offset) { HAL_FILE_WRAPPED_CALL(HalStorage::Operation::Seek, seekSet, offset); }
uint64_t HalFile::position64() const { HAL_FILE_FORWARD_CALL(position, ); }
HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock(HalStorage::Operation::Directory);
  assert(impl != nullptr);
  return HalFile(std::make_unique<Impl>(impl->file.openNextFile()));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
