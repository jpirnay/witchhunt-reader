#include "FinishedBookActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <Txt.h>
#include <Xtc.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "OpdsServerStore.h"
#include "ReaderActivity.h"
#include "ReadingSessionTracker.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "activities/home/RecentBooksActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
std::string extractFolderPath(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return "/";
  }
  return filePath.substr(0, lastSlash);
}

std::string getFilename(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos) {
    return filePath;
  }
  return filePath.substr(lastSlash + 1);
}

static constexpr int kFinishedBookCoverHeight = RecentBooksActivity::GRID_THUMB_HEIGHT;
static constexpr int kFinishedBookCoverMaxWidth = RecentBooksActivity::GRID_THUMB_WIDTH;

std::string findUniquePathWithSuffix(const std::string& basePath) {
  if (!Storage.exists(basePath.c_str())) {
    return basePath;
  }

  const auto dotPos = basePath.find_last_of('.');
  const std::string base = (dotPos == std::string::npos) ? basePath : basePath.substr(0, dotPos);
  const std::string ext = (dotPos == std::string::npos) ? std::string() : basePath.substr(dotPos);
  for (int suffix = 1; suffix < 1000; ++suffix) {
    const std::string candidate = base + " (" + std::to_string(suffix) + ")" + ext;
    if (!Storage.exists(candidate.c_str())) {
      return candidate;
    }
  }
  return {};
}

std::string findUniqueCompletedSidecarPath(const std::string& basePath) { return findUniquePathWithSuffix(basePath); }

std::string convertSidecarToBmp(const std::string& cacheDir, const std::string& sidecarPath, int width, int height,
                                const std::string& fileName) {
  if (!Storage.exists(cacheDir.c_str())) {
    Storage.mkdir(cacheDir.c_str());
  }
  const std::string bmpPath = cacheDir + "/" + fileName;
  if (Storage.exists(bmpPath.c_str())) {
    return bmpPath;
  }

  FsFile src;
  if (!Storage.openFileForRead("FIN", sidecarPath, src)) {
    return "";
  }
  FsFile dst;
  if (!Storage.openFileForWrite("FIN", bmpPath, dst)) {
    src.close();
    return "";
  }

  bool ok = false;
  if (FsHelpers::hasJpgExtension(sidecarPath)) {
    ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(src, dst, width, height);
  } else if (FsHelpers::hasPngExtension(sidecarPath)) {
    ok = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(src, dst, width, height);
  } else if (FsHelpers::hasBmpExtension(sidecarPath)) {
    uint8_t buffer[1024];
    while (src.available()) {
      size_t bytesRead = src.read(buffer, sizeof(buffer));
      dst.write(buffer, bytesRead);
    }
    ok = true;
  }

  src.close();
  dst.close();
  if (!ok) {
    Storage.remove(bmpPath.c_str());
    return "";
  }
  return bmpPath;
}

std::string getSidecarCoverBmpPath(const std::string& bookPath, int width, int height) {
  const std::string sidecarPath = ReaderActivity::sidecarCoverPath(bookPath);
  if (sidecarPath.empty()) {
    return "";
  }

  if (FsHelpers::hasBmpExtension(sidecarPath)) {
    return sidecarPath;
  }

  const std::string fileName = "thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
  return convertSidecarToBmp(ReaderActivity::bookCacheDir(bookPath), sidecarPath, width, height, fileName);
}

bool moveSidecarFilesToCompleted(const std::string& currentBookPath, const std::string& targetBookPath) {
  const auto srcDot = currentBookPath.rfind('.');
  const auto dstDot = targetBookPath.rfind('.');
  if (srcDot == std::string::npos || dstDot == std::string::npos) {
    return false;
  }

  const std::string srcBase = currentBookPath.substr(0, srcDot);
  const std::string dstBase = targetBookPath.substr(0, dstDot);
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};
  bool success = true;
  for (const char* ext : extensions) {
    const std::string srcSidecar = srcBase + ext;
    if (!Storage.exists(srcSidecar.c_str())) {
      continue;
    }

    std::string dstSidecar = dstBase + ext;
    if (Storage.exists(dstSidecar.c_str())) {
      dstSidecar = findUniqueCompletedSidecarPath(dstSidecar);
      if (dstSidecar.empty()) {
        LOG_ERR("FIN", "Failed to create unique sidecar target for %s", srcSidecar.c_str());
        success = false;
        continue;
      }
    }

    if (!Storage.rename(srcSidecar.c_str(), dstSidecar.c_str())) {
      LOG_ERR("FIN", "Failed to move sidecar %s -> %s", srcSidecar.c_str(), dstSidecar.c_str());
      success = false;
    }
  }
  return success;
}

struct NextBookMetadata {
  std::string title;
  std::string author;
  std::string series;
  std::string coverPath;
};

NextBookMetadata loadNextBookMetadata(const std::string& nextBookPath) {
  NextBookMetadata metadata;
  if (nextBookPath.empty()) {
    return metadata;
  }

  if (FsHelpers::hasEpubExtension(nextBookPath)) {
    Epub epub(nextBookPath, "/.crosspoint");
    epub.setSyntheticTocFallbackEnabled(SETTINGS.syntheticTocFallback != 0);
    // loadForCover(), not load(): this preview needs metadata + the cover thumb, never the spine/TOC.
    // A full load() here rebuilt book.bin and reparsed the CSS for a book the user may not even open,
    // costing ~2 s inside the reader — the same waste the sequel scan used to pay per candidate.
    if (epub.loadForCover()) {
      metadata.title = epub.getTitle();
      metadata.author = epub.getAuthor();
      metadata.series = epub.getSeries();
      if (!metadata.series.empty() && !epub.getSeriesIndex().empty()) {
        metadata.series += " #" + epub.getSeriesIndex();
      }
      if (epub.generateThumbBmp(kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight) == ThumbResult::Ok) {
        metadata.coverPath = epub.getThumbBmpPath(kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight);
      } else {
        metadata.coverPath = getSidecarCoverBmpPath(nextBookPath, kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight);
      }
    }
    return metadata;
  }

  if (FsHelpers::hasXtcExtension(nextBookPath)) {
    Xtc xtc(nextBookPath, "/.crosspoint");
    if (xtc.load()) {
      metadata.title = xtc.getTitle();
      metadata.author = xtc.getAuthor();
      if (xtc.generateThumbBmp(kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight)) {
        metadata.coverPath = xtc.getThumbBmpPath(kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight);
      } else {
        metadata.coverPath = getSidecarCoverBmpPath(nextBookPath, kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight);
      }
    }
    return metadata;
  }

  if (FsHelpers::hasMarkdownExtension(nextBookPath) || FsHelpers::hasTxtExtension(nextBookPath)) {
    Txt txt(nextBookPath, "/.crosspoint");
    if (txt.load()) {
      metadata.title = txt.getTitle();
      if (txt.generateCoverBmp()) {
        metadata.coverPath = txt.getCoverBmpPath();
      } else {
        metadata.coverPath = getSidecarCoverBmpPath(nextBookPath, kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight);
      }
    }
    return metadata;
  }

  return metadata;
}

bool isSupportedBookFile(const std::string& fileName) {
  return FsHelpers::hasEpubExtension(fileName) || FsHelpers::hasXtcExtension(fileName) ||
         FsHelpers::hasTxtExtension(fileName) || FsHelpers::hasMarkdownExtension(fileName);
}

bool caseInsensitiveEqual(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  for (size_t i = 0; i < left.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) {
      return false;
    }
  }
  return true;
}

bool naturalLess(const std::string& str1, const std::string& str2) {
  const char* s1 = str1.c_str();
  const char* s2 = str2.c_str();
  while (*s1 && *s2) {
    if (std::isdigit(static_cast<unsigned char>(*s1)) && std::isdigit(static_cast<unsigned char>(*s2))) {
      while (*s1 == '0') s1++;
      while (*s2 == '0') s2++;
      int len1 = 0;
      int len2 = 0;
      while (std::isdigit(static_cast<unsigned char>(s1[len1]))) len1++;
      while (std::isdigit(static_cast<unsigned char>(s2[len2]))) len2++;
      if (len1 != len2) return len1 < len2;
      for (int i = 0; i < len1; ++i) {
        if (s1[i] != s2[i]) return s1[i] < s2[i];
      }
      s1 += len1;
      s2 += len2;
    } else {
      const char c1 = std::tolower(static_cast<unsigned char>(*s1));
      const char c2 = std::tolower(static_cast<unsigned char>(*s2));
      if (c1 != c2) return c1 < c2;
      s1++;
      s2++;
    }
  }
  return *s1 == '\0' && *s2 != '\0';
}

std::optional<float> parseSeriesIndex(const std::string& rawIndex) {
  const char* start = rawIndex.c_str();
  char* end = nullptr;
  errno = 0;
  const float value = std::strtof(start, &end);
  if (end == start || errno == ERANGE) {
    return std::nullopt;
  }
  return value;
}

std::string pathWithFilename(const std::string& directory, const std::string& fileName) {
  if (directory.empty() || directory == "/") {
    return std::string("/") + fileName;
  }
  return directory + "/" + fileName;
}

// Finds the next book in the series: the candidate whose series index is the SMALLEST one still
// greater than the current book's.
//
// Streams the directory and keeps only the running best — one path string of state, no file list, no
// sort. That matters more than it looks: this runs inside the reader with the secondary framebuffer
// restored (~10.7 KB largest contiguous block, measured on X4), so materialising and sorting a vector
// of every filename was the single largest allocation on the path. Streaming makes peak memory O(1)
// in the folder size, which removed the need for a heap gate and a listing cap.
//
// Exhaustive over the folder, deliberately and without a candidate cap. Two earlier revisions tried
// to bound the work and both were silently WRONG rather than merely incomplete:
//   - skipping everything before the current file in sorted order breaks whenever filenames disagree
//     with series numbering;
//   - stopping after the first N entries breaks too, because directory order is filesystem order, so
//     "the first N" is arbitrary — in a 30-book folder the real sequel can sit at position 25 and
//     never be examined. That fails precisely on the large series folders this feature is for.
// Exhaustive is affordable because each candidate is a metadata-only parse (~300 ms) rather than the
// full load (~2 s) this used to do — see Epub::loadForMetadata(). A very large folder still costs
// real time; the answer to that is moving the scan off the main loop, not truncating it into a wrong
// answer. The watchdog feed below keeps a long scan from rebooting the device.
std::string findSeriesSequel(const std::string& directory, const std::string& currentFilename,
                             const std::string& currentSeries, const std::string& currentSeriesIndex) {
  const auto currentIndex = parseSeriesIndex(currentSeriesIndex);
  if (!currentIndex.has_value() || currentSeries.empty()) {
    return {};
  }

  auto root = Storage.open(directory.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return {};
  }

  std::string bestPath;
  float bestIndex = std::numeric_limits<float>::infinity();
  size_t examined = 0;

  root.rewindDirectory();
  char name[512];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    const bool isDir = file.isDirectory();
    file.close();
    if (isDir) continue;

    const std::string fileName = name;
    if (fileName.empty() || fileName[0] == '.' || !FsHelpers::hasEpubExtension(fileName)) {
      continue;
    }
    // The current book is in this directory too; skip it rather than spending a parse on a book that
    // can never be its own sequel.
    if (fileName == currentFilename) {
      continue;
    }

    ++examined;

    // The scan runs synchronously in the main loop, so it blocks input and the idle task throughout,
    // and it is unbounded in the folder size. Each candidate is a ZIP open + OPF parse, so a big
    // folder is many seconds — feed the watchdog and let other tasks run between candidates. The
    // pre-fix scan full-loaded every EPUB (~2 s each) with no yield at all, the suspected cause of
    // the issue #104 reboot.
    esp_task_wdt_reset();
    yield();

    const std::string candidatePath = pathWithFilename(directory, fileName);
    Epub epub(candidatePath, "/.crosspoint");
    epub.setSyntheticTocFallbackEnabled(SETTINGS.syntheticTocFallback != 0);
    // Metadata-only: no spine/TOC book.bin build, no manifest index, no CSS parse.
    if (!epub.loadForMetadata()) {
      continue;
    }
    if (!caseInsensitiveEqual(epub.getSeries(), currentSeries)) {
      continue;
    }
    const auto candidateIndex = parseSeriesIndex(epub.getSeriesIndex());
    if (!candidateIndex.has_value() || candidateIndex.value() <= currentIndex.value()) {
      continue;
    }
    if (candidateIndex.value() < bestIndex) {
      bestIndex = candidateIndex.value();
      bestPath = candidatePath;
    }
  }
  root.close();

  LOG_DBG("FIN", "Series-sequel scan: examined %zu candidate(s), next=%s", examined,
          bestPath.empty() ? "(none)" : bestPath.c_str());
  return bestPath;
}

// Finds the book immediately after currentFilename in natural filename order — i.e. the SMALLEST
// filename still greater than the current one.
//
// Like findSeriesSequel, this streams the directory keeping only the running best rather than listing
// and sorting every entry, so peak memory is two short strings regardless of folder size. Opens no
// files at all: filenames alone decide the answer.
std::string findNextAlphabeticalBook(const std::string& directory, const std::string& currentFilename) {
  auto root = Storage.open(directory.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return {};
  }

  std::string best;
  root.rewindDirectory();
  char name[512];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    const bool isDir = file.isDirectory();
    file.close();
    if (isDir) continue;

    const std::string fileName = name;
    if (fileName.empty() || fileName[0] == '.' || !isSupportedBookFile(fileName)) {
      continue;
    }
    // Strictly after the current book, and the earliest such name seen so far.
    if (!naturalLess(currentFilename, fileName)) {
      continue;
    }
    if (best.empty() || naturalLess(fileName, best)) {
      best = fileName;
    }
  }
  root.close();

  if (best.empty()) return {};
  return pathWithFilename(directory, best);
}

bool pathIsInCompleted(const std::string& bookPath) {
  return bookPath.rfind("/COMPLETED/", 0) == 0 || bookPath == "/COMPLETED";
}

std::string buildCompletedTargetPath(const std::string& currentBookPath) {
  const std::string fileName = getFilename(currentBookPath);
  return std::string("/COMPLETED/") + fileName;
}

std::string findUniqueCompletedPath(const std::string& basePath) { return findUniquePathWithSuffix(basePath); }
}  // namespace

namespace BookFinished {

std::string findNextBookInDirectory(const std::string& currentBookPath, const std::string& currentBookSeries,
                                    const std::string& currentBookSeriesIndex) {
  const std::string directory = extractFolderPath(currentBookPath);
  const std::string currentFilename = getFilename(currentBookPath);

  if (!currentBookSeries.empty() && !currentBookSeriesIndex.empty()) {
    const std::string sequel = findSeriesSequel(directory, currentFilename, currentBookSeries, currentBookSeriesIndex);
    if (!sequel.empty() && sequel != currentBookPath) {
      return sequel;
    }
  }

  return findNextAlphabeticalBook(directory, currentFilename);
}

bool moveFinishedBookToCompleted(const std::string& currentBookPath, std::string& outMovedPath) {
  if (pathIsInCompleted(currentBookPath)) {
    outMovedPath = currentBookPath;
    return true;
  }

  const std::string completedDir = "/COMPLETED";
  if (!Storage.exists(completedDir.c_str()) && !Storage.mkdir(completedDir.c_str())) {
    LOG_ERR("FIN", "Failed to create /COMPLETED directory");
    return false;
  }

  std::string targetPath = buildCompletedTargetPath(currentBookPath);
  if (Storage.exists(targetPath.c_str())) {
    targetPath = findUniqueCompletedPath(targetPath);
    if (targetPath.empty()) {
      LOG_ERR("FIN", "Cannot resolve unique /COMPLETED filename");
      return false;
    }
  }

  if (!Storage.rename(currentBookPath.c_str(), targetPath.c_str())) {
    LOG_ERR("FIN", "Failed to move book to /COMPLETED: %s -> %s", currentBookPath.c_str(), targetPath.c_str());
    return false;
  }

  if (!moveSidecarFilesToCompleted(currentBookPath, targetPath)) {
    LOG_ERR("FIN", "One or more sidecar files failed to move for %s", currentBookPath.c_str());
  }

  outMovedPath = targetPath;
  return true;
}

void launchFinishedBookFlow(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::string& bookPath, const std::string& series, const std::string& seriesIndex,
                            const std::string& author, void (*onMenuClosed)(void*), void* onMenuClosedCtx) {
  const std::string nextBookPath = findNextBookInDirectory(bookPath, series, seriesIndex);
  Activity* hostPtr = &host;
  host.startActivityForResult(
      std::make_unique<FinishedBookActivity>(renderer, mappedInput, bookPath, nextBookPath, author),
      [hostPtr, bookPath, nextBookPath, author, onMenuClosed, onMenuClosedCtx](const ActivityResult& result) {
        if (onMenuClosed) {
          onMenuClosed(onMenuClosedCtx);
        }
        if (result.isCancelled) {
          hostPtr->requestUpdate();
          return;
        }
        // User confirmed they're done with this book — credit a finish to the
        // in-flight session before any tear-down side effects.
        globalReadingSessionTracker().markFinished();
        const auto& menuResult = std::get<MenuResult>(result.data);
        const bool goHome = menuResult.action == static_cast<int>(FinishedBookAction::GoHome);
        const bool openNext =
            menuResult.action == static_cast<int>(FinishedBookAction::OpenNextBook) && !nextBookPath.empty();
        const bool searchOpds =
            menuResult.action == static_cast<int>(FinishedBookAction::SearchOpdsForAuthor) && !author.empty();
        if (!goHome && !openNext && !searchOpds) {
          hostPtr->requestUpdate();
          return;
        }
        if (SETTINGS.moveFinishedBooksToCompleted) {
          std::string movedPath;
          moveFinishedBookToCompleted(bookPath, movedPath);
        }
        if (SETTINGS.removeFinishedBooksFromRecents) {
          RECENT_BOOKS.removeBook(bookPath);
        }
        if (goHome) {
          activityManager.goHome();
        } else if (openNext) {
          activityManager.goToReader(nextBookPath);
        } else {
          activityManager.goToBrowserWithSearch(author);
        }
      });
}

}  // namespace BookFinished

FinishedBookActivity::FinishedBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::string currentBookPath, std::string nextBookPath,
                                           std::string currentBookAuthor)
    : Activity("FinishedBook", renderer, mappedInput),
      currentBookPath_(std::move(currentBookPath)),
      nextBookPath_(std::move(nextBookPath)),
      currentBookAuthor_(std::move(currentBookAuthor)),
      nextBookAvailable_(!nextBookPath_.empty()) {
  nextBookName_ = nextBookAvailable_ ? getFilename(nextBookPath_) : tr(STR_NOT_SET);
}

FinishedBookActivity::RowModel FinishedBookActivity::buildRowModel() const {
  RowModel model;

  model.actions.push_back(RowModel::Action::GoHome);
  model.titles.push_back(tr(STR_GO_BACK_TO_HOME));
  model.subtitles.push_back(tr(STR_GO_BACK_TO_HOME_DESC));
  model.values.push_back(tr(STR_HOME));

  if (nextBookAvailable_) {
    // Subtitle prefers the loaded metadata; before loop() has loaded it, fall back to the filename so
    // the row is never blank.
    std::string subtitle = nextBookAuthor_;
    if (!nextBookSeries_.empty()) {
      if (!subtitle.empty()) subtitle += " • ";
      subtitle += nextBookSeries_;
    }
    if (subtitle.empty()) {
      subtitle = nextBookName_;
    }
    model.actions.push_back(RowModel::Action::OpenNext);
    model.titles.push_back(nextBookTitle_.empty() ? tr(STR_OPEN_NEXT_BOOK) : nextBookTitle_);
    model.subtitles.push_back(subtitle);
    model.values.push_back(tr(STR_OPEN));
  }

  if (!currentBookAuthor_.empty() && OPDS_STORE.hasServers()) {
    model.actions.push_back(RowModel::Action::SearchOpds);
    model.titles.push_back(tr(STR_SEARCH_OPDS_FOR_AUTHOR));
    model.subtitles.push_back(currentBookAuthor_);
    model.values.push_back(tr(STR_SEARCH));
  }

  if (!pathIsInCompleted(currentBookPath_)) {
    model.actions.push_back(RowModel::Action::ToggleMoveToCompleted);
    model.titles.push_back(tr(STR_MOVE_FINISHED_TO_COMPLETED));
    model.subtitles.push_back("");
    model.values.push_back(moveFinishedBooksToCompleted_ ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
  }

  model.actions.push_back(RowModel::Action::ToggleForget);
  model.titles.push_back(tr(STR_FORGET_BOOK));
  model.subtitles.push_back(tr(STR_FORGET_BOOK_DESC));
  model.values.push_back(removeFinishedBooksFromRecents_ ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));

  return model;
}

void FinishedBookActivity::onEnter() {
  Activity::onEnter();
  moveFinishedBooksToCompleted_ = SETTINGS.moveFinishedBooksToCompleted;
  removeFinishedBooksFromRecents_ = SETTINGS.removeFinishedBooksFromRecents;
  selectedIndex_ = std::clamp(selectedIndex_, 0, std::max(0, buildRowModel().count() - 1));

  if (nextBookAvailable_) {
    nextBookTitle_ = nextBookName_;
    nextBookAuthor_.clear();
    nextBookSeries_.clear();
    nextBookCoverPath_.clear();
    nextBookMetadataLoaded_ = false;
  } else {
    nextBookTitle_.clear();
    nextBookAuthor_.clear();
    nextBookSeries_.clear();
    nextBookCoverPath_.clear();
    nextBookMetadataLoaded_ = true;
  }
  requestUpdate();
}

void FinishedBookActivity::loop() {
  // Built ONCE per loop() rather than per event: the model depends only on member state, and
  // constructing it allocates five rows of translated strings. Rebuilding it inside the event loop
  // put that cost in front of every button press. Any handler below that changes what the rows say
  // (the two toggles) returns immediately, so a single build stays consistent with the events it
  // dispatches.
  const RowModel model = buildRowModel();
  const int optionCount = model.count();
  if (optionCount <= 0) {
    return;
  }

  selectedIndex_ = std::clamp(selectedIndex_, 0, optionCount - 1);

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.type != ButtonEventManager::PressType::Short) {
      continue;
    }

    const auto finishWith = [this](const BookFinished::FinishedBookAction action) {
      MenuResult menuResult;
      menuResult.action = static_cast<int>(action);
      ActivityResult result(menuResult);
      setResult(std::move(result));
      finish();
    };

    if (ev.button == MappedInputManager::Button::Back) {
      finishWith(BookFinished::FinishedBookAction::GoHome);
      return;
    }

    if (ev.button == MappedInputManager::Button::Confirm) {
      switch (model.actions[selectedIndex_]) {
        case RowModel::Action::GoHome:
          finishWith(BookFinished::FinishedBookAction::GoHome);
          return;
        case RowModel::Action::OpenNext:
          finishWith(BookFinished::FinishedBookAction::OpenNextBook);
          return;
        case RowModel::Action::SearchOpds:
          finishWith(BookFinished::FinishedBookAction::SearchOpdsForAuthor);
          return;
        case RowModel::Action::ToggleMoveToCompleted:
          moveFinishedBooksToCompleted_ = !moveFinishedBooksToCompleted_;
          SETTINGS.moveFinishedBooksToCompleted = moveFinishedBooksToCompleted_;
          SETTINGS.saveToFile();
          requestUpdate();
          return;
        case RowModel::Action::ToggleForget:
          removeFinishedBooksFromRecents_ = !removeFinishedBooksFromRecents_;
          SETTINGS.removeFinishedBooksFromRecents = removeFinishedBooksFromRecents_;
          SETTINGS.saveToFile();
          requestUpdate();
          return;
      }
      return;
    }
  }

  // Selection movement via the navigator, not consumed events — see the buttonNavigator comment in
  // the header for why Up/Down/Left/Right never arrive as events on this screen.
  buttonNavigator.onNextList(selectedIndex_, optionCount, [this] { requestUpdate(); });
  buttonNavigator.onPreviousList(selectedIndex_, optionCount, [this] { requestUpdate(); });

  if (nextBookAvailable_ && !nextBookMetadataLoaded_) {
    const auto metadata = loadNextBookMetadata(nextBookPath_);
    nextBookTitle_ = metadata.title.empty() ? getFilename(nextBookPath_) : metadata.title;
    nextBookAuthor_ = metadata.author;
    nextBookSeries_ = metadata.series;
    nextBookCoverPath_ = metadata.coverPath;
    nextBookMetadataLoaded_ = true;
    requestUpdate();
  }
}

void FinishedBookActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  const int contentTop = contentRect.y;
  const int contentBottom = contentRect.y + contentRect.height - metrics.buttonHintsHeight;
  const int contentWidth = contentRect.width - 2 * metrics.contentSidePadding;
  int y = contentTop;
  int yGap = renderer.getLineHeight(UI_10_FONT_ID);
  GUI.drawHeader(renderer, Rect{contentRect.x, contentTop, contentRect.width, metrics.headerHeight},
                 tr(STR_FINISHED_BOOK_HEADER), "");
  y += metrics.headerHeight + metrics.verticalSpacing;
  renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, y, tr(STR_FINISHED_BOOK_HEADER_LINE1),
                    true, EpdFontFamily::REGULAR);
  y += yGap + 4;
  renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, y, tr(STR_FINISHED_BOOK_HEADER_LINE2),
                    true, EpdFontFamily::REGULAR);
  y += yGap + 4;

  if (nextBookAvailable_) {
    renderer.drawText(UI_12_FONT_ID, contentRect.x + metrics.contentSidePadding, y, tr(STR_NEXT_BOOK_HEADER), true,
                      EpdFontFamily::BOLD);
    y += yGap + metrics.verticalSpacing;
  }

  const int previewHeight =
      nextBookAvailable_ ? std::max(0, std::min(kFinishedBookCoverHeight,
                                                contentBottom - y - 3 * (renderer.getLineHeight(UI_12_FONT_ID) + 8) -
                                                    metrics.verticalSpacing))
                         : 0;
  const int previewWidth = nextBookAvailable_ ? std::min(contentWidth / 2, kFinishedBookCoverMaxWidth) : 0;
  const int previewX = contentRect.x + metrics.contentSidePadding;
  const int previewY = y;

  if (nextBookAvailable_) {
    int actualCoverWidth = 0;
    int previewTextX = previewX;

    if (!nextBookCoverPath_.empty()) {
      const std::string coverPath =
          UITheme::getCoverThumbPath(nextBookCoverPath_, kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight);
      HalFile coverFile = Storage.open(coverPath.c_str());
      if (coverFile) {
        Bitmap bmp(coverFile);
        if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
          int actualCoverHeight = previewHeight;
          actualCoverWidth = bmp.getWidth() * actualCoverHeight / bmp.getHeight();
          if (actualCoverWidth > previewWidth) {
            actualCoverWidth = previewWidth;
            actualCoverHeight = bmp.getHeight() * actualCoverWidth / bmp.getWidth();
          }
          renderer.drawBitmap(bmp, previewX, previewY, actualCoverWidth, actualCoverHeight);
        }
        coverFile.close();
      }
    }

    if (actualCoverWidth > 0) {
      previewTextX = previewX + actualCoverWidth + metrics.contentSidePadding;
    }

    const int previewTextWidth = contentRect.x + contentRect.width - metrics.contentSidePadding - previewTextX;
    int infoY = previewY;
    const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, nextBookTitle_.c_str(), previewTextWidth, 3);
    const auto authorLines = renderer.wrappedText(UI_10_FONT_ID, nextBookAuthor_.c_str(), previewTextWidth, 3);
    const auto seriesLines = renderer.wrappedText(UI_10_FONT_ID, nextBookSeries_.c_str(), previewTextWidth, 2);

    if (!nextBookTitle_.empty()) {
      for (const auto& line : titleLines) {
        renderer.drawText(UI_12_FONT_ID, previewTextX, infoY, line.c_str(), true, EpdFontFamily::BOLD);
        infoY += renderer.getLineHeight(UI_12_FONT_ID);
      }
      infoY += 4;
    }
    if (!nextBookAuthor_.empty()) {
      for (const auto& line : authorLines) {
        renderer.drawText(UI_10_FONT_ID, previewTextX, infoY, line.c_str(), true);
        infoY += renderer.getLineHeight(UI_10_FONT_ID);
      }
      infoY += 4;
    }
    if (!nextBookSeries_.empty()) {
      for (const auto& line : seriesLines) {
        renderer.drawText(UI_10_FONT_ID, previewTextX, infoY, line.c_str(), true);
        infoY += renderer.getLineHeight(UI_10_FONT_ID);
      }
    }
    y = std::max(y + previewHeight, infoY) + metrics.verticalSpacing;
  }

  const RowModel model = buildRowModel();
  const int selected = std::clamp(selectedIndex_, 0, std::max(0, model.count() - 1));

  const Rect listRect{contentRect.x, y, contentRect.width, contentBottom - y};
  GUI.drawList(
      renderer, listRect, model.count(), selected, [&model](int index) { return model.titles[index]; },
      [&model](int index) { return model.subtitles[index]; }, nullptr,
      [&model](int index) { return model.values[index]; }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
