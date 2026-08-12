#pragma once
#include <Epub.h>

#include <memory>

#include "ChapterXPathIndexer.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"  // DocumentMatchMethod
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

/**
 * Activity for syncing reading progress with KOReader sync server.
 *
 * This activity is launched as a standalone replacement screen, not as a
 * child activity of the reader. The reader persists a compact handoff record,
 * is destroyed to reclaim memory before WiFi/TLS work begins, and a fresh
 * reader instance is reopened after sync completes or is cancelled.
 *
 * Shared pipeline:
 * 1. Connect to WiFi (if not connected)
 * 2. Optionally sync NTP (if stale)
 * 3. Calculate document hash
 *
 * Intent-specific behavior:
 * - COMPARE: fetch remote progress, show full comparison screen, let user
 *   choose Apply or Upload.
 * - PULL_REMOTE: fetch and map remote progress, show success feedback, then
 *   persist an applied SyncResult for the reopened reader.
 * - PUSH_LOCAL: compute local mapping, warm session with GET, then upload via
 *   reused connection to avoid a second full TLS handshake.
 */
class KOReaderSyncActivity final : public Activity {
 public:
  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                int currentSpineIndex, int currentPage, int totalPagesInSpine,
                                uint16_t paragraphIndex = 0, bool hasParagraphIndex = false, uint32_t xhtmlSeekHint = 0,
                                KOReaderSyncIntentState syncIntent = KOReaderSyncIntentState::COMPARE)
      : Activity("KOReaderSync", renderer, mappedInput),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        localParagraphIndex(paragraphIndex),
        hasLocalParagraphIndex(hasParagraphIndex),
        localXhtmlSeekHint(xhtmlSeekHint),
        syncIntent(syncIntent),
        remoteProgress{},
        remotePosition{},
        localProgress{} {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING; }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    APPLY_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NO_CREDENTIALS
  };

  std::shared_ptr<Epub> epub;
  std::string epubPath;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;
  uint16_t localParagraphIndex;
  bool hasLocalParagraphIndex;
  uint32_t localXhtmlSeekHint;
  KOReaderSyncIntentState syncIntent = KOReaderSyncIntentState::COMPARE;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  // The matching method documentHash was actually computed with: the learned per-book method
  // when one has been recorded, otherwise the configured one. Uploads use this too, so a book
  // the server holds under the other device's id keeps syncing under that id.
  DocumentMatchMethod effectiveMatchMethod = DocumentMatchMethod::FILENAME;

  // Remote progress data
  bool hasRemoteProgress = false;
  bool remotePositionMapped = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader format (for display)
  KOReaderPosition localProgress;
  std::string remoteChapterLabel;
  std::string localChapterLabel;
  std::optional<KOReaderMetadata> localDocumentMetadata;

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  // Timestamp when completion state was entered (for auto-close)
  unsigned long uploadCompleteTime = 0;
  bool closeRequested = false;

  // Tracks whether this session activated WiFi. Set in onEnter past the credentials
  // check; checked in onExit to decide whether to silent-reboot. Can't rely on
  // WiFi.getMode() because intermediate paths call esp_wifi_stop() to drop the
  // radio while user reads the result, which makes WiFi.getMode() return WIFI_MODE_NULL.
  bool wifiActivated = false;

  // Captured from sync.exitToHomeAfterSync in resumeReader() so onExit can route the
  // silent reboot to home instead of the reader when reader-close auto-sync triggered.
  bool exitToHomeAfterSync = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  bool calculateDocumentHash();
  std::string hashForMethod(DocumentMatchMethod method) const;
  static const char* matchMethodName(DocumentMatchMethod method);
  // On a NOT_FOUND, look the book up under the other matching method. On a hit, adopts that
  // id for this session (documentHash, effectiveMatchMethod, remoteProgress) and persists it.
  bool probeAlternateDocumentId();
  bool handleAutoPushPreflight();
  void performFetchAndCompare();
  void performUpload();
  void closeCancelled();
  void resumeReader(KOReaderSyncOutcomeState outcome, const SyncResult* appliedResult = nullptr);
  bool ensureEpubLoadedForMapping();
  void releaseEpubForMapping();
  bool computeLocalProgressAndChapter();
  void computeRemoteChapter();
  bool ensureRemotePositionMapped(bool closeSessionBeforeMapping = true);
};
