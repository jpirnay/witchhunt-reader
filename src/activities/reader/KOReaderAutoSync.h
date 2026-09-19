#pragma once

#include <BoardConfig.h>

#if FREEINK_MCU_S3 && defined(BOARD_HAS_PSRAM)
#define CROSSPOINT_KOREADER_AUTOSYNC 1
#else
#define CROSSPOINT_KOREADER_AUTOSYNC 0
#endif

#if CROSSPOINT_KOREADER_AUTOSYNC

#include <cstdint>
#include <string>

#include "KOReaderCredentialStore.h"
#include "ProgressMapper.h"

class AutoSyncState {
  static AutoSyncState instance;

  std::string lastDocPath;
  int32_t lastPushedSpine = -1;
  int32_t lastPushedPage = -1;
  bool pending = false;
  bool lastSyncFailedFlag = false;

  int32_t lastSeenSpine = -2;
  int32_t lastSeenPage = -2;
  int32_t turnsSincePush = 0;
  bool lastNoteWasTurnFlag = false;
  bool dirty = false;

  AutoSyncState() = default;

 public:
  AutoSyncState(const AutoSyncState&) = delete;
  AutoSyncState& operator=(const AutoSyncState&) = delete;

  static AutoSyncState& getInstance() { return instance; }

  bool saveToFile();
  bool loadFromFile();

  void flush();

  bool isPending() const { return pending; }

  bool isPositionSynced(const std::string& bookPath, int spine, int page) const;

  void noteProgress(const std::string& bookPath, int spine, int page);

  void markSynced(const std::string& bookPath, int spine, int page);

  void markSyncFailed();
  bool lastSyncFailed() const { return lastSyncFailedFlag; }

  int32_t getTurnsSincePush() const { return turnsSincePush; }
  bool lastNoteWasTurn() const { return lastNoteWasTurnFlag; }

  void seedLastSeen(int spine, int page);
};

#define AUTOSYNC_STATE AutoSyncState::getInstance()

class KOReaderAutoSync {
 public:
  static constexpr unsigned long SLEEP_PUSH_CONNECT_TIMEOUT_MS = 6000;
  static constexpr unsigned long SLEEP_PUSH_JOIN_TIMEOUT_MS = 15000;

  static bool jobActive();

  static DocumentMatchMethod effectiveMatchMethod(const std::string& epubPath);
  static std::string documentHashFor(const std::string& epubPath);

  static bool pullEnabled();
  static bool sleepPushEnabled();
  static bool intervalPushEnabled();

  static uint64_t pushSilentAsync(const std::string& epubPath, const KOReaderPosition& localKoPos, int spine, int page,
                                  const std::string& title = "", const std::string& authors = "");

  static bool finalizeSilentPush(uint64_t seq);

  static bool stashSleepPush(const std::string& epubPath, const KOReaderPosition& localKoPos, int spine, int page,
                             const std::string& title = "", const std::string& authors = "");
  static uint64_t pushStashedSleepPushAsync();
  static void joinStashedSleepPush(uint64_t seq);
};

#endif
