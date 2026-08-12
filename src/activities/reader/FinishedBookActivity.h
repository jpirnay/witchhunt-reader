#pragma once

#include <string>
#include <vector>

#include "../Activity.h"
#include "CrossPointState.h"
#include "util/ButtonNavigator.h"

namespace BookFinished {
std::string findNextBookInDirectory(const std::string& currentBookPath, const std::string& currentBookSeries,
                                    const std::string& currentBookSeriesIndex);

bool moveFinishedBookToCompleted(const std::string& currentBookPath, std::string& outMovedPath);

enum class FinishedBookAction {
  Stay = 0,
  GoHome = 1,
  OpenNextBook = 2,
  SearchOpdsForAuthor = 3,
};

// Launches the finished-book menu on top of `host` and handles its result:
// credits a finish to the reading-stats session, applies the move-to-/COMPLETED
// and remove-from-recents settings, then navigates home or to the next book.
// On cancel/stay the host gets a requestUpdate() to re-render its last page.
// The caller is responsible for persisting reading progress beforehand (the
// progress formats differ per reader).
// `onMenuClosed` (optional, with `onMenuClosedCtx`) runs first in the result
// handler regardless of outcome — readers use it to clear a "menu is open"
// flag. Plain function pointer + context instead of std::function per the
// project callback convention.
// `onSyncToKOReader` (optional, with `onSyncToKOReaderCtx`), when non-null, offers a "sync
// progress to KOReader" toggle (only when KOReader credentials are also configured) alongside
// the move-to-/COMPLETED and forget-book toggles. Unlike those two, applying it isn't a plain
// settings write: it's invoked in place of navigating away directly, with the book's current
// path (which may already be the moved-to-/COMPLETED path — the move/forget settings are still
// applied first, "in parallel" rather than replaced), a KOReaderSyncPostAction describing which
// of Go Home / Open Next / Search OPDS was picked, and that action's target (next-book path or
// OPDS author, empty for Go Home). The callback owns pushing progress and, once its own reboot
// completes, performing that action itself (EpubReaderActivity is the only caller that supplies
// one, since KOReader sync is EPUB-only).
void launchFinishedBookFlow(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::string& bookPath, const std::string& series, const std::string& seriesIndex,
                            const std::string& author = {}, void (*onMenuClosed)(void*) = nullptr,
                            void* onMenuClosedCtx = nullptr,
                            void (*onSyncToKOReader)(void*, const std::string& bookPath, KOReaderSyncPostAction,
                                                     const std::string& target) = nullptr,
                            void* onSyncToKOReaderCtx = nullptr);
}  // namespace BookFinished

class FinishedBookActivity : public Activity {
 public:
  FinishedBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string currentBookPath,
                       std::string nextBookPath, std::string currentBookAuthor = {},
                       bool koReaderSyncAvailable = false);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Selection movement goes through ButtonNavigator (as in StarredPagesActivity and the other
  // child-of-reader lists), NOT through consumed button events. This activity sits on top of the
  // reader on the activity stack, so main.cpp still sees "in the reader" and routes reader-scoped
  // actions (Up/Down/Left/Right commonly map to PREV/NEXT_SECTION) to dispatchButtonAction()
  // instead of delivering them here — consumeEvent() never sees them. ButtonNavigator reads the
  // mapped input state directly and is unaffected.
  ButtonNavigator buttonNavigator;

  // The menu's rows are conditional (next-book, OPDS-search, sync-to-KOReader and
  // move-to-/COMPLETED each appear only when applicable), so the row count and every row's
  // index depend on the same booleans. Those were previously re-derived independently in
  // onEnter(), loop() and render(); any divergence between the count handed to GUI.drawList
  // and the vectors its callbacks index is an out-of-bounds read. Building the model once per
  // use keeps the count, the indices and the row content in sync by construction.
  //
  // Rows are single-line (title + right-aligned value, no subtitle): with up to six rows now
  // possible, the two-line "with subtitle" row height would push the list well past what fits
  // alongside the header and next-book preview. Anything a subtitle used to carry (next-book
  // author/series, which author an OPDS search) either duplicates the preview panel above or
  // fits in the title itself.
  struct RowModel {
    enum class Action { GoHome, OpenNext, SearchOpds, ToggleMoveToCompleted, ToggleForget, ToggleSyncToKOReader };
    std::vector<Action> actions;
    std::vector<std::string> titles;
    std::vector<std::string> values;

    int count() const { return static_cast<int>(actions.size()); }
  };

  RowModel buildRowModel() const;

  std::string currentBookPath_;
  std::string nextBookPath_;
  std::string currentBookAuthor_;
  std::string nextBookName_;
  std::string nextBookTitle_;
  std::string nextBookAuthor_;
  std::string nextBookSeries_;
  std::string nextBookCoverPath_;
  bool nextBookAvailable_ = false;
  bool nextBookMetadataLoaded_ = false;
  bool koReaderSyncAvailable_ = false;
  bool moveFinishedBooksToCompleted_ = false;
  bool removeFinishedBooksFromRecents_ = false;
  bool syncFinishedBookToKOReader_ = false;
  int selectedIndex_ = 0;
};
