#pragma once

#include <PngToBmpConverter.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../Activity.h"
#include "./FileBrowserActivity.h"
#include "ReadingStats.h"
#include "activities/reader/ReaderActivity.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
 public:
  enum class MenuAction {
    FileBrowser,
    Recents,
    GlobalBookmarks,
    OpdsBrowser,
    FileTransfer,
    Weather,
    Settings,
  };

 private:
  struct MenuEntry {
    MenuAction action;
    StrId label;
    UIIcon icon;
  };

  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int lastCarouselBookIndex = 0;  // remembered position when leaving carousel row
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool coverRendered = false;
  bool coverBufferStored = false;
  // True while the ~48 KB secondary framebuffer has been released to give the
  // cold-cache cover decode/extract pipeline headroom (mirrors the reader's pattern).
  // Restored once all covers are loaded, and on exit. See loadRecentCovers().
  bool secondaryBufferReleased = false;
  size_t nextRecentCoverIndex = 0;
  size_t nextThumbSizeIndex = 0;  // which thumb size within the current book is next

  // Phase 1: sliced ZIP extraction of cover.img (only needed for large embedded PNG covers)
  std::unique_ptr<ReaderActivity::CoverExtractSession> extractSession;

  // Phase 2: sliced PNG decode session (non-null while a PNG cover is being decoded row-by-row)
  std::unique_ptr<PngDecodeSession> pngSession;
  ReaderActivity::PngThumbFiles pngSessionFiles;  // open FsFiles borrowed by pngSession
  bool pngSessionFailed = false;                  // set on error; triggers empty-path store same as sync failure

  // Session-scoped transient-failure counter, keyed by book path. A cover can fail to load for
  // transient reasons (OOM under heap pressure, an interrupted write, an extraction that could not
  // start). We retry such a book on later passes, but bounded: after COVER_MAX_TRANSIENT_ATTEMPTS
  // transient failures in one Home session we stop retrying it THIS session (store an empty cover
  // and advance) so a persistently-failing book can't starve the others. No persistent sentinel is
  // written for transient failures — a reboot resets the map and gives every book a fresh chance;
  // only a structurally-absent cover (no cover item / unsupported format) earns a permanent sentinel
  // (written by generateThumbBmp). Cleared in onEnter().
  static constexpr uint8_t COVER_MAX_TRANSIENT_ATTEMPTS = 2;
  std::unordered_map<std::string, uint8_t> coverTransientAttempts;

  uint8_t* coverBuffer = nullptr;
  size_t coverBufferSize = 0;
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;

  std::vector<RecentBook> recentBooks;
  std::vector<MenuEntry> menuEntries;
  bool menuEntriesDirty = true;

  std::string focusBookPath;
  int focusSelectorIndex = -1;

  void onSelectBook(const std::string& path);
  void dispatchMenuAction(MenuAction action);

  void rebuildMenuEntries();
  bool storeCoverBuffer();
  bool restoreCoverBuffer();
  void freeCoverBuffer();
  void restoreSecondaryBuffer(bool callerHoldsRenderLock = false);
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);
  // One thumbnail slot to placeholder on a permanent give-up: the exact on-disk path the cover
  // loader checks, plus the (w,h) the placeholder BMP should be written at.
  struct ThumbSlot {
    std::string path;
    int width;
    int height;
  };
  // Give up on a book's cover for this pass. Bumps the session retry counter for a transient
  // failure. When the failure is permanent — structurally absent, or transient but past the
  // session retry budget — writes a valid placeholder BMP at each slot (like RecentBooksActivity)
  // so the book reads as resolved on disk and is not re-decoded on the next boot; otherwise just
  // records an empty cover so it retries next session. Shared by both cover paths.
  void giveUpCover(RecentBook& book, ThumbResult res, const std::vector<ThumbSlot>& slots);
  // True once a book has burned COVER_MAX_TRANSIENT_ATTEMPTS transient failures this session.
  bool coverAttemptsExhausted(const std::string& path) const;

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string focusBookPath = {},
                        int focusSelectorIndex = -1)
      : Activity("Home", renderer, mappedInput),
        focusBookPath(std::move(focusBookPath)),
        focusSelectorIndex(focusSelectorIndex) {}
  void onEnter() override;
  void onExit() override;

 private:
  // Bound to the time Home is displayed, not to the object: Home outlives its own visibility
  // when the reader is pushed on top, and the whole point is that the reader runs without the
  // history resident. Optional rather than a plain member for exactly that reason.
  std::optional<ReadingStatsStore::ScopedLoad> statsLoad_;

 public:
  void loop() override;
  void render(RenderLock&&) override;
  // Covers still resolving (not just mid-pass): loadRecentCovers() clears recentsLoading at
  // every yield point so loop() re-enters it, which briefly makes the activity look idle. If
  // skipLoopDelay went false in that window, the main loop's inactivity governor could drop
  // the CPU to 10 MHz mid-burst and the next decode tick would crawl (observed: a ~1.5 s
  // cover decode taking ~25 s). Hold full speed until every recent cover is resolved.
  bool skipLoopDelay() override {
    return (firstRenderDone && !recentsLoaded) || recentsLoading || extractSession != nullptr || pngSession != nullptr;
  }
};
