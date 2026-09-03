#pragma once

#include <functional>
#include <string>

#ifndef CROSSPOINT_GIT_REPOSITORY
#define CROSSPOINT_GIT_REPOSITORY "jpirnay/witchhunt-reader"
#endif

class OtaUpdater {
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;
  bool render = false;
  bool cancelRequested = false;

 public:
  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
    METADATA_TOO_LARGE_ERROR,
    UPDATE_CANCELLED,
    UPDATE_IN_PROGRESS,
    VALIDATE_FAILED,
    // Release asset was built for another board: chip_id or the embedded board
    // tag disagreed with this build. See FirmwareBoardTag.h.
    WRONG_DEVICE_ERROR,
  };

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  bool getRender() const { return render; }
  void clearRender() { render = false; }

  bool isUpdateInProgress() const { return otaWriteHandle != nullptr; }

  // Called periodically during the streaming install with (processed, total)
  // so the host can redraw progress and poll for cancel. Return false to abort
  // the install (e.g. Back pressed). Optional; if unset, the install runs to
  // completion without per-chunk UI updates.
  using InstallProgressFn = std::function<bool(size_t processed, size_t total)>;
  void setInstallProgressCallback(InstallProgressFn cb) { installProgressCb = std::move(cb); }

  OtaUpdater() = default;
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError beginInstallUpdate();
  OtaUpdaterError performInstallUpdateStep();
  void cancelUpdate();
  void cleanupUpdate();

 private:
  static int forceSetOtaBootPartition();
  InstallProgressFn installProgressCb;
  // Opaque esp_ota handle for the streaming install (void* avoids including
  // esp_ota_ops.h in the header). Set by beginInstallUpdate, consumed by the step.
  void* otaWriteHandle = nullptr;
  bool installDone = false;
  OtaUpdaterError installResult = OK;
};
