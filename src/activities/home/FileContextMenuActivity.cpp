#include "FileContextMenuActivity.h"

#include <FsHelpers.h>
#include <I18n.h>

#include "../ActivityResult.h"
#include "../settings/SettingInfo.h"
#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "components/UITheme.h"

FileContextMenuActivity::FileContextMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 const std::string& filePath,
                                                 CrossPointSettings::FILE_SORT_MODE sortMode,
                                                 CrossPointSettings::FILE_SORT_DIRECTION sortDirection)
    : MenuListActivity("FileContextMenu", renderer, mappedInput),
      filePath(filePath),
      isBrowserMode(filePath.empty()),
      sortMode(static_cast<uint8_t>(sortMode)),
      sortDirection(static_cast<uint8_t>(sortDirection)),
      showHiddenFiles(SETTINGS.showHiddenFiles),
      showFileExtensions(SETTINGS.showFileExtensions) {
  buildMenuItems();
}

void FileContextMenuActivity::buildMenuItems() {
  auto* self = this;

  // --- Display options (always shown: files, directories, unsupported types) ---
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

  menuItems.push_back(SettingInfo::Separator(StrId::STR_SHOW_FILES));
  // Visibility: show hidden files (OFF/ON)
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_SHOW_HIDDEN_FILES, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, self,
      [](const void* ctx) -> uint8_t {
        return static_cast<const FileContextMenuActivity*>(ctx)->showHiddenFiles ? 1 : 0;
      },
      [](void* ctx, uint8_t v) { static_cast<FileContextMenuActivity*>(ctx)->showHiddenFiles = (v != 0) ? 1 : 0; }));

  // Visibility: show file extensions (OFF/ON)
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_SHOW_FILE_EXTENSIONS, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, self,
      [](const void* ctx) -> uint8_t {
        return static_cast<const FileContextMenuActivity*>(ctx)->showFileExtensions ? 1 : 0;
      },
      [](void* ctx, uint8_t v) { static_cast<FileContextMenuActivity*>(ctx)->showFileExtensions = (v != 0) ? 1 : 0; }));

  // In browser mode (no file / directory / unsupported type) we stop here.
  if (isBrowserMode) return;

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

void FileContextMenuActivity::finishWithDisplayOptions(Action action) {
  MenuResult res;
  res.action = static_cast<int>(action);
  res.sortMode = sortMode;
  res.sortDirection = sortDirection;
  res.showHiddenFiles = showHiddenFiles;
  res.showFileExtensions = showFileExtensions;
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
  } else if (nameId == StrId::STR_FLASH_FIRMWARE) {
    action = Action::FlashFirmware;
  } else if (nameId == StrId::STR_REMOVE) {
    action = Action::Remove;
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
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);

  // Header: bare filename when a file is selected, otherwise a generic title
  const std::string header = isBrowserMode ? std::string(tr(STR_SORT_BY)) : [&] {
    const auto slashPos = filePath.rfind('/');
    return (slashPos == std::string::npos) ? filePath : filePath.substr(slashPos + 1);
  }();
  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 header.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;
  drawMenuList(Rect{contentRect.x, contentTop, contentRect.width, contentHeight});

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
