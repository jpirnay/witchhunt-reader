#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "KOReaderCredentialStore.h"
#include "MdReaderActivity.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

#ifndef DEBUG_MEMORY_CONSUMPTION
#define DEBUG_MEMORY_CONSUMPTION 0
#endif

namespace {
#if DEBUG_MEMORY_CONSUMPTION
void logReaderLaunchMemSnapshot(const char* stage) {
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  LOG_DBG("READER", "Reader mem[%s]: free=%lu contig=%lu", stage, freeHeap, contigHeap);
}
#else
inline void logReaderLaunchMemSnapshot(const char*) {}
#endif
}  // namespace

std::string ReaderActivity::extractFolderPath(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return "/";
  }
  return filePath.substr(0, lastSlash);
}

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) { return FsHelpers::hasTxtExtension(path); }

bool ReaderActivity::isMdFile(const std::string& path) { return FsHelpers::hasMarkdownExtension(path); }

bool ReaderActivity::isImageFile(const std::string& path) {
  return FsHelpers::hasBmpExtension(path) || FsHelpers::hasJpgExtension(path) || FsHelpers::hasPngExtension(path);
}

std::string ReaderActivity::sidecarCoverPath(const std::string& bookPath) {
  const auto sep = bookPath.find_last_of("/\\");
  const auto dot = bookPath.rfind('.');
  if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) return "";
  const std::string base = bookPath.substr(0, dot);
  for (const char* ext : {".jpg", ".jpeg", ".png", ".bmp", ".JPG", ".JPEG", ".PNG", ".BMP"}) {
    const std::string candidate = base + ext;
    if (Storage.exists(candidate.c_str())) {
      LOG_DBG("SIDECAR", "Found sidecar cover: %s", candidate.c_str());
      return candidate;
    }
  }
  return "";
}

std::string ReaderActivity::bookCacheDir(const std::string& bookPath) {
  if (FsHelpers::hasEpubExtension(bookPath)) return Epub(bookPath, "/.crosspoint").getCachePath();
  if (FsHelpers::hasXtcExtension(bookPath)) return Xtc(bookPath, "/.crosspoint").getCachePath();
  return Txt(bookPath, "/.crosspoint").getCachePath();
}

std::string ReaderActivity::convertSidecarToBmp(const std::string& cacheDir, const std::string& sidecarPath, int width,
                                                int height, const std::string& fileName) {
  if (!Storage.exists(cacheDir.c_str())) Storage.mkdir(cacheDir.c_str());
  const std::string bmpPath = cacheDir + "/" + fileName;
  if (Storage.exists(bmpPath.c_str())) return bmpPath;

  FsFile src;
  if (!Storage.openFileForRead("COVER", sidecarPath, src)) return "";
  FsFile dst;
  if (!Storage.openFileForWrite("COVER", bmpPath, dst)) {
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
    while (src.available()) dst.write(buffer, src.read(buffer, sizeof(buffer)));
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

std::string ReaderActivity::coverThumbPlaceholder(const std::string& bookPath) {
  return bookCacheDir(bookPath) + "/thumb_[HEIGHT].bmp";
}

namespace {
// True if a thumbnail file exists and is non-empty (a 0-byte sentinel left by a prior failed
// extraction must be treated as missing so regeneration retries).
bool thumbFileValid(const std::string& path) {
  FsFile f;
  const bool ok = Storage.openFileForRead("COVER", path, f) && f.size() > 0;
  f.close();
  return ok;
}
}  // namespace

bool ReaderActivity::ensureCoverThumb(const std::string& bookPath, int width, int height) {
  const std::string dir = bookCacheDir(bookPath);
  const std::string name = "thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
  const std::string file = dir + "/" + name;
  if (thumbFileValid(file)) return true;
  if (Storage.exists(file.c_str())) Storage.remove(file.c_str());  // clear sentinel before retry

  // Source preference: a sidecar image beside the book wins over the embedded cover.
  const std::string sidecar = sidecarCoverPath(bookPath);
  if (!sidecar.empty() && !convertSidecarToBmp(dir, sidecar, width, height, name).empty()) return true;

  // No usable sidecar — fall back to the embedded cover (deferred load: a sidecar hit never
  // pays for a full book parse).
  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    return epub.load(true, true) && epub.generateThumbBmp(width, height);
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    return xtc.load() && xtc.generateThumbBmp(width, height);
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.crosspoint");
    return txt.generateThumbBmp(width, height);
  }
  return false;
}

bool ReaderActivity::ensureCoverThumb(const std::string& bookPath, int height) {
  const std::string dir = bookCacheDir(bookPath);
  const std::string name = "thumb_" + std::to_string(height) + ".bmp";
  const std::string file = dir + "/" + name;
  if (thumbFileValid(file)) return true;
  if (Storage.exists(file.c_str())) Storage.remove(file.c_str());  // clear sentinel before retry

  // Embedded single-height thumbnails scale to height*0.6 wide; mirror that for the sidecar.
  const std::string sidecar = sidecarCoverPath(bookPath);
  if (!sidecar.empty() && !convertSidecarToBmp(dir, sidecar, height * 6 / 10, height, name).empty()) return true;

  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    return epub.load(true, true) && epub.generateThumbBmp(height);
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    return xtc.load() && xtc.generateThumbBmp(height);
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.crosspoint");
    return txt.generateThumbBmp(height);
  }
  return false;
}

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto epub = std::unique_ptr<Epub>(new Epub(path, "/.crosspoint"));
  epub->setSyntheticTocFallbackEnabled(SETTINGS.syntheticTocFallback != 0);
  if (epub->load(true, SETTINGS.embeddedStyle == 0)) {
    return epub;
  }

  if (const std::string sidecar = sidecarCoverPath(path); !sidecar.empty()) {
    LOG_INF("READER", "EPUB load failed but sidecar cover exists: %s", sidecar.c_str());
  }

  LOG_ERR("READER", "Failed to load epub");
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = std::unique_ptr<Xtc>(new Xtc(path, "/.crosspoint"));
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = std::unique_ptr<Txt>(new Txt(path, "/.crosspoint"));
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
  return nullptr;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;

  // Long-press Confirm on RecentBooks/FileBrowser sets autoPullEpubPath so the user can
  // ask for KOReader sync at open time. Route into the sync activity instead of creating the
  // reader; sync's resumeReader() will create the reader once the remote position is applied.
  // Pull-only mode does not need accurate local reader state, so we hand off zeros for spine/page.
  auto& sync = APP_STATE.koReaderSyncSession;
  if (!sync.autoPullEpubPath.empty() && sync.autoPullEpubPath == epubPath && KOREADER_STORE.hasCredentials()) {
    LOG_DBG("READER", "AUTO_PULL on open: %s", epubPath.c_str());
    sync.autoPullEpubPath.clear();  // consume the flag
    sync.active = true;
    sync.epubPath = epubPath;
    sync.spineIndex = 0;
    sync.page = 0;
    sync.totalPagesInSpine = 0;
    sync.paragraphIndex = 0;
    sync.hasParagraphIndex = false;
    sync.xhtmlSeekHint = 0;
    sync.intent = KOReaderSyncIntentState::AUTO_PULL;
    sync.outcome = KOReaderSyncOutcomeState::PENDING;
    sync.resultSpineIndex = 0;
    sync.resultPage = 0;
    sync.resultParagraphIndex = 0;
    sync.resultHasParagraphIndex = false;
    sync.resultListItemIndex = 0;
    sync.resultHasListItemIndex = false;
    sync.exitToHomeAfterSync = false;
    APP_STATE.saveToFile();
    // Drop the loaded Epub before TLS — sync activity will reload it for remote-position
    // mapping. Holding it here would needlessly inflate the heap during WiFi/TLS work.
    epub.reset();
    activityManager.goToKOReaderSync();
    return;
  }

  logReaderLaunchMemSnapshot("before_replace_epub_reader");
  activityManager.replaceActivity(std::make_unique<EpubReaderActivity>(renderer, mappedInput, std::move(epub)));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  activityManager.replaceActivity(std::make_unique<XtcReaderActivity>(renderer, mappedInput, std::move(xtc)));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.replaceActivity(std::make_unique<TxtReaderActivity>(renderer, mappedInput, std::move(txt)));
}

void ReaderActivity::onGoToMdReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.replaceActivity(std::make_unique<MdReaderActivity>(renderer, mappedInput, std::move(txt)));
}

void ReaderActivity::onEnter() {
  Activity::onEnter();
  logReaderLaunchMemSnapshot("onEnter_begin");

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  if (APP_STATE.koReaderSyncSession.active && APP_STATE.koReaderSyncSession.epubPath == initialBookPath) {
    LOG_DBG("READER", "Opening EPUB with pending KOReader sync outcome=%d",
            static_cast<int>(APP_STATE.koReaderSyncSession.outcome));
  }

  currentBookPath = initialBookPath;
  if (isImageFile(initialBookPath)) {
    onGoToBmpViewer(initialBookPath);
  } else if (isXtcFile(initialBookPath)) {
    {
      RenderLock lock(*this);
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_LOADING), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
    }

    auto xtc = loadXtc(initialBookPath);
    if (!xtc) {
      onGoBack();
      return;
    }
    onGoToXtcReader(std::move(xtc));
  } else if (isMdFile(initialBookPath)) {
    auto txt = loadTxt(initialBookPath);
    if (!txt) {
      onGoBack();
      return;
    }
    onGoToMdReader(std::move(txt));
  } else if (isTxtFile(initialBookPath)) {
    auto txt = loadTxt(initialBookPath);
    if (!txt) {
      onGoBack();
      return;
    }
    onGoToTxtReader(std::move(txt));
  } else {
    // The first open of a book runs a multi-second index build inside load()
    // (spine/TOC, content.opf, and the CSS compile). Show a popup so the wait
    // isn't a dead screen; skip it on a warm cache so cached re-opens don't add
    // an extra e-ink flash. The throwaway Epub only computes paths + does a few
    // file-existence checks (no parsing), so this is cheap.
    if (Epub(initialBookPath, "/.crosspoint").needsFirstOpenIndexing()) {
      RenderLock lock;
      // drawPopup() overlays the indexing box on the write buffer and ships it
      // with a differential refresh. displayBuffer() swaps buffers, so the write
      // buffer holds the frame from two refreshes ago — and the screen we were
      // launched from (e.g. the recent-books grid) uses a partial repaint, so
      // that stale frame shows the previous selection. Without this resync the
      // diff toggles the old/new selection cells back, making the highlight
      // visibly jump before the popup appears. Sync to the displayed frame so
      // the popup overlays what's actually on screen.
      renderer.syncWriteBufferFromDisplayed();
      GUI.drawPopup(renderer, tr(STR_INDEXING));
    }
    auto epub = loadEpub(initialBookPath);
    if (!epub) {
      onGoBack();
      return;
    }
    onGoToEpubReader(std::move(epub));
  }
}

void ReaderActivity::onGoBack() { finish(); }
