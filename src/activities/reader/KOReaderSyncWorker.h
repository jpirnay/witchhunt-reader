#pragma once

#include "KOReaderAutoSync.h"

#if CROSSPOINT_KOREADER_AUTOSYNC

#include <cstdint>
#include <string>

#include "KOReaderSyncClient.h"

struct KOReaderSyncJob {
  enum Kind { FETCH_PROGRESS, SILENT_PUSH };
  Kind kind = FETCH_PROGRESS;

  std::string primaryHash;
  std::string altHash;
  bool smartProbe = false;

  KOReaderProgress progress;
  std::string path;
  int spine = -1;
  int page = -1;

  unsigned long connectTimeoutMs = 0;

  KOReaderSyncClient::Error result = KOReaderSyncClient::OK;
  KOReaderProgress remote;
  KOReaderSyncClient::Error altResult = KOReaderSyncClient::NOT_FOUND;
  KOReaderProgress altRemote;
};

namespace KOReaderSyncWorker {
bool isBusy();

bool post(KOReaderSyncJob&& job, uint64_t* seqOut = nullptr);

bool isDone(uint64_t seq);

bool waitFor(uint64_t seq, unsigned long timeoutMs);

KOReaderSyncJob consume(uint64_t seq);

bool drain(unsigned long timeoutMs);

}  // namespace KOReaderSyncWorker

#endif
