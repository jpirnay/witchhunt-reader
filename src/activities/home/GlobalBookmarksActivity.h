#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Home-screen activity that aggregates bookmarks from every indexed book and
// jumps directly into the chosen book/position on Confirm.
//
// Data source: GlobalBookmarkIndex (persisted at /.crosspoint/global_bookmarks.bin).
// Reconciles against the filesystem on entry (drops entries whose source file
// has disappeared).
//
// The display list is a flat vector of rows, where each row is either a book
// header separator or a bookmark entry belonging to the preceding header.
class GlobalBookmarksActivity final : public Activity {
 public:
  explicit GlobalBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ReturnHint restoreHint = {})
      : Activity("GlobalBookmarks", renderer, mappedInput), restoreHint(std::move(restoreHint)) {}

  void onEnter() override;
  void onExit() override;
  // Tap on a list row -> move the selection there; ActivityManager then synthesizes Confirm.
  bool selectListRow(int index) override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Row {
    bool isSeparator = false;
    size_t bookIndex = 0;      // index into GlobalBookmarkIndex entries
    size_t bookmarkIndex = 0;  // index within that entry's bookmarks (separator: ignored)
  };

  ButtonNavigator buttonNavigator;
  std::vector<Row> rows;
  int selectorIndex = 0;
  ReturnHint restoreHint;

  void rebuildRows();
  std::string getRowTitle(int index) const;
  bool isSeparatorRow(int index) const;
  int firstSelectableIndex() const;

  void openSelected();
  void deleteSelected();
  void renameSelected();

  // Apply a mutation to the underlying per-book BookmarkStore + global index.
  // `op` is invoked with the loaded store; it should mutate and return true
  // when something changed worth persisting. Title/cacheDir/isTxt are taken
  // from the current index entry.
  template <typename Op>
  void mutateBook(size_t bookIndex, Op&& op);
};
