#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <ZipFile.h>
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

// ── CoverExtractSession ──────────────────────────────────────────────────────

ReaderActivity::CoverExtractSession::~CoverExtractSession() {
  if (buf_) {
    free(buf_);
    buf_ = nullptr;
  }
  if (dst_.isOpen()) dst_.close();
  // reader_ destructor closes entry; zip_ destructor is harmless
}

bool ReaderActivity::CoverExtractSession::begin(const std::string& epubPath, const std::string& zipEntryPath,
                                                const std::string& destPath) {
  destPath_ = destPath;
  zip_ = std::unique_ptr<ZipFile>(new ZipFile(epubPath));
  reader_ = std::unique_ptr<ZipFile::EntryReader>(new ZipFile::EntryReader(*zip_));
  if (!reader_->open(zipEntryPath.c_str())) {
    LOG_ERR("CEX", "Failed to open ZIP entry %s in %s", zipEntryPath.c_str(), epubPath.c_str());
    return false;
  }
  if (!Storage.openFileForWrite("CEX", destPath_, dst_)) {
    LOG_ERR("CEX", "Failed to open dest %s for write", destPath_.c_str());
    return false;
  }
  LOG_DBG("CEX", "Extracting %s -> %s (%zu bytes)", zipEntryPath.c_str(), destPath_.c_str(), reader_->inflatedSize());
  return true;
}

ReaderActivity::CoverExtractSession::Status ReaderActivity::CoverExtractSession::continueStep(size_t chunkBytes) {
  if (!reader_ || !reader_->isOpen()) return Status::Error;

  if (!buf_ || chunkBytes_ != chunkBytes) {
    free(buf_);
    buf_ = static_cast<uint8_t*>(malloc(chunkBytes));
    if (!buf_) {
      LOG_ERR("CEX", "OOM allocating %zu byte chunk buffer", chunkBytes);
      return Status::Error;
    }
    chunkBytes_ = chunkBytes;
  }

  size_t produced = 0;
  bool done = false;
  if (!reader_->step(buf_, chunkBytes, &produced, &done)) {
    LOG_ERR("CEX", "ZIP inflate error at %zu/%zu bytes", reader_->bytesProduced(), reader_->inflatedSize());
    dst_.close();
    Storage.remove(destPath_.c_str());
    return Status::Error;
  }
  if (produced > 0) dst_.write(buf_, produced);

  if (done) {
    dst_.close();
    LOG_DBG("CEX", "Extraction complete: %zu bytes -> %s", reader_->bytesProduced(), destPath_.c_str());
    return Status::Done;
  }
  return Status::Running;
}

size_t ReaderActivity::CoverExtractSession::bytesProduced() const { return reader_ ? reader_->bytesProduced() : 0; }

size_t ReaderActivity::CoverExtractSession::totalBytes() const { return reader_ ? reader_->inflatedSize() : 0; }

std::unique_ptr<ReaderActivity::CoverExtractSession> ReaderActivity::beginCoverExtractSession(
    const std::string& bookPath) {
  if (!FsHelpers::hasEpubExtension(bookPath)) return nullptr;
  if (!sidecarCoverPath(bookPath).empty()) return nullptr;  // sidecar takes priority; no extract needed

  Epub epub(bookPath, "/.crosspoint");
  if (!epub.load(true, true)) return nullptr;

  // If cover.img already exists and is valid, no extraction needed.
  const std::string coverImgPath = epub.getCoverImageCachePath();
  if (Storage.exists(coverImgPath.c_str())) {
    FsFile existing;
    if (Storage.openFileForRead("CEX", coverImgPath, existing)) {
      const bool valid = existing.size() > 0;
      existing.close();
      if (valid) return nullptr;  // already cached
    }
  }

  const std::string coverHref = epub.getCoverItemHref();
  if (coverHref.empty()) {
    LOG_DBG("CEX", "No cover item href for %s", bookPath.c_str());
    return nullptr;
  }

  const std::string normHref = FsHelpers::normalisePath(coverHref);
  const std::string dir = epub.getCachePath();
  if (!Storage.exists(dir.c_str())) Storage.mkdir(dir.c_str());

  auto session = std::unique_ptr<CoverExtractSession>(new CoverExtractSession());
  if (!session->begin(bookPath, normHref, coverImgPath)) return nullptr;

  return session;
}

// ── end CoverExtractSession ──────────────────────────────────────────────────

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
  if (Storage.exists(bmpPath.c_str())) {
    FsFile existing;
    const uint32_t existingSize = Storage.openFileForRead("COVER", bmpPath, existing) ? (uint32_t)existing.size() : 0;
    existing.close();
    LOG_DBG("COVER", "convertSidecarToBmp: BMP already exists path=%s size=%u", bmpPath.c_str(), existingSize);
    return bmpPath;
  }

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
    LOG_DBG("COVER", "convertSidecarToBmp: conversion FAILED sidecar=%s bmpPath=%s", sidecarPath.c_str(),
            bmpPath.c_str());
    Storage.remove(bmpPath.c_str());
    return "";
  }
  LOG_DBG("COVER", "convertSidecarToBmp: wrote %s from %s", bmpPath.c_str(), sidecarPath.c_str());
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

  // Source preference: a sidecar image beside the book wins over the embedded cover.
  const std::string sidecar = sidecarCoverPath(bookPath);
  if (!sidecar.empty()) {
    // A sidecar may have changed since the sentinel was written — clear it so the
    // sidecar conversion runs fresh.
    if (Storage.exists(file.c_str())) Storage.remove(file.c_str());
    const std::string result = convertSidecarToBmp(dir, sidecar, width, height, name);
    LOG_DBG("COVER", "convertSidecarToBmp(%dx%d) sidecar=%s result=%s", width, height, sidecar.c_str(),
            result.empty() ? "FAILED" : result.c_str());
    if (!result.empty()) return true;
  }

  // No usable sidecar — fall back to the embedded cover (deferred load: a sidecar hit never
  // pays for a full book parse).
  // Note: embedded-cover failures write a 0-byte sentinel via generateThumbBmp; we leave
  // it in place here so repeated homescreen loads don't re-attempt a known-failing decode.
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

  // Embedded single-height thumbnails scale to height*0.6 wide; mirror that for the sidecar.
  const std::string sidecar = sidecarCoverPath(bookPath);
  if (!sidecar.empty()) {
    // Sidecar may have changed — clear sentinel so conversion runs fresh.
    if (Storage.exists(file.c_str())) Storage.remove(file.c_str());
    const std::string result = convertSidecarToBmp(dir, sidecar, height * 6 / 10, height, name);
    LOG_DBG("COVER", "convertSidecarToBmp(h=%d) sidecar=%s result=%s", height, sidecar.c_str(),
            result.empty() ? "FAILED" : result.c_str());
    if (!result.empty()) return true;
  }

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

std::unique_ptr<PngDecodeSession> ReaderActivity::beginPngThumbSession(const std::string& bookPath, int width,
                                                                       int height, PngThumbFiles& filesOut) {
  const std::string dir = bookCacheDir(bookPath);
  const std::string name = "thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
  const std::string bmpPath = dir + "/" + name;

  // Already cached (valid) — caller should have checked, but be safe.
  if (thumbFileValid(bmpPath)) return nullptr;

  // Sidecar PNG takes priority over embedded cover.
  std::string srcPath;
  const std::string sidecar = sidecarCoverPath(bookPath);
  bool isSidecar = false;
  if (!sidecar.empty() && FsHelpers::hasPngExtension(sidecar)) {
    srcPath = sidecar;
    isSidecar = true;
    // Clear any stale sentinel so the write can proceed.
    if (Storage.exists(bmpPath.c_str())) Storage.remove(bmpPath.c_str());
  } else if (sidecar.empty() && FsHelpers::hasEpubExtension(bookPath)) {
    // Check if the embedded cover.img is a PNG.
    Epub epub(bookPath, "/.crosspoint");
    if (!epub.load(true, true) || !epub.ensureCoverImageCached()) return nullptr;
    srcPath = epub.getCoverImageCachePath();
    // ensureCoverImageCached validates format — peek format to confirm PNG.
    FsFile peek;
    if (!Storage.openFileForRead("PNG", srcPath, peek)) return nullptr;
    uint8_t magic[8];
    const bool isPng = peek.read(magic, 8) == 8 && magic[0] == 0x89 && magic[1] == 0x50;
    peek.close();
    if (!isPng) return nullptr;
  } else {
    return nullptr;
  }

  if (!Storage.exists(dir.c_str())) Storage.mkdir(dir.c_str());

  if (!Storage.openFileForRead("PNG", srcPath, filesOut.src)) {
    LOG_ERR("PNG", "beginPngThumbSession: failed to open src %s", srcPath.c_str());
    return nullptr;
  }
  if (!Storage.openFileForWrite("PNG", bmpPath, filesOut.dst)) {
    LOG_ERR("PNG", "beginPngThumbSession: failed to open dst %s", bmpPath.c_str());
    filesOut.src.close();
    return nullptr;
  }

  auto session = std::unique_ptr<PngDecodeSession>(new PngDecodeSession());
  if (!session->begin(filesOut.src, filesOut.dst, width, height)) {
    filesOut.src.close();
    filesOut.dst.close();
    // Leave 0-byte sentinel so we don't retry if the failure is permanent (e.g. PNG too large).
    if (!isSidecar) {
      LOG_DBG("PNG", "beginPngThumbSession: begin() failed, leaving sentinel for %s", bmpPath.c_str());
    } else {
      Storage.remove(bmpPath.c_str());
    }
    return nullptr;
  }

  LOG_DBG("PNG", "beginPngThumbSession: started sliced decode for %s -> %s (%dx%d)", srcPath.c_str(), bmpPath.c_str(),
          width, height);
  return session;
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
