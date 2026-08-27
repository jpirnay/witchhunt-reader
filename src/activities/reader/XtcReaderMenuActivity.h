#pragma once
#include <I18n.h>

#include <string>

#include "../MenuListActivity.h"

// Reader menu for the pre-rendered XTC/XTCH formats.
//
// Deliberately a fraction of EpubReaderMenuActivity: XTC pages are bitmaps produced by the
// converter, so there is no text layer and no layout to re-decide here — font, size, alignment,
// hyphenation, embedded style, image handling and screen rotation all have nothing to act on.
// What is left is navigation (which XTC does *better* than EPUB, because the page count is exact
// and never repaginates) and the format-agnostic tools.
class XtcReaderMenuActivity final : public MenuListActivity {
 public:
  // Menu actions identified by the StrId of the menu item, interpreted by the parent activity.
  enum class MenuAction {
    NONE,
    SELECT_CHAPTER,
    GO_TO_PERCENT,
    GO_TO_PAGE,
    BOOK_INFO,
    READING_STATS_FOR_BOOK,
    MARK_AS_READ,
    DELETE_CACHE,
    GO_HOME,
  };

  explicit XtcReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                 int currentPage, int totalPages, bool hasChapters);

  void render(RenderLock&&) override;

 private:
  void buildMenuItems(bool hasChapters);
  static MenuAction actionForNameId(StrId nameId);
  void finishWithAction(MenuAction action);

  // MenuListActivity overrides
  void onActionSelected(int index) override;
  void onBackPressed() override;

  std::string title;
  int currentPage = 0;
  int totalPages = 0;
};
