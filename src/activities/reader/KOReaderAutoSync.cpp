#include "KOReaderAutoSync.h"

#if CROSSPOINT_KOREADER_AUTOSYNC

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <utility>

#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "KOReaderSyncWorker.h"

AutoSyncState AutoSyncState::instance;

namespace {
constexpr char AUTOSYNC_FILE_JSON[] = "/.crosspoint/autosync.json";

std::string basenameOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

KOReaderSyncJob buildSilentPushJob(const std::string& epubPath, const KOReaderPosition& localKoPos, const int spine,
                                   const int page, const std::string& title, const std::string& authors) {
  KOReaderSyncJob job;
  job.kind = KOReaderSyncJob::SILENT_PUSH;
  job.path = epubPath;
  job.spine = spine;
  job.page = page;
  job.progress.document = KOReaderAutoSync::documentHashFor(epubPath);
  job.progress.progress = localKoPos.xpath;
  job.progress.percentage = localKoPos.percentage;
  if (KOREADER_STORE.getSendMetadata()) {
    KOReaderMetadata meta;
    meta.filename = basenameOf(epubPath);
    meta.title = title;
    meta.authors = authors;
    job.progress.metadata = std::move(meta);
  }
  return job;
}

KOReaderSyncJob pendingSleepPush;

}  // namespace

bool AutoSyncState::saveToFile() {
  JsonDocument doc;
  doc["lastDocPath"] = lastDocPath;
  doc["lastPushedSpine"] = lastPushedSpine;
  doc["lastPushedPage"] = lastPushedPage;
  doc["pending"] = pending;
  doc["lastSyncFailed"] = lastSyncFailedFlag;
  doc["lastSeenSpine"] = lastSeenSpine;
  doc["lastSeenPage"] = lastSeenPage;
  doc["turnsSincePush"] = turnsSincePush;

  String json;
  serializeJson(doc, json);
  Storage.mkdir("/.crosspoint");
  const bool written = Storage.writeFile(AUTOSYNC_FILE_JSON, json);
  if (written) {
    dirty = false;
  }
  return written;
}

bool AutoSyncState::loadFromFile() {
  if (!Storage.exists(AUTOSYNC_FILE_JSON)) {
    return false;
  }
  const String json = Storage.readFile(AUTOSYNC_FILE_JSON);
  if (json.isEmpty()) {
    return false;
  }
  JsonDocument doc;
  if (const auto error = deserializeJson(doc, json.c_str())) {
    LOG_ERR("AutoSync", "JSON parse error: %s", error.c_str());
    return false;
  }
  lastDocPath = doc["lastDocPath"] | std::string("");
  lastPushedSpine = doc["lastPushedSpine"] | -1;
  lastPushedPage = doc["lastPushedPage"] | -1;
  pending = doc["pending"] | false;
  lastSyncFailedFlag = doc["lastSyncFailed"] | false;
  lastSeenSpine = doc["lastSeenSpine"] | -2;
  lastSeenPage = doc["lastSeenPage"] | -2;
  turnsSincePush = doc["turnsSincePush"] | 0;
  lastNoteWasTurnFlag = false;
  return true;
}

bool AutoSyncState::isPositionSynced(const std::string& bookPath, const int spine, const int page) const {
  return !lastDocPath.empty() && bookPath == lastDocPath && spine == lastPushedSpine && page == lastPushedPage;
}

void AutoSyncState::noteProgress(const std::string& bookPath, const int spine, const int page) {
  if (!KOReaderAutoSync::sleepPushEnabled() && !KOReaderAutoSync::intervalPushEnabled()) {
    return;
  }
  lastNoteWasTurnFlag = spine != lastSeenSpine || page != lastSeenPage;
  if (lastNoteWasTurnFlag) {
    lastSeenSpine = spine;
    lastSeenPage = page;

    if (KOReaderAutoSync::intervalPushEnabled()) {
      turnsSincePush++;
      dirty = true;
    }
  }
  if (!pending && (bookPath != lastDocPath || spine != lastPushedSpine || page != lastPushedPage)) {
    pending = true;
    saveToFile();
  }
}

void AutoSyncState::flush() {
  if (dirty) {
    saveToFile();
  }
}

void AutoSyncState::markSyncFailed() {
  if (lastSyncFailedFlag) {
    return;
  }
  lastSyncFailedFlag = true;
  saveToFile();
}

void AutoSyncState::markSynced(const std::string& bookPath, const int spine, const int page) {
  pending = false;
  lastSyncFailedFlag = false;
  lastDocPath = bookPath;
  lastPushedSpine = spine;
  lastPushedPage = page;

  lastSeenSpine = spine;
  lastSeenPage = page;
  turnsSincePush = 0;
  saveToFile();
}

void AutoSyncState::seedLastSeen(const int spine, const int page) {
  lastSeenSpine = spine;
  lastSeenPage = page;
  lastNoteWasTurnFlag = false;
}

DocumentMatchMethod KOReaderAutoSync::effectiveMatchMethod(const std::string& epubPath) {
  return KOReaderDocumentId::loadLearnedMatchMethod(epubPath).value_or(KOREADER_STORE.getMatchMethod());
}

std::string KOReaderAutoSync::documentHashFor(const std::string& epubPath) {
  return effectiveMatchMethod(epubPath) == DocumentMatchMethod::FILENAME
             ? KOReaderDocumentId::calculateFromFilename(epubPath)
             : KOReaderDocumentId::calculate(epubPath);
}

bool KOReaderAutoSync::jobActive() { return KOReaderSyncWorker::isBusy(); }

bool KOReaderAutoSync::pullEnabled() { return KOREADER_STORE.hasCredentials() && KOREADER_STORE.getSyncOnWake(); }

bool KOReaderAutoSync::sleepPushEnabled() { return KOREADER_STORE.hasCredentials() && KOREADER_STORE.getSyncOnSleep(); }

bool KOReaderAutoSync::intervalPushEnabled() {
  return KOREADER_STORE.hasCredentials() && KOREADER_STORE.getPushIntervalPages() > 0;
}

uint64_t KOReaderAutoSync::pushSilentAsync(const std::string& epubPath, const KOReaderPosition& localKoPos,
                                           const int spine, const int page, const std::string& title,
                                           const std::string& authors) {
  if (!KOREADER_STORE.hasCredentials()) return 0;
  KOReaderSyncJob job = buildSilentPushJob(epubPath, localKoPos, spine, page, title, authors);
  if (job.progress.document.empty()) {
    LOG_ERR("AutoSync", "Document hash failed for %s", epubPath.c_str());
    return 0;
  }
  if (job.progress.progress.empty()) {
    LOG_ERR("AutoSync", "Local mapping produced no xpath for %s; skipping push", epubPath.c_str());
    return 0;
  }
  uint64_t seq = 0;
  if (!KOReaderSyncWorker::post(std::move(job), &seq)) {
    LOG_DBG("AutoSync", "Push deferred: sync worker busy");
    return 0;
  }
  LOG_DBG("AutoSync", "Silent push queued: %s (%.6f)", epubPath.c_str(), localKoPos.percentage);
  return seq;
}

bool KOReaderAutoSync::finalizeSilentPush(const uint64_t seq) {
  if (seq == 0) {
    return false;
  }
  const KOReaderSyncJob job = KOReaderSyncWorker::consume(seq);
  if (job.kind != KOReaderSyncJob::SILENT_PUSH) {
    return false;
  }
  if (job.result != KOReaderSyncClient::OK) {
    LOG_INF("AutoSync", "Silent push failed: %s", KOReaderSyncClient::errorString(job.result));
    AUTOSYNC_STATE.markSyncFailed();
    return false;
  }
  AUTOSYNC_STATE.markSynced(job.path, job.spine, job.page);
  LOG_DBG("AutoSync", "Silent push OK: %s (%.6f)", job.path.c_str(), job.progress.percentage);
  return true;
}

bool KOReaderAutoSync::stashSleepPush(const std::string& epubPath, const KOReaderPosition& localKoPos, const int spine,
                                      const int page, const std::string& title, const std::string& authors) {
  if (!KOREADER_STORE.hasCredentials()) return false;
  KOReaderSyncJob job = buildSilentPushJob(epubPath, localKoPos, spine, page, title, authors);
  if (job.progress.document.empty() || job.progress.progress.empty()) {
    return false;
  }
  job.connectTimeoutMs = SLEEP_PUSH_CONNECT_TIMEOUT_MS;
  pendingSleepPush = std::move(job);
  return true;
}

uint64_t KOReaderAutoSync::pushStashedSleepPushAsync() {
  if (pendingSleepPush.path.empty()) {
    return 0;
  }

  KOReaderSyncJob job = pendingSleepPush;
  uint64_t seq = 0;
  if (!KOReaderSyncWorker::post(std::move(job), &seq)) {
    LOG_DBG("AutoSync", "Sleep push deferred: sync worker busy; join tail will retry");
    return 0;
  }
  pendingSleepPush = KOReaderSyncJob{};
  return seq;
}

void KOReaderAutoSync::joinStashedSleepPush(const uint64_t postedSeq) {
  uint64_t seq = postedSeq;
  unsigned long budget = SLEEP_PUSH_JOIN_TIMEOUT_MS;
  if (seq == 0) {
    if (pendingSleepPush.path.empty()) {
      return;
    }

    LOG_DBG("AutoSync", "Retrying deferred sleep push on the join tail");
    const unsigned long start = millis();
    KOReaderSyncWorker::drain(budget);
    const bool posted = KOReaderSyncWorker::post(std::move(pendingSleepPush), &seq);

    pendingSleepPush = KOReaderSyncJob{};
    if (!posted) {
      LOG_DBG("AutoSync", "Sleep push retry timed out; deferring to the next boundary");
      AUTOSYNC_STATE.markSyncFailed();
      return;
    }
    const unsigned long elapsed = millis() - start;
    budget = elapsed < budget ? budget - elapsed : 0;
  }
  if (!KOReaderSyncWorker::waitFor(seq, budget)) {
    LOG_DBG("AutoSync", "Sleep push still running at the join deadline; deferring to the next boundary");
    AUTOSYNC_STATE.markSyncFailed();
    return;
  }
  if (finalizeSilentPush(seq)) {
    LOG_DBG("AutoSync", "Sleep push OK");
  }
}

#endif
