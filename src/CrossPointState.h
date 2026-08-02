#pragma once
#include <cstdint>
#include <iosfwd>
#include <string>

enum class KOReaderSyncIntentState : uint8_t {
  COMPARE = 0,
  PULL_REMOTE = 1,
  PUSH_LOCAL = 2,
  // Auto variants compare progress before writing and skip silently when the other side
  // is already ahead. AUTO_PUSH fires from the reader-close auto-sync path; AUTO_PULL fires
  // when the user opens a book with long-press Confirm. Neither prompts the user.
  AUTO_PUSH = 3,
  AUTO_PULL = 4,
};

enum class KOReaderSyncOutcomeState : uint8_t {
  NONE = 0,
  PENDING = 1,
  CANCELLED = 2,
  FAILED = 3,
  UPLOAD_COMPLETE = 4,
  APPLIED_REMOTE = 5,
};

// Where KOReaderSyncActivity lands once a sync completes (or fails/cancels) and the device has
// rebooted to reclaim WiFi-session heap fragmentation. Reader/Home cover the two cases that
// existed before the finished-book "sync + then continue with the picked action" flow; OpenBook
// and OpdsSearch let that flow land on the next book / an OPDS search after syncing, instead of
// only ever the synced book or Home.
enum class KOReaderSyncPostAction : uint8_t {
  Reader = 0,      // reopen APP_STATE.openEpubPath (the book that was being synced)
  Home = 1,        // go to the home screen
  OpenBook = 2,    // reopen a different book (postActionTarget = its path)
  OpdsSearch = 3,  // go home, then launch an OPDS author search (postActionTarget = author)
};

struct PendingBookmarkJumpState {
  bool active = false;
  std::string bookPath;     // source file path for disambiguation
  uint16_t spineIndex = 0;  // EPUB spine; ignored for TXT
  uint16_t pageNumber = 0;  // page within spine (EPUB) or global page (TXT)

  void clear() {
    active = false;
    bookPath.clear();
    spineIndex = 0;
    pageNumber = 0;
  }
};

struct KOReaderSyncSessionState {
  bool active = false;
  std::string epubPath;
  int spineIndex = 0;
  int page = 0;
  int totalPagesInSpine = 0;
  uint16_t paragraphIndex = 0;
  bool hasParagraphIndex = false;
  uint32_t xhtmlSeekHint = 0;  // byte offset hint for findXPathForParagraph (0 = no hint)
  KOReaderSyncIntentState intent = KOReaderSyncIntentState::COMPARE;
  KOReaderSyncOutcomeState outcome = KOReaderSyncOutcomeState::NONE;
  int resultSpineIndex = 0;
  int resultPage = 0;
  uint16_t resultParagraphIndex = 0;
  bool resultHasParagraphIndex = false;
  // Running <li> count for the matched element, used by EpubReaderActivity to snap a
  // KOReader-supplied list-item XPath to the precise page via Section::getPageForListItemIndex.
  // Preferred over resultParagraphIndex when the deepest target element is /li[N].
  uint16_t resultListItemIndex = 0;
  bool resultHasListItemIndex = false;
  // Where to land once this sync (and its reboot) completes. Defaults to Reader so auto-push-on-
  // close and reader-menu-triggered syncs keep their existing behavior without every call site
  // having to set it explicitly; AUTO_PUSH's caller sets it to Home, the finished-book flow sets
  // it to whichever of Home / OpenBook / OpdsSearch matches the action the user picked.
  KOReaderSyncPostAction postAction = KOReaderSyncPostAction::Reader;
  // Book path (OpenBook) or author (OpdsSearch); unused for Reader/Home.
  std::string postActionTarget;
  // Set by RecentBooks / FileBrowser long-press to ask the reader to perform an AUTO_PULL
  // before rendering its first page. Stored by EPUB path so the flag cannot leak across books.
  std::string autoPullEpubPath;

  void clear() {
    active = false;
    epubPath.clear();
    spineIndex = 0;
    page = 0;
    totalPagesInSpine = 0;
    paragraphIndex = 0;
    hasParagraphIndex = false;
    xhtmlSeekHint = 0;
    intent = KOReaderSyncIntentState::COMPARE;
    outcome = KOReaderSyncOutcomeState::NONE;
    resultSpineIndex = 0;
    resultPage = 0;
    resultParagraphIndex = 0;
    resultHasParagraphIndex = false;
    resultListItemIndex = 0;
    resultHasListItemIndex = false;
    postAction = KOReaderSyncPostAction::Reader;
    postActionTarget.clear();
    autoPullEpubPath.clear();
  }
};

class CrossPointState {
  // Static instance
  static CrossPointState instance;

 public:
  std::string openEpubPath;
  size_t lastSleepImage = SIZE_MAX;  // SIZE_MAX = unset sentinel
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool recentBooksGridView = false;  // true = grid/thumbnail view, false = list view
  // When false, setup() skips the boot screen on wake and instead restores the
  // saved framebuffer overlaid with a loading icon (Quick Resume).
  bool showBootScreen = true;
  KOReaderSyncSessionState koReaderSyncSession;
  PendingBookmarkJumpState pendingBookmarkJump;
  ~CrossPointState() = default;

  // Get singleton instance
  static CrossPointState& getInstance() { return instance; }

  bool saveToFile() const;

  bool loadFromFile();
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
