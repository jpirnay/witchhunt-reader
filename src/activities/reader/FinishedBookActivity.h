#pragma once

#include <string>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

namespace BookFinished {
std::string findNextBookInDirectory(const std::string& currentBookPath, const std::string& currentBookSeries,
                                    const std::string& currentBookSeriesIndex);

bool moveFinishedBookToCompleted(const std::string& currentBookPath, std::string& outMovedPath);

enum class FinishedBookAction {
  Stay = 0,
  GoHome = 1,
  OpenNextBook = 2,
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
void launchFinishedBookFlow(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::string& bookPath, const std::string& series, const std::string& seriesIndex,
                            void (*onMenuClosed)(void*) = nullptr, void* onMenuClosedCtx = nullptr);
}  // namespace BookFinished

class FinishedBookActivity : public Activity {
 public:
  FinishedBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string currentBookPath,
                       std::string nextBookPath);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string currentBookPath_;
  std::string nextBookPath_;
  std::string nextBookName_;
  std::string nextBookTitle_;
  std::string nextBookAuthor_;
  std::string nextBookSeries_;
  std::string nextBookCoverPath_;
  bool nextBookAvailable_ = false;
  bool nextBookMetadataLoaded_ = false;
  bool moveFinishedBooksToCompleted_ = false;
  bool removeFinishedBooksFromRecents_ = false;
  int selectedIndex_ = 0;
};
