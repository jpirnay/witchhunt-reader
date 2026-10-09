#include "FileContextMenuActivity.h"

#include <FsHelpers.h>
#include <I18n.h>

#include "../ActivityResult.h"
#include "../settings/SettingInfo.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "components/UITheme.h"

FileContextMenuActivity::FileContextMenuActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& filePath,
    CrossPointSettings::FILE_SORT_MODE sortMode, CrossPointSettings::FILE_SORT_DIRECTION sortDirection,
    const bool offerDirectoryActions, const bool searchActive, const bool offerGoToFolder,
    const bool offerFileManagement, const bool offerViewChoice, const ListSource source, const uint8_t view)
    : MenuListActivity("FileContextMenu", renderer, mappedInput),
      filePath(filePath),
      isBrowserMode(filePath.empty()),
      offerDirectoryActions(offerDirectoryActions),
      searchActive(searchActive),
      offerGoToFolder(offerGoToFolder),
      offerFileManagement(offerFileManagement),
      offerViewChoice(offerViewChoice),
      source(source),
      sortMode(static_cast<uint8_t>(sortMode)),
      sortDirection(static_cast<uint8_t>(sortDirection)),
      showHiddenFiles(SETTINGS.showHiddenFiles),
      showFileExtensions(SETTINGS.showFileExtensions),
      browserView(view) {
  buildMenuItems();
}

void FileContextMenuActivity::buildMenuItems() {
  auto* self = this;

  // Entering the folder is what someone opening this menu on a directory almost always came for.
  if (offerDirectoryActions) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_OPEN, SettingAction::None));
  }

  // --- Display options (always shown: files, directories, unsupported types) ---
  const bool fixedOrder = source != ListSource::Folder;
  if (!fixedOrder) {
    menuItems.push_back(SettingInfo::Separator(StrId::STR_SORT_BY));

    // Sort mode: single cycling item Name -> Date -> Size -> Type
    menuItems.push_back(SettingInfo::DynamicEnumCtx(
        StrId::STR_SORT_BY, {StrId::STR_SORT_NAME, StrId::STR_SORT_DATE, StrId::STR_SORT_SIZE, StrId::STR_SORT_TYPE},
        self, [](const void* ctx) -> uint8_t { return static_cast<const FileContextMenuActivity*>(ctx)->sortMode; },
        [](void* ctx, uint8_t v) { static_cast<FileContextMenuActivity*>(ctx)->sortMode = v; }));

    // Sort direction: single cycling item Ascending -> Descending
    menuItems.push_back(SettingInfo::DynamicEnumCtx(
        StrId::STR_SORT_DIR, {StrId::STR_SORT_ASC, StrId::STR_SORT_DESC}, self,
        [](const void* ctx) -> uint8_t { return static_cast<const FileContextMenuActivity*>(ctx)->sortDirection; },
        [](void* ctx, uint8_t v) { static_cast<FileContextMenuActivity*>(ctx)->sortDirection = v; }));
  }

  menuItems.push_back(SettingInfo::Separator(StrId::STR_SHOW_FILES));
  // Visibility: show hidden files (OFF/ON)
  if (!fixedOrder) {
    menuItems.push_back(SettingInfo::DynamicEnumCtx(
        StrId::STR_SHOW_HIDDEN_FILES, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, self,
        [](const void* ctx) -> uint8_t {
          return static_cast<const FileContextMenuActivity*>(ctx)->showHiddenFiles ? 1 : 0;
        },
        [](void* ctx, uint8_t v) { static_cast<FileContextMenuActivity*>(ctx)->showHiddenFiles = (v != 0) ? 1 : 0; }));
  }

  // Visibility: show file extensions (OFF/ON)
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_SHOW_FILE_EXTENSIONS, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, self,
      [](const void* ctx) -> uint8_t {
        return static_cast<const FileContextMenuActivity*>(ctx)->showFileExtensions ? 1 : 0;
      },
      [](void* ctx, uint8_t v) { static_cast<FileContextMenuActivity*>(ctx)->showFileExtensions = (v != 0) ? 1 : 0; }));

  if (offerViewChoice) {
    menuItems.push_back(SettingInfo::DynamicEnumCtx(
        StrId::STR_BROWSER_VIEW, {StrId::STR_VIEW_FILENAMES, StrId::STR_VIEW_DETAILS, StrId::STR_VIEW_COVERS}, self,
        [](const void* ctx) -> uint8_t { return static_cast<const FileContextMenuActivity*>(ctx)->browserView; },
        [](void* ctx, uint8_t v) { static_cast<FileContextMenuActivity*>(ctx)->browserView = v; }));
  }

  // Browser mode stops after the display options, except for the two things that belong to the
  // folder you are standing in rather than to any row: making one, and (for a directory)
  // deleting the one selected.
  if (isBrowserMode) {
    if (fixedOrder) {  // nothing in the list is selected: only what belongs to the list itself
      if (source == ListSource::Index) {
        menuItems.push_back(SettingInfo::Separator(StrId::STR_TOOL_UTILITIES));
        menuItems.push_back(SettingInfo::Action(StrId::STR_REFRESH_LIBRARY, SettingAction::None));
      }
      return;
    }
    menuItems.push_back(SettingInfo::Separator(StrId::STR_TOOL_UTILITIES));
    // Search narrows the folder you are standing in, so it sits with the folder actions rather
    // than the row actions. Clearing only appears when there is something to clear.
    menuItems.push_back(SettingInfo::Action(StrId::STR_SEARCH, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_SEARCH_ALL, SettingAction::None));
    if (searchActive) {
      menuItems.push_back(SettingInfo::Action(StrId::STR_CLEAR_SEARCH, SettingAction::None));
    }
    if (offerFileManagement) {
      menuItems.push_back(SettingInfo::Action(StrId::STR_NEW_FOLDER, SettingAction::None));
      if (offerDirectoryActions) {
        menuItems.push_back(SettingInfo::Action(StrId::STR_REMOVE, SettingAction::None));
      }
    }
    return;
  }

  // --- File-specific actions (only when a supported file is selected) ---
  const std::string_view name{filePath};
  const bool isBin = FsHelpers::checkFileExtension(name, ".bin");
  const bool isEpub = FsHelpers::hasEpubExtension(name);
  const bool isXtc = FsHelpers::hasXtcExtension(name);
  const bool isTxt = FsHelpers::hasTxtExtension(name) || FsHelpers::hasMarkdownExtension(name);
  const bool isImage =
      FsHelpers::hasBmpExtension(name) || FsHelpers::hasJpgExtension(name) || FsHelpers::hasPngExtension(name);

  menuItems.push_back(SettingInfo::Separator(StrId::STR_TOOL_UTILITIES));

  if (isBin) {
    // Only the firmware picker lists a .bin, and there Select already flashes it. What the picker
    // cannot do without this menu is get rid of an image that is no longer wanted.
    menuItems.push_back(SettingInfo::Action(StrId::STR_REMOVE, SettingAction::None));
  } else if (isImage) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_OPEN, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_SET_SLEEP_SCREEN, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_REMOVE, SettingAction::None));
  } else if (isEpub) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_OPEN, SettingAction::None));
    if (KOREADER_STORE.hasCredentials()) {
      menuItems.push_back(SettingInfo::Action(StrId::STR_FETCH_AND_OPEN, SettingAction::None));
    }
    menuItems.push_back(SettingInfo::Action(StrId::STR_MARK_AS_READ, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_INFO, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_DELETE_CACHE, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_REMOVE, SettingAction::None));
  } else if (isXtc) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_OPEN, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_MARK_AS_READ, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_INFO, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_DELETE_CACHE, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_REMOVE, SettingAction::None));
  } else if (isTxt) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_OPEN, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_MARK_AS_READ, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_REMOVE, SettingAction::None));
  } else if (offerFileManagement) {
    // Only All files lists a file the reader cannot open. Getting rid of it is most of what there
    // is to do with one here.
    menuItems.push_back(SettingInfo::Action(StrId::STR_REMOVE, SettingAction::None));
  }

  if (fixedOrder) {
    // Off the list, not off the card: Recent Books swaps the file's own Remove for that.
    if (source == ListSource::Recents) {
      for (auto& item : menuItems) {
        if (item.nameId == StrId::STR_REMOVE) item.nameId = StrId::STR_REMOVE_FROM_RECENTS;
      }
    }
    if (offerGoToFolder) menuItems.push_back(SettingInfo::Action(StrId::STR_GO_TO_FOLDER, SettingAction::None));
    // New and Authors: build the index again, for what was changed where the firmware did not see it.
    if (source == ListSource::Index) {
      menuItems.push_back(SettingInfo::Action(StrId::STR_REFRESH_LIBRARY, SettingAction::None));
    }
    return;
  }

  // Every file can be moved, whatever its type: this is a rename, and rename does not care what
  // the bytes are.
  if (offerFileManagement) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_MOVE_TO_FOLDER, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_NEW_FOLDER, SettingAction::None));
  }
  // Searching belongs to the folder, not to the row, so it has to be reachable with a file
  // selected as well -- which is most of the time.
  if (offerGoToFolder) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_GO_TO_FOLDER, SettingAction::None));
  }
  menuItems.push_back(SettingInfo::Action(StrId::STR_SEARCH, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_SEARCH_ALL, SettingAction::None));
  // No Clear search here. Back already ends a search, which is the gesture people reach for
  // anyway, and this menu is about the file you selected -- clearing would throw that file away
  // and drop you at the top of the folder you searched from. The folder menu still offers it.
}

void FileContextMenuActivity::finishWithDisplayOptions(Action action) {
  MenuResult res;
  res.action = static_cast<int>(action);
  res.sortMode = sortMode;
  res.sortDirection = sortDirection;
  res.showHiddenFiles = showHiddenFiles;
  res.showFileExtensions = showFileExtensions;
  res.browserView = browserView;
  ActivityResult result{std::move(res)};
  result.isCancelled = false;
  setResult(std::move(result));
  finish();
}

void FileContextMenuActivity::onActionSelected(int index) {
  if (index < 0 || index >= static_cast<int>(menuItems.size())) return;

  const StrId nameId = menuItems[index].nameId;
  Action action = Action::None;

  // Only file-specific actions reach here; display options are DynamicEnum
  // items handled inline via their setters (no finish() until the menu closes).
  if (nameId == StrId::STR_OPEN) {
    action = Action::Open;
  } else if (nameId == StrId::STR_FETCH_AND_OPEN) {
    action = Action::FetchAndOpen;
  } else if (nameId == StrId::STR_MARK_AS_READ) {
    action = Action::MarkAsRead;
  } else if (nameId == StrId::STR_INFO) {
    action = Action::Info;
  } else if (nameId == StrId::STR_DELETE_CACHE) {
    action = Action::DeleteCache;
  } else if (nameId == StrId::STR_SET_SLEEP_SCREEN) {
    action = Action::SetAsSleepCover;
  } else if (nameId == StrId::STR_MOVE_TO_FOLDER) {
    action = Action::MoveTo;
  } else if (nameId == StrId::STR_SEARCH) {
    action = Action::Search;
  } else if (nameId == StrId::STR_SEARCH_ALL) {
    action = Action::SearchAll;
  } else if (nameId == StrId::STR_GO_TO_FOLDER) {
    action = Action::GoToFolder;
  } else if (nameId == StrId::STR_CLEAR_SEARCH) {
    action = Action::ClearSearch;
  } else if (nameId == StrId::STR_NEW_FOLDER) {
    action = Action::NewFolder;
  } else if (nameId == StrId::STR_REMOVE) {
    action = Action::Remove;
  } else if (nameId == StrId::STR_REMOVE_FROM_RECENTS) {
    action = Action::RemoveFromRecents;
  } else if (nameId == StrId::STR_REFRESH_LIBRARY) {
    action = Action::RefreshLibrary;
  }

  if (action == Action::None) return;

  MenuResult res;
  res.action = static_cast<int>(action);
  ActivityResult result{std::move(res)};
  result.isCancelled = false;
  setResult(std::move(result));
  finish();
}

void FileContextMenuActivity::onBackPressed() {
  // Closing the menu commits any display-option changes the user cycled through.
  finishWithDisplayOptions(Action::DisplayOptionsChanged);
}

void FileContextMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = listContentRect();

  // Header: bare filename when a file is selected, otherwise a generic title
  const std::string header = isBrowserMode
                                 ? std::string(source == ListSource::Recents ? tr(STR_MENU_RECENT_BOOKS)
                                               : source == ListSource::Index ? tr(STR_LIBRARY)
                                                                             : tr(STR_SORT_BY))
                                 : [&] {
                                     const auto slashPos = filePath.rfind('/');
                                     return (slashPos == std::string::npos) ? filePath : filePath.substr(slashPos + 1);
                                   }();
  GUI.drawHeader(renderer, listHeaderRect(), header.c_str());

  const int contentTop = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.y + contentRect.height - contentTop - metrics.verticalSpacing;
  drawMenuList(Rect{contentRect.x, contentTop, contentRect.width, contentHeight});

  drawListHints();

  renderer.displayBuffer();
}
