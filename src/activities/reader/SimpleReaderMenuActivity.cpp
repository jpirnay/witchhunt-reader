#include "SimpleReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <utility>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"

SimpleReaderMenuActivity::SimpleReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   Options options)
    : MenuListActivity("SimpleReaderMenu", renderer, mappedInput), options(std::move(options)) {
  buildMenuItems();
}

void SimpleReaderMenuActivity::buildMenuItems() {
  menuItems.reserve(11);

  // --- Navigation ---
  menuItems.push_back(SettingInfo::Separator(StrId::STR_READER_NAVIGATION));
  if (options.hasChapters) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_SELECT_CHAPTER, SettingAction::None));
  }
  menuItems.push_back(SettingInfo::Action(StrId::STR_GO_TO_PERCENT, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_GO_TO_PAGE, SettingAction::None));

  // --- Bookmarks ---
  if (options.canStarPages) {
    menuItems.push_back(SettingInfo::Separator(StrId::STR_READER_BOOKMARKS));
    menuItems.push_back(SettingInfo::Action(StrId::STR_STAR_PAGE, SettingAction::None));
    if (options.hasStarredPages) {
      menuItems.push_back(SettingInfo::Action(StrId::STR_STARRED_PAGES, SettingAction::None));
    }
  }

  // --- Tools ---
  menuItems.push_back(SettingInfo::Separator(StrId::STR_READER_TOOLS));
  menuItems.push_back(SettingInfo::Action(StrId::STR_BOOK_INFO, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_READING_STATS_FOR_THIS_BOOK, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_MARK_AS_READ, SettingAction::None));
  if (options.canDeleteCache) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_DELETE_CACHE, SettingAction::None));
  }
  menuItems.push_back(SettingInfo::Action(StrId::STR_GO_HOME_BUTTON, SettingAction::None));
}

SimpleReaderMenuActivity::MenuAction SimpleReaderMenuActivity::actionForNameId(const StrId nameId) {
  switch (nameId) {
    case StrId::STR_SELECT_CHAPTER:
      return MenuAction::SELECT_CHAPTER;
    case StrId::STR_GO_TO_PERCENT:
      return MenuAction::GO_TO_PERCENT;
    case StrId::STR_GO_TO_PAGE:
      return MenuAction::GO_TO_PAGE;
    case StrId::STR_STAR_PAGE:
      return MenuAction::STAR_PAGE;
    case StrId::STR_STARRED_PAGES:
      return MenuAction::STARRED_PAGES;
    case StrId::STR_BOOK_INFO:
      return MenuAction::BOOK_INFO;
    case StrId::STR_READING_STATS_FOR_THIS_BOOK:
      return MenuAction::READING_STATS_FOR_BOOK;
    case StrId::STR_MARK_AS_READ:
      return MenuAction::MARK_AS_READ;
    case StrId::STR_DELETE_CACHE:
      return MenuAction::DELETE_CACHE;
    case StrId::STR_GO_HOME_BUTTON:
      return MenuAction::GO_HOME;
    default:
      return MenuAction::NONE;
  }
}

std::string SimpleReaderMenuActivity::getItemValueString(const int index) const {
  if (menuItems[index].nameId == StrId::STR_STAR_PAGE) {
    return options.currentPageStarred ? std::string(tr(STR_STATE_ON)) : std::string(tr(STR_STATE_OFF));
  }
  return MenuListActivity::getItemValueString(index);
}

void SimpleReaderMenuActivity::finishWithAction(const MenuAction action) {
  MenuResult payload;
  payload.action = static_cast<int>(action);
  setResult(std::move(payload));
  finish();
}

void SimpleReaderMenuActivity::onActionSelected(const int index) {
  finishWithAction(actionForNameId(menuItems[index].nameId));
}

void SimpleReaderMenuActivity::onBackPressed() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void SimpleReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  // Title
  const std::string truncTitle =
      renderer.truncatedText(UI_12_FONT_ID, options.title.c_str(), contentRect.width - 40, EpdFontFamily::BOLD);
  const int titleX =
      contentRect.x +
      (contentRect.width - renderer.getTextWidth(UI_12_FONT_ID, truncTitle.c_str(), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentRect.y, truncTitle.c_str(), true, EpdFontFamily::BOLD);

  // Progress summary. Unlike EPUB there is no chapter/book split to report — these formats paginate
  // the whole document at once — so one exact "page x/y  z%" line says everything.
  if (options.totalPages > 0) {
    const std::string progressLine = std::to_string(options.currentPage) + "/" + std::to_string(options.totalPages) +
                                     "  " + std::to_string(options.currentPage * 100 / options.totalPages) + "%";
    renderer.drawCenteredText(UI_10_FONT_ID, 45, progressLine.c_str());
  }

  // Menu Items
  const int startY = 75 + contentRect.y;
  const int listHeight = contentRect.height - (startY - contentRect.y);
  drawMenuList(Rect{contentRect.x, startY, contentRect.width, listHeight});

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
