#include "FileBrowserActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "../ActivityManager.h"
#include "../ActivityResult.h"
#include "../reader/FinishedBookActivity.h"
#include "../settings/SdFirmwareUpdateActivity.h"
#include "../util/BmpViewerActivity.h"
#include "../util/ConfirmationActivity.h"
#include "BookInfoActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FileContextMenuActivity.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Legacy global function (for backward compat if needed elsewhere)
void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    // Directories first
    bool isDir1 = str1.back() == '/';
    bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

    return FsHelpers::naturalCompare(str1.c_str(), str2.c_str()) < 0;
  });
}

void FileBrowserActivity::loadFiles() {
  files.clear();
  fileSizes.clear();
  fileDateTimes.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if ((!SETTINGS.showHiddenFiles && name[0] == '.') || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
      fileSizes.push_back(0);      // directories have size 0
      fileDateTimes.push_back(0);  // will use default date
    } else {
      std::string_view filename{name};
      bool shouldAdd = false;
      if (mode == Mode::PickFirmware) {
        shouldAdd = FsHelpers::checkFileExtension(filename, ".bin");
      } else {
        shouldAdd = FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
                    FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
                    FsHelpers::hasBmpExtension(filename) || FsHelpers::hasJpgExtension(filename) ||
                    FsHelpers::hasPngExtension(filename);
      }
      if (shouldAdd) {
        files.emplace_back(filename);
        fileSizes.push_back(static_cast<uint32_t>(file.fileSize()));
        uint16_t fdate = 0, ftime = 0;
        file.getModifyDateTime(&fdate, &ftime);
        uint32_t combined = (static_cast<uint32_t>(fdate) << 16) | ftime;
        fileDateTimes.push_back(combined);
      }
    }
    file.close();
  }
  root.close();

  // Try to use FileIndex for large folders (64+ entries); fall back to in-RAM sort
  tryOpenFileIndex();

  // Only sort in-RAM if FileIndex is not in use
  if (!fileIndex) {
    sortFileList();
  }
}

bool FileBrowserActivity::acceptFileForBrowser(const char* name, bool isDir) {
  // Mirror loadFiles() filter logic for FileIndex
  if (!SETTINGS.showHiddenFiles && name[0] == '.') return false;
  if (strcmp(name, "System Volume Information") == 0) return false;
  if (isDir) return true;  // all dirs accepted

  // File: check extension
  std::string_view filename{name};
  return FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
         FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
         FsHelpers::hasBmpExtension(filename) || FsHelpers::hasJpgExtension(filename) ||
         FsHelpers::hasPngExtension(filename);
}

void FileBrowserActivity::tryOpenFileIndex() {
  // For large folders (64+ entries), use SD-backed index to keep RAM bounded
  if (files.size() < FILE_INDEX_THRESHOLD) {
    fileIndex = nullptr;
    return;
  }

  fileIndex = std::make_unique<FileIndex>();
  const FileIndex::SortMode indexSortMode = static_cast<FileIndex::SortMode>(sortMode);
  if (!fileIndex->open(basepath.c_str(), indexSortMode, acceptFileForBrowser)) {
    LOG_ERR("FBR", "FileIndex build failed for %s, falling back to in-RAM sort", basepath.c_str());
    fileIndex = nullptr;
  }
}

size_t FileBrowserActivity::getDisplayEntryCount() const { return fileIndex ? fileIndex->totalCount() : files.size(); }

bool FileBrowserActivity::useFileIndexForEntry(size_t displayIndex, FileIndex::Entry& out) {
  if (!fileIndex) return false;
  const bool desc = (sortDirection == CrossPointSettings::SORT_DESCENDING);
  return fileIndex->entryAt(displayIndex, desc, out);
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  loadFiles();
  selectorIndex = 0;

  if (!focusName.empty()) {
    const size_t idx = findEntry(focusName);
    if (idx < files.size()) {
      selectorIndex = static_cast<int>(idx);
    }
    focusName.clear();
  }

  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
  fileSizes.clear();
  fileDateTimes.clear();
  if (fileIndex) fileIndex->close();
  fileIndex = nullptr;
}

void FileBrowserActivity::clearFileMetadata(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("FileBrowser", "Cleared metadata cache for: %s", fullPath.c_str());
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    Xtc(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("FileBrowser", "Cleared metadata cache for: %s", fullPath.c_str());
  }
}

// Iterative post-order traversal: clear book caches then delete files/dirs.
// Adapted from upstream PR #1892 (WuTofu) to handle our EPUB+XTC cache clearing.
bool FileBrowserActivity::removeDirRecursive(const std::string& fullPath) {
  auto file = Storage.open(fullPath.c_str());
  if (!file) {
    LOG_ERR("FBR", "Failed to open for removal: %s", fullPath.c_str());
    return false;
  }
  if (!file.isDirectory()) {
    file.close();
    clearFileMetadata(fullPath);
    return Storage.remove(fullPath.c_str());
  }
  file.close();

  constexpr size_t NAME_BUF = 500;
  char nameBuf[NAME_BUF];

  // Stack of (path, postOrder): postOrder=true means rmdir this path after its children.
  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({fullPath, false});

  while (!stack.empty()) {
    auto [currentPath, postOrder] = std::move(stack.back());
    stack.pop_back();

    if (postOrder) {
      if (!Storage.rmdir(currentPath.c_str())) {
        LOG_ERR("FBR", "Failed to rmdir: %s", currentPath.c_str());
        return false;
      }
      continue;
    }

    auto dir = Storage.open(currentPath.c_str());
    if (!dir || !dir.isDirectory()) {
      LOG_ERR("FBR", "Failed to open dir: %s", currentPath.c_str());
      return false;
    }

    stack.push_back({currentPath, true});

    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(nameBuf, NAME_BUF);
      if (strcmp(nameBuf, ".") == 0 || strcmp(nameBuf, "..") == 0) continue;
      std::string entryPath = currentPath;
      if (entryPath.back() != '/') entryPath += '/';
      entryPath += nameBuf;
      const bool isDir = entry.isDirectory();
      entry.close();
      if (isDir) {
        stack.push_back({std::move(entryPath), false});
      } else {
        clearFileMetadata(entryPath);
        if (!Storage.remove(entryPath.c_str())) {
          LOG_ERR("FBR", "Failed to remove file: %s", entryPath.c_str());
          dir.close();
          return false;
        }
      }
    }
    dir.close();
  }
  return true;
}

void FileBrowserActivity::loop() {
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Back) {
      if (ev.type == ButtonEventManager::PressType::Long) {
        if (mode == Mode::Books) {
          onGoHome();
          return;
        }
        // PickFirmware: long Back = same as short Back (cancel / up dir)
      }
      if (ev.type == ButtonEventManager::PressType::Short || ev.type == ButtonEventManager::PressType::Long) {
        if (basepath != "/") {
          const std::string oldPath = basepath;
          basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
          if (basepath.empty()) basepath = "/";
          loadFiles();
          const auto pos = oldPath.find_last_of('/');
          const std::string dirName = oldPath.substr(pos + 1) + "/";
          const size_t idx = findEntry(dirName);
          selectorIndex = (idx < files.size()) ? static_cast<int>(idx) : 0;
          requestUpdate();
        } else if (mode == Mode::PickFirmware) {
          // At root in PickFirmware: cancel back to caller.
          ActivityResult res;
          res.isCancelled = true;
          setResult(std::move(res));
          finish();
        } else {
          onGoHome();
        }
        return;
      }
    }

    if (ev.button == MappedInputManager::Button::Confirm &&
        (ev.type == ButtonEventManager::PressType::Short || ev.type == ButtonEventManager::PressType::Long)) {
      if (files.empty()) return;

      const std::string& entry = files[selectorIndex];
      const bool isDirectory = (entry.back() == '/');
      const bool longPress = (ev.type == ButtonEventManager::PressType::Long);

      if (isDirectory) {
        // Long press on a directory has no useful sync action; ignore.
        if (longPress) return;
        if (basepath.back() != '/') basepath += "/";
        basepath += entry.substr(0, entry.length() - 1);
        loadFiles();
        selectorIndex = 0;
        requestUpdate();
      } else if (mode == Mode::PickFirmware) {
        // Firmware picker: return the selected path to the caller.
        std::string cleanBasePath = basepath;
        if (cleanBasePath.back() != '/') cleanBasePath += "/";
        ActivityResult res{FilePathResult{cleanBasePath + entry}};
        res.isCancelled = false;
        setResult(std::move(res));
        finish();
        return;
      } else {
        std::string fullPath = basepath;
        if (fullPath.back() != '/') fullPath += "/";
        fullPath += entry;
        // Long-press on an EPUB arms an AUTO_PULL before the reader renders its first page.
        // Restricted to EPUB so long-pressing a non-EPUB cannot leak the flag to the reader.
        if (longPress && KOREADER_STORE.hasCredentials() && FsHelpers::hasEpubExtension(fullPath)) {
          auto& sync = APP_STATE.koReaderSyncSession;
          sync.autoPullEpubPath = fullPath;
          sync.exitToHomeAfterSync = false;
          APP_STATE.saveToFile();
        }
        ReturnHint hint;
        hint.target = ReturnTo::FileBrowser;
        hint.path = basepath;
        hint.selectName = entry;
        activityManager.replaceWithReader(std::move(fullPath), std::move(hint));
      }
      return;
    }

    if (ev.button == MappedInputManager::Button::Left && ev.type == ButtonEventManager::PressType::Long) {
      if (files.empty()) {
        return;
      }

      const std::string& entry = files[selectorIndex];
      const bool isDirectory = (entry.back() == '/');
      std::string entryName = entry;
      if (isDirectory) entryName.pop_back();

      std::string cleanBase = basepath;
      if (cleanBase.back() != '/') cleanBase += "/";
      const std::string fullPath = cleanBase + entryName;

      doRemove(fullPath, entry, isDirectory);
      return;
    }

    if (ev.button == MappedInputManager::Button::Right && ev.type == ButtonEventManager::PressType::Short) {
      // Open the context menu for any selection. openContextMenu() shows
      // file-specific actions for supported files and the browser display
      // options (sort + visibility) for directories / unsupported types.
      openContextMenu();
      return;
    }
  }

  // Up/Down side buttons navigate the list (Left/Right are reserved for Back/Info actions)
  const int listSize = static_cast<int>(files.size());
  buttonNavigator.onNextList({MappedInputManager::Button::Down}, selectorIndex, listSize, [this] { requestUpdate(); });
  buttonNavigator.onPreviousList({MappedInputManager::Button::Up}, selectorIndex, listSize,
                                 [this] { requestUpdate(); });
}

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  if (SETTINGS.showFileExtensions) {
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

void FileBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);

  std::string folderName =
      (mode == Mode::PickFirmware)
          ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
          : ((basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1));
  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 folderName.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;
  if (files.empty()) {
    const char* emptyMsg = (mode == Mode::PickFirmware) ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND);
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop + 20, emptyMsg);
  } else {
    GUI.drawList(
        renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight}, files.size(), selectorIndex,
        [this](int index) { return getFileName(files[index]); }, nullptr,
        [this](int index) { return UITheme::getFileIcon(files[index]); });
  }

  // Side buttons (Up/Down) navigate; show their hints on the side
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  // Front buttons
  const char* backLabel = (basepath == "/") ? (mode == Mode::PickFirmware ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK);
  const bool selectingFirmwareFile = mode == Mode::PickFirmware && !files.empty() && files[selectorIndex].back() != '/';
  const char* confirmLabel = files.empty() ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : tr(STR_OPEN));
  // The Options menu is available for every entry in Books mode. The menu always
  // offers the browser display options (sort + visibility); supported files get
  // extra file-specific actions appended. So the hint shows for files and dirs alike.
  const bool showOptionsHint = mode == Mode::Books && !files.empty();
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, "", showOptionsHint ? tr(STR_OPTIONS) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return files.size();
}

std::string FileBrowserActivity::getFileExtension(const std::string& name) const {
  const char* dot = strrchr(name.c_str(), '.');
  if (!dot || dot == name.c_str() || name.back() == '/') {
    return "";  // directory, no extension, or dot-file
  }
  return std::string(dot + 1);
}

uint32_t FileBrowserActivity::getFileSize(const std::string& filePath) const {
  auto file = Storage.open(filePath.c_str());
  if (!file) return 0;
  uint32_t size = static_cast<uint32_t>(file.fileSize());
  file.close();
  return size;
}

uint32_t FileBrowserActivity::getFileDateTime(const std::string& filePath) const {
  auto file = Storage.open(filePath.c_str());
  if (!file) return 0;
  uint16_t date = 0, time = 0;
  file.getModifyDateTime(&date, &time);
  // Pack into 32-bit: (date << 16) | time (matching FAT format used by HalStorage)
  file.close();
  return (static_cast<uint32_t>(date) << 16) | time;
}

void FileBrowserActivity::sortFileList() {
  // Create index array to preserve metadata array alignment
  std::vector<size_t> indices(files.size());
  for (size_t i = 0; i < files.size(); ++i) indices[i] = i;

  std::sort(indices.begin(), indices.end(), [this](size_t idx_a, size_t idx_b) {
    const std::string& a = files[idx_a];
    const std::string& b = files[idx_b];
    const bool isDir_a = a.back() == '/';
    const bool isDir_b = b.back() == '/';

    // Directories always sort first
    if (isDir_a != isDir_b) return isDir_a;

    // Both are directories or both are files; apply sort mode
    const char* name_a = a.c_str();
    const char* name_b = b.c_str();

    int cmp = 0;  // -1 if a < b, 0 if equal, +1 if a > b

    switch (sortMode) {
      case CrossPointSettings::SORT_BY_NAME:
        cmp = FsHelpers::naturalCompare(name_a, name_b);
        break;

      case CrossPointSettings::SORT_BY_DATE: {
        // Use cached metadata (no file opens)
        uint32_t dt_a = (idx_a < fileDateTimes.size()) ? fileDateTimes[idx_a] : 0;
        uint32_t dt_b = (idx_b < fileDateTimes.size()) ? fileDateTimes[idx_b] : 0;
        if (dt_a < dt_b) {
          cmp = -1;
        } else if (dt_a > dt_b) {
          cmp = 1;
        } else {
          cmp = FsHelpers::naturalCompare(name_a, name_b);  // Tie: use name
        }
        break;
      }

      case CrossPointSettings::SORT_BY_SIZE: {
        // Use cached metadata (no file opens)
        uint32_t size_a = (idx_a < fileSizes.size()) ? fileSizes[idx_a] : 0;
        uint32_t size_b = (idx_b < fileSizes.size()) ? fileSizes[idx_b] : 0;
        if (size_a < size_b) {
          cmp = -1;
        } else if (size_a > size_b) {
          cmp = 1;
        } else {
          cmp = FsHelpers::naturalCompare(name_a, name_b);  // Tie: use name
        }
        break;
      }

      case CrossPointSettings::SORT_BY_TYPE: {
        std::string ext_a = getFileExtension(a);
        std::string ext_b = getFileExtension(b);
        // Case-insensitive extension comparison
        std::transform(ext_a.begin(), ext_a.end(), ext_a.begin(), ::tolower);
        std::transform(ext_b.begin(), ext_b.end(), ext_b.begin(), ::tolower);
        cmp = FsHelpers::naturalCompare(ext_a.c_str(), ext_b.c_str());
        if (cmp == 0) {
          cmp = FsHelpers::naturalCompare(name_a, name_b);  // Tie: use name
        }
        break;
      }

      default:
        cmp = 0;
    }

    // Apply sort direction
    if (sortDirection == CrossPointSettings::SORT_DESCENDING) {
      cmp = -cmp;
    }

    return cmp < 0;
  });

  // Reorder files vector and metadata arrays based on sorted indices
  std::vector<std::string> sorted_files(files.size());
  std::vector<uint32_t> sorted_sizes(fileSizes.size());
  std::vector<uint32_t> sorted_dateTimes(fileDateTimes.size());
  for (size_t i = 0; i < indices.size(); ++i) {
    size_t idx = indices[i];
    sorted_files[i] = files[idx];
    if (idx < fileSizes.size()) sorted_sizes[i] = fileSizes[idx];
    if (idx < fileDateTimes.size()) sorted_dateTimes[i] = fileDateTimes[idx];
  }
  files = std::move(sorted_files);
  fileSizes = std::move(sorted_sizes);
  fileDateTimes = std::move(sorted_dateTimes);
}

void FileBrowserActivity::openContextMenu() {
  // If no file selected or a directory selected, show browser options only
  if (files.empty() || selectorIndex < 0 || selectorIndex >= static_cast<int>(files.size())) {
    showBrowserOptionsMenu();
    return;
  }

  const std::string& entry = files[selectorIndex];
  if (entry.back() == '/') {
    showBrowserOptionsMenu();
    return;
  }

  std::string cleanBase = basepath;
  if (cleanBase.back() != '/') cleanBase += "/";
  const std::string fullPath = cleanBase + entry;

  startActivityForResult(
      std::make_unique<FileContextMenuActivity>(renderer, mappedInput, fullPath, sortMode, sortDirection),
      [this, fullPath, entry](const ActivityResult& res) {
        if (res.isCancelled) {
          requestUpdate();
          return;
        }
        const auto* menuRes = std::get_if<MenuResult>(&res.data);
        if (!menuRes) {
          requestUpdate();
          return;
        }
        handleContextMenuAction(menuRes->action, fullPath, entry, menuRes);
      });
}

void FileBrowserActivity::showBrowserOptionsMenu() {
  startActivityForResult(std::make_unique<FileContextMenuActivity>(renderer, mappedInput, "", sortMode, sortDirection),
                         [this](const ActivityResult& res) {
                           if (res.isCancelled) {
                             requestUpdate();
                             return;
                           }
                           const auto* menuRes = std::get_if<MenuResult>(&res.data);
                           if (!menuRes) {
                             requestUpdate();
                             return;
                           }
                           handleContextMenuAction(menuRes->action, "", "", menuRes);
                         });
}

void FileBrowserActivity::handleContextMenuAction(int action, const std::string& fullPath, const std::string& entry,
                                                  const MenuResult* menuRes) {
  using Action = FileContextMenuActivity::Action;
  const Action actionEnum = static_cast<Action>(action);

  // Display options: apply sort + visibility state returned from the menu.
  if (actionEnum == Action::DisplayOptionsChanged) {
    if (!menuRes) {
      requestUpdate();
      return;
    }
    sortMode = static_cast<CrossPointSettings::FILE_SORT_MODE>(menuRes->sortMode);
    sortDirection = static_cast<CrossPointSettings::FILE_SORT_DIRECTION>(menuRes->sortDirection);

    // Hidden-files visibility changes the set of entries, so reload from disk.
    const bool hiddenChanged = (SETTINGS.showHiddenFiles != menuRes->showHiddenFiles);
    SETTINGS.showHiddenFiles = menuRes->showHiddenFiles;
    SETTINGS.showFileExtensions = menuRes->showFileExtensions;
    SETTINGS.saveToFile();

    if (hiddenChanged) {
      loadFiles();  // re-enumerate (also re-sorts with current mode)
    } else {
      sortFileList();
    }
    requestUpdate();
    return;
  }

  // File-specific actions (require fullPath)
  switch (actionEnum) {
    case Action::Open: {
      ReturnHint hint;
      hint.target = ReturnTo::FileBrowser;
      hint.path = basepath;
      hint.selectName = entry;
      activityManager.replaceWithReader(std::string(fullPath), std::move(hint));
      return;
    }
    case Action::FetchAndOpen: {
      if (KOREADER_STORE.hasCredentials() && FsHelpers::hasEpubExtension(fullPath)) {
        auto& sync = APP_STATE.koReaderSyncSession;
        sync.autoPullEpubPath = fullPath;
        sync.exitToHomeAfterSync = false;
        APP_STATE.saveToFile();
      }
      ReturnHint hint;
      hint.target = ReturnTo::FileBrowser;
      hint.path = basepath;
      hint.selectName = entry;
      activityManager.replaceWithReader(std::string(fullPath), std::move(hint));
      return;
    }
    case Action::MarkAsRead:
      doMarkAsRead(fullPath);
      return;
    case Action::Info:
      startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, fullPath),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
    case Action::DeleteCache:
      doDeleteCache(fullPath, entry);
      return;
    case Action::SetAsSleepCover:
      doSetAsSleepCover(fullPath);
      return;
    case Action::FlashFirmware:
      doFlashFirmware(fullPath);
      return;
    case Action::Remove:
      doRemove(fullPath, entry, false);
      return;
    default:
      requestUpdate();
      return;
  }
}

void FileBrowserActivity::doMarkAsRead(const std::string& fullPath) {
  std::string cachePath;
  uint8_t data[7] = {0};
  size_t dataLen = 0;

  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub epub(fullPath, "/.crosspoint");
    epub.setupCacheDir();
    cachePath = epub.getCachePath();
    // 7-byte EPUB progress: spine(2) + page(2) + pageCount(2) + percent(1)
    data[6] = 100;
    dataLen = 7;
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    Xtc xtc(fullPath, "/.crosspoint");
    xtc.setupCacheDir();
    cachePath = xtc.getCachePath();
    // 5-byte XTC progress: page(4) + percent(1)
    data[4] = 100;
    dataLen = 5;
  } else if (FsHelpers::hasTxtExtension(fullPath) || FsHelpers::hasMarkdownExtension(fullPath)) {
    Txt txt(fullPath, "/.crosspoint");
    txt.setupCacheDir();
    cachePath = txt.getCachePath();
    // 7-byte TXT progress: page(2) + offset(4) + percent(1)
    data[6] = 100;
    dataLen = 7;
  } else {
    return;
  }

  FsFile f;
  if (!Storage.openFileForWrite("FBR", cachePath + "/progress.bin", f)) {
    LOG_ERR("FBR", "Failed to write progress for mark-as-read: %s", fullPath.c_str());
    return;
  }
  f.write(data, dataLen);
  f.close();
  LOG_INF("FBR", "Marked as read: %s", fullPath.c_str());

  // Series/index unknown without loading — findNextBook falls back to alphabetical order.
  const std::string nextBookPath = BookFinished::findNextBookInDirectory(fullPath, {}, {});
  startActivityForResult(std::make_unique<FinishedBookActivity>(renderer, mappedInput, fullPath, nextBookPath),
                         [this, fullPath, nextBookPath](const ActivityResult& result) {
                           if (result.isCancelled) {
                             requestUpdate();
                             return;
                           }
                           const auto& menuResult = std::get<MenuResult>(result.data);
                           if (menuResult.action == static_cast<int>(BookFinished::FinishedBookAction::GoHome)) {
                             if (SETTINGS.moveFinishedBooksToCompleted) {
                               std::string movedPath;
                               BookFinished::moveFinishedBookToCompleted(fullPath, movedPath);
                             }
                             if (SETTINGS.removeFinishedBooksFromRecents) {
                               RECENT_BOOKS.removeBook(fullPath);
                             }
                             onGoHome();
                             return;
                           }
                           if (menuResult.action == static_cast<int>(BookFinished::FinishedBookAction::OpenNextBook) &&
                               !nextBookPath.empty()) {
                             if (SETTINGS.moveFinishedBooksToCompleted) {
                               std::string movedPath;
                               BookFinished::moveFinishedBookToCompleted(fullPath, movedPath);
                             }
                             if (SETTINGS.removeFinishedBooksFromRecents) {
                               RECENT_BOOKS.removeBook(fullPath);
                             }
                             ReturnHint hint;
                             hint.target = ReturnTo::FileBrowser;
                             hint.path = basepath;
                             activityManager.replaceWithReader(nextBookPath, std::move(hint));
                             return;
                           }
                           // Stay — apply side effects then reload the list (file may have moved to /COMPLETED).
                           if (SETTINGS.moveFinishedBooksToCompleted) {
                             std::string movedPath;
                             BookFinished::moveFinishedBookToCompleted(fullPath, movedPath);
                           }
                           if (SETTINGS.removeFinishedBooksFromRecents) {
                             RECENT_BOOKS.removeBook(fullPath);
                           }
                           loadFiles();
                           if (selectorIndex >= static_cast<int>(files.size())) {
                             selectorIndex = files.empty() ? 0 : static_cast<int>(files.size()) - 1;
                           }
                           requestUpdate(true);
                         });
}

void FileBrowserActivity::doSetAsSleepCover(const std::string& fullPath) {
  if (FsHelpers::hasBmpExtension(fullPath)) {
    // BMP: use the shared helper that just does a file copy + settings update.
    const bool success = BmpViewerActivity::setBmpFileAsSleepScreen(fullPath);
    {
      RenderLock lock(*this);
      const char* msg = success ? tr(STR_SLEEP_SCREEN_SET) : tr(STR_FAILED_TO_SET_SLEEP_SCREEN);
      GUI.drawPopup(renderer, msg);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    requestUpdate();
  } else {
    // JPG/PNG: must render to framebuffer — open the image viewer so the user can use its Set Sleep button.
    ReturnHint hint;
    hint.target = ReturnTo::FileBrowser;
    hint.path = basepath;
    hint.selectName = files[selectorIndex];
    activityManager.replaceWithReader(std::string(fullPath), std::move(hint));
  }
}

void FileBrowserActivity::doDeleteCache(const std::string& fullPath, const std::string& entry) {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_CACHE) + std::string("?"), entry),
      [this, fullPath](const ActivityResult& res) {
        if (!res.isCancelled) {
          clearFileMetadata(fullPath);
          LOG_INF("FBR", "Cache deleted for: %s", fullPath.c_str());
        }
        requestUpdate();
      });
}

void FileBrowserActivity::doRemove(const std::string& fullPath, const std::string& entry, bool isDirectory) {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE) + std::string("? "), entry),
      [this, fullPath, isDirectory](const ActivityResult& res) {
        if (!res.isCancelled) {
          LOG_DBG("FBR", "Attempting to delete: %s", fullPath.c_str());
          bool deleted;
          if (isDirectory) {
            deleted = removeDirRecursive(fullPath);
          } else {
            clearFileMetadata(fullPath);
            deleted = Storage.remove(fullPath.c_str());
          }
          if (deleted) {
            LOG_DBG("FBR", "Deleted successfully");
            loadFiles();
            if (files.empty()) {
              selectorIndex = 0;
            } else if (selectorIndex >= static_cast<int>(files.size())) {
              selectorIndex = static_cast<int>(files.size()) - 1;
            }
            requestUpdate(true);
          } else {
            LOG_ERR("FBR", "Failed to delete: %s", fullPath.c_str());
            requestUpdate();
          }
        } else {
          requestUpdate();
        }
      });
}

void FileBrowserActivity::doFlashFirmware(const std::string& fullPath) {
  // Use the pre-selected-path constructor to skip the picker inside SdFirmwareUpdateActivity.
  startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput, fullPath),
                         [this](const ActivityResult&) { requestUpdate(); });
}