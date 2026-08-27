#pragma once
#include <I18n.h>

#include <string>

#include "../MenuListActivity.h"

// Reader menu shared by the formats that are *not* the EPUB engine: TXT, Markdown and the
// pre-rendered XTC/XTCH containers.
//
// What those three have in common is more than it looks. None of them has a live layout to
// re-decide from a menu — TXT/MD repaginate from settings rather than per book, and an XTC page is
// a finished bitmap — so the whole appearance half of EpubReaderMenuActivity (font, size,
// alignment, hyphenation, embedded style, images, rotation) has nothing to act on. What is left is
// navigation, bookmarks and the format-agnostic tools, and all three formats hold an *exact* page
// count, so "go to page" and "go to %" land precisely instead of on an estimate.
//
// Entries a given reader cannot back are left out of the list rather than shown disabled: the menu
// is short enough that a missing row reads as "not applicable to this format".
class SimpleReaderMenuActivity final : public MenuListActivity {
 public:
  // Menu actions identified by the StrId of the menu item, interpreted by the parent activity.
  enum class MenuAction {
    NONE,
    SELECT_CHAPTER,
    GO_TO_PERCENT,
    GO_TO_PAGE,
    STAR_PAGE,
    STARRED_PAGES,
    BOOK_INFO,
    READING_STATS_FOR_BOOK,
    MARK_AS_READ,
    DELETE_CACHE,
    GO_HOME,
  };

  struct Options {
    std::string title;
    int currentPage = 0;  // 1-based, for the header line only
    int totalPages = 0;
    // A chapter/heading list worth jumping into (XTC chapters, Markdown headings).
    bool hasChapters = false;
    bool canStarPages = false;
    bool hasStarredPages = false;
    bool currentPageStarred = false;
    // Only offered where clearing the cache actually recovers something: an XTH book leaves tens
    // of megabytes of transposed page planes behind, a TXT page index is a few kilobytes.
    bool canDeleteCache = false;
  };

  explicit SimpleReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Options options);

  void render(RenderLock&&) override;

 private:
  void buildMenuItems();
  static MenuAction actionForNameId(StrId nameId);
  void finishWithAction(MenuAction action);

  // MenuListActivity overrides
  std::string getItemValueString(int index) const override;
  void onActionSelected(int index) override;
  void onBackPressed() override;

  Options options;
};
