#include "FileContextMenuActivity.h"

#include <FsHelpers.h>
#include <I18n.h>

#include "../ActivityResult.h"
#include "../settings/SettingInfo.h"
#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "components/UITheme.h"

FileContextMenuActivity::FileContextMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 const std::string& filePath)
    : MenuListActivity("FileContextMenu", renderer, mappedInput), filePath(filePath), isBrowserMode(filePath.empty()) {
  // Display options section (always shown in browser mode, or when a file is selected)
  menuItems.push_back(SettingInfo::Separator(StrId::STR_SORT_BY));
  menuItems.push_back(SettingInfo::Action(StrId::STR_SORT_NAME, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_SORT_DATE, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_SORT_SIZE, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_SORT_TYPE, SettingAction::None));

  menuItems.push_back(SettingInfo::Separator(StrId::STR_SORT_DIR));
  menuItems.push_back(SettingInfo::Action(StrId::STR_SORT_ASC, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_SORT_DESC, SettingAction::None));

  menuItems.push_back(SettingInfo::Action(StrId::STR_SHOW_HIDDEN_FILES, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_SHOW_FILE_EXTENSIONS, SettingAction::None));

  // If no file selected (browser options mode), stop here
  if (isBrowserMode) {
    return;
  }

  // File-specific actions (only when a file is selected)
  const std::string_view name{filePath};
  const bool isBin = FsHelpers::checkFileExtension(name, ".bin");
  const bool isEpub = FsHelpers::hasEpubExtension(name);
  const bool isXtc = FsHelpers::hasXtcExtension(name);
  const bool isTxt = FsHelpers::hasTxtExtension(name) || FsHelpers::hasMarkdownExtension(name);
  const bool isImage =
      FsHelpers::hasBmpExtension(name) || FsHelpers::hasJpgExtension(name) || FsHelpers::hasPngExtension(name);

  menuItems.push_back(SettingInfo::Separator(StrId::STR_TOOL_UTILITIES));

  if (isBin) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_FLASH_FIRMWARE, SettingAction::None));
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
  }
}

void FileContextMenuActivity::onActionSelected(int index) {
  const StrId nameId = menuItems[index].nameId;
  Action action = Action::None;

  // Display options section
  if (nameId == StrId::STR_SORT_NAME) {
    action = Action::ChangeSortMode;
  } else if (nameId == StrId::STR_SORT_DATE) {
    action = Action::ChangeSortMode;
  } else if (nameId == StrId::STR_SORT_SIZE) {
    action = Action::ChangeSortMode;
  } else if (nameId == StrId::STR_SORT_TYPE) {
    action = Action::ChangeSortMode;
  } else if (nameId == StrId::STR_SORT_ASC) {
    action = Action::ChangeSortDirection;
  } else if (nameId == StrId::STR_SORT_DESC) {
    action = Action::ChangeSortDirection;
  } else if (nameId == StrId::STR_SHOW_HIDDEN_FILES) {
    action = Action::ToggleHiddenFiles;
  } else if (nameId == StrId::STR_SHOW_FILE_EXTENSIONS) {
    action = Action::ToggleExtensions;
  }
  // File-specific actions (only if not in browser mode)
  else if (!isBrowserMode) {
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
    } else if (nameId == StrId::STR_FLASH_FIRMWARE) {
      action = Action::FlashFirmware;
    } else if (nameId == StrId::STR_REMOVE) {
      action = Action::Remove;
    }
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
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void FileContextMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);

  // Show the bare filename (without path) as the header
  const auto slashPos = filePath.rfind('/');
  const std::string fileName = (slashPos == std::string::npos) ? filePath : filePath.substr(slashPos + 1);
  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 fileName.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;
  drawMenuList(Rect{contentRect.x, contentTop, contentRect.width, contentHeight});

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
