#pragma once
#include <Epub.h>

#include <memory>

#include "ChapterXPathIndexer.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"  // DocumentMatchMethod
#include "KOReaderSyncClient.h"
#include "LastPushCache.h"
#include "ProgressComparison.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

// Where the reader was when it handed off to the sync screen, as ActivityManager reads it out of
// APP_STATE.koReaderSyncSession. One struct rather than nine positional ints.
struct SyncLocalPosition {
  int spineIndex = 0;
  int page = 0;
  int totalPages = 0;
  uint16_t paragraphIndex = 0;
  bool hasParagraphIndex = false;
  uint16_t paragraphIndexBefore = 0;
  uint32_t visibleOffsetAtPage = 0;
  uint32_t visibleOffsetAtNextPage = UINT32_MAX;
  bool hasVisibleOffset = false;
};

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
 * - COMPARE: fetch remote progress. Under ASK_EVERY_TIME, show the full comparison
 *   screen and let the user choose Apply or Upload; under SMART, resolve it in favour
 *   of whichever side is further and report the outcome.
 * - PULL_REMOTE: fetch and map remote progress, show success feedback, then
 *   persist an applied SyncResult for the reopened reader.
 * - PUSH_LOCAL: compute local mapping, warm session with GET, then upload via
 *   reused connection to avoid a second full TLS handshake.
 */
class KOReaderSyncActivity final : public Activity {
 public:
  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                const SyncLocalPosition& local,
                                KOReaderSyncIntentState syncIntent = KOReaderSyncIntentState::COMPARE)
      : Activity("KOReaderSync", renderer, mappedInput),
        epubPath(epubPath),
        currentSpineIndex(local.spineIndex),
        currentPage(local.page),
        totalPagesInSpine(local.totalPages),
        localParagraphIndex(local.paragraphIndex),
        hasLocalParagraphIndex(local.hasParagraphIndex),
        localParagraphIndexBefore(local.paragraphIndexBefore),
        localVisibleOffsetAtPage(local.visibleOffsetAtPage),
        localVisibleOffsetAtNextPage(local.visibleOffsetAtNextPage),
        hasLocalVisibleOffset(local.hasVisibleOffset),
        syncIntent(syncIntent),
        remoteProgress{},
        remotePosition{},
        localProgress{} {}

  void onEnter() override;
  bool usesWifi() const override { return true; }
  void onExit() override;
  // Tap on one of the three conflict options -> select it; ActivityManager synthesizes
  // Confirm. Only accepted in SHOWING_RESULT, the one state that draws them.
  ListRowTap::Result selectListRow(int index) override;
  void loop() override;
  void render(RenderLock&&) override;
  // UPLOADING belongs here too: sleeping mid-PUT drops the connection with the write in
  // flight, and the user is not touching buttons while it runs.
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == UPLOADING; }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    APPLY_COMPLETE,
    // Smart mode found the two sides already at the same place: nothing to upload or apply.
    SYNC_COMPLETE,
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
  uint16_t localParagraphIndexBefore;
  uint32_t localVisibleOffsetAtPage;
  uint32_t localVisibleOffsetAtNextPage;
  bool hasLocalVisibleOffset;
  KOReaderSyncIntentState syncIntent = KOReaderSyncIntentState::COMPARE;
  // Known once the Epub was loaded for local mapping; 0 until then. Lets a fetched record's
  // DocFragment be range-checked after the Epub has been released for TLS.
  int spineCount = 0;
  // The book's cache directory (Epub::getCachePath), known once the Epub was loaded for mapping:
  // where our last push is kept (LastPushCache.h). Empty until then.
  std::string bookCachePath;
  // WiFi was taken fully down after the GET so the remote mapping runs with the radio's memory
  // back (a pull, or a compare against a record that is not our own last push). An upload then
  // reconnects first (uploadLocalProgress).
  bool radioDroppedForMapping = false;

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
  // localProgress and the chapter label are mapped for this session. Set by the mapping in
  // onEnter (before WiFi), so performSync and performUpload reuse it instead of mapping again
  // with the radio up; cleared when a mapping fails, so the next call retries.
  bool localProgressComputed = false;
  std::string remoteChapterLabel;
  std::string localChapterLabel;
  std::optional<KOReaderMetadata> localDocumentMetadata;

  // Selection in the compare screen: 0=Apply remote, 1=Upload local, 2=conflict policy toggle.
  // The policy row is not an action — confirming it flips the setting and returns the cursor to
  // whichever of the two actions the new policy would pick.
  static constexpr int OPTION_SYNC_BEHAVIOR = 2;
  static constexpr int OPTION_COUNT = 3;
  int selectedOption = 0;

  // Timestamp when completion state was entered (for auto-close)
  unsigned long uploadCompleteTime = 0;
  bool closeRequested = false;

  // Tracks whether this session activated WiFi. Set in onEnter past the credentials
  // check; checked in onExit to decide whether to silent-reboot. Can't rely on
  // WiFi.getMode() because intermediate paths call esp_wifi_stop() to drop the
  // radio while user reads the result, which makes WiFi.getMode() return WIFI_MODE_NULL.
  bool wifiActivated = false;

  // Captured from APP_STATE.koReaderSyncSession's postAction/postActionTarget in resumeReader()
  // so onExit() can route the silent reboot even after resumeReader() has cleared the persisted
  // session (Reader/Home/OpenBook all resolve their destination before the reboot happens, so
  // nothing needs to survive it — only OpdsSearch leaves the persisted fields in place, since that
  // one is resolved by HomeActivity after the reboot instead).
  KOReaderSyncPostAction postAction_ = KOReaderSyncPostAction::Reader;
  std::string postActionTarget_;

  void onWifiSelectionComplete(bool success);
  void performSync();
  bool calculateDocumentHash();
  std::string hashForMethod(DocumentMatchMethod method) const;
  static const char* matchMethodName(DocumentMatchMethod method);
  // On a NOT_FOUND, look the book up under the other matching method. On a hit, adopts that
  // id for this session (documentHash, effectiveMatchMethod, remoteProgress) and persists it.
  // havePrimaryRecord: our own id already resolved, so only adopt the alternate when it is
  // further along. When false, any hit wins because we had nothing.
  bool probeAlternateDocumentId(bool havePrimaryRecord);
  bool smartSyncEnabled() const;
  // Which side is further, by spine, then the paragraph LUT, then percentage (ProgressComparison).
  ProgressComparison compareLocalToRemote() const;
  // The reader's position as ProgressComparison wants it.
  LocalReadingPosition localReadingPosition() const;
  // Persist the mapped remote position and show the apply confirmation (or return straight
  // to the reader for auto-pull). Shared by the pull intent and smart resolution.
  void applyRemoteAndFinish();
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
  // End the TLS session and take WiFi fully down before the remote mapping; idempotent.
  void dropRadioForRemoteMapping();
  // Our last push of this book, when the server's record is exactly it: its position as the
  // mapping would resolve it, without the mapping. nullopt otherwise (or without a cached push).
  std::optional<CrossPointPosition> ownLastPushPosition(const std::string& remoteXPath) const;
  // Fill the remote position from our last push when the fetched record is that push.
  bool useOwnLastPush();
  // Remember a successful upload as our last push (or forget an older one it replaced).
  void saveOwnLastPush() const;
  // performUpload, reconnecting first when the radio was dropped for the remote mapping.
  void uploadLocalProgress();
  void onReconnectForUpload(bool success);
};
