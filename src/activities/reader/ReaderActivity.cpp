#include "ReaderActivity.h"

#include <Bitmap.h>
#include <CooperativeAbort.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <LongTaskProgress.h>
#include <PngToBmpConverter.h>
#include <SidecarFiles.h>
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
#include "util/WakeTrace.h"

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
    // Halve the request rather than abandon the cover. Device-observed on X4: a 16 KB chunk
    // failed with ~74 KB free — plenty of heap, just not that much of it contiguous after a
    // carousel's worth of cover work — and the book silently lost its thumbnail for the whole
    // session. A smaller chunk only costs more inflate steps, which are sliced across ticks
    // anyway, so degrading beats failing. 1 KB is still 400+ SD writes for a 450 KB cover, which
    // is slow but finishes; below that the extract is not worth starting.
    constexpr size_t MIN_CHUNK_BYTES = 1024;
    free(buf_);
    buf_ = nullptr;
    size_t want = chunkBytes;
    while (!buf_ && want >= MIN_CHUNK_BYTES) {
      buf_ = static_cast<uint8_t*>(malloc(want));
      if (!buf_) want /= 2;
    }
    if (!buf_) {
      LOG_ERR("CEX", "OOM allocating chunk buffer (wanted %zu, gave up below %zu)", chunkBytes, MIN_CHUNK_BYTES);
      chunkBytes_ = 0;
      return Status::Error;
    }
    if (want != chunkBytes) {
      LOG_DBG("CEX", "Chunk buffer degraded %zu -> %zu bytes (heap too fragmented for the full size)", chunkBytes,
              want);
    }
    chunkBytes_ = want;
  }

  size_t produced = 0;
  bool done = false;
  // chunkBytes_ , not chunkBytes: the allocation above may have degraded to a smaller buffer.
  if (!reader_->step(buf_, chunkBytes_, &produced, &done)) {
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
  if (!epub.loadForCover()) return nullptr;  // cover ref only, no full book.bin build

  // If cover.img already exists and is a recognized image format, no extraction
  // needed. Must match the decode side's validity check (coverImageCachedValidOnly,
  // which also checks magic bytes) rather than a size-only check: a stale/corrupt
  // cover.img (e.g. left by an interrupted earlier extraction) is non-empty but
  // has no recognized format, so a size-only check here would treat it as
  // "already cached" forever while generateThumbBmp() perpetually reports it
  // invalid — a silent deadlock with no ERR log (observed as an EPUB whose cover
  // renders fine in-book but never produces a home-screen thumbnail).
  // openFileForWrite() truncates (O_TRUNC), so begin() below safely overwrites it.
  const std::string coverImgPath = epub.getCoverImageCachePath();
  if (epub.coverImageCachedValidOnly()) return nullptr;  // already cached

  // Cover bytes already on disk that no decoder recognises must NOT be re-extracted: the ZIP entry
  // inflates to the same bytes every time, so a caller whose ladder ends in "start an extract
  // session" would re-inflate them forever (observed at ~1.3 s per round on an EPUB whose
  // "cover.png" is really an AVIF). generateThumbBmp() has already written the structural sentinel
  // for this case. The >=8-byte sniff inside coverImageCachedButUnsupported() means a partially
  // extracted PNG/JPEG never reaches this verdict, so an interrupted extraction still resumes.
  if (epub.coverImageCachedButUnsupported()) {
    LOG_DBG("CEX", "Cached cover.img for %s is an unsupported format — not re-extracting", bookPath.c_str());
    return nullptr;
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

// Which extensions count, and how the base name is derived, live in
// SidecarFiles - see the header there for why this is one definition.
std::string ReaderActivity::sidecarCoverPath(const std::string& bookPath) {
  const std::string candidate = SidecarFiles::coverPath(bookPath);
  if (!candidate.empty()) LOG_DBG("SIDECAR", "Found sidecar cover: %s", candidate.c_str());
  return candidate;
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
    // Reuse only a COMPLETE BMP. A previous conversion truncated by an interrupted write would
    // otherwise be returned and drawn forever (fails partway); remove it and reconvert instead.
    if (isCoverThumbComplete(bmpPath)) {
      LOG_DBG("COVER", "convertSidecarToBmp: BMP already exists (complete) path=%s", bmpPath.c_str());
      return bmpPath;
    }
    LOG_DBG("COVER", "convertSidecarToBmp: existing BMP truncated, regenerating path=%s", bmpPath.c_str());
    Storage.remove(bmpPath.c_str());
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
    // Verbatim copy is only usable when the sidecar already fits the slot: the themes
    // draw thumbs 1:1, and an oversized BMP would be rescaled at draw time, aliasing
    // its dithered pixels into a grid. (It would also fail isCoverThumbComplete's
    // oversize check and trigger an endless re-copy loop.) Reject instead; after the
    // transient budget the book gets a placeholder.
    Bitmap bmp(src);
    if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() <= width && bmp.getHeight() <= height &&
        src.seek(0)) {
      uint8_t buffer[1024];
      while (src.available()) dst.write(buffer, src.read(buffer, sizeof(buffer)));
      ok = true;
    } else {
      LOG_DBG("COVER", "Sidecar BMP unusable for %dx%d slot (must fit 1:1): %s", width, height, sidecarPath.c_str());
    }
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

bool ReaderActivity::isCoverThumbComplete(const std::string& path, int expectedWidth, int expectedHeight) {
  FsFile f;
  if (!Storage.openFileForRead("COVER", path, f)) return false;
  if (f.size() == 0) {  // 0-byte sentinel from a prior failed extraction → treat as missing
    f.close();
    return false;
  }
  Bitmap bmp(f);
  // Header intact but pixel data short → truncated by an interrupted write; treat as invalid so
  // the caller regenerates rather than keeping the unrenderable partial forever.
  bool ok = bmp.parseHeaders() == BmpReaderError::Ok && bmp.isComplete();
  // A thumb LARGER than its slot in either dimension was generated by an older build whose
  // crop mode kept the overfill (e.g. a 340x561 BMP in thumb_340x540.bmp). The themes draw
  // thumbs 1:1, so drawBitmap would rescale it — aliasing the 1-bit dither into a visible
  // grid. Treat it as invalid so it is regenerated at the exact slot size. Smaller (fit-mode
  // legacy) thumbs draw 1:1 with whitespace and stay valid — never a regeneration loop.
  if (ok && expectedWidth > 0 && expectedHeight > 0 &&
      (bmp.getWidth() > expectedWidth || bmp.getHeight() > expectedHeight)) {
    LOG_DBG("COVER", "Thumb %s is %dx%d, larger than slot %dx%d — regenerating", path.c_str(), bmp.getWidth(),
            bmp.getHeight(), expectedWidth, expectedHeight);
    ok = false;
  }
  f.close();
  return ok;
}

bool ReaderActivity::writeCoverPlaceholderBmp(const std::string& path) {
  // A 1x1 marker, not a slot-sized picture. The old full-size version wrote an empty framed box
  // that every theme then blitted over the tile — so a book whose cover we gave up on rendered
  // WORSE than a book with no cover at all, which gets the theme's proper no-cover tile. The
  // marker keeps the on-disk bookkeeping (complete BMP => isCoverThumbComplete => never
  // re-decoded, and smaller than any slot so it never trips the oversize regeneration path)
  // while letting the themes recognise it and draw that tile. Also 62 bytes instead of ~23 KB.
  const int width = UITheme::COVER_PLACEHOLDER_DIM;
  const int height = UITheme::COVER_PLACEHOLDER_DIM;
  const int rowBytes = ((width + 31) / 32) * 4;  // 1-bit rows padded to 4 bytes
  uint8_t row[256];                              // cover thumbs are <=464px wide (rowBytes<=60)
  if (rowBytes > static_cast<int>(sizeof(row))) return false;
  const uint32_t imageSize = static_cast<uint32_t>(rowBytes) * static_cast<uint32_t>(height);
  const uint32_t offBits = 14 + 40 + 8;  // file header + info header + 2-entry palette

  FsFile f;
  if (!Storage.openFileForWrite("COVER", path, f)) return false;
  auto w16 = [&](uint16_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    f.write(b, 2);
  };
  auto w32 = [&](uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                          static_cast<uint8_t>(v >> 24)};
    f.write(b, 4);
  };
  f.write(reinterpret_cast<const uint8_t*>("BM"), 2);
  w32(offBits + imageSize);                     // bfSize
  w16(0);                                       // reserved1
  w16(0);                                       // reserved2
  w32(offBits);                                 // bfOffBits
  w32(40);                                      // biSize
  w32(static_cast<uint32_t>(width));            // biWidth
  w32(static_cast<uint32_t>(-height));          // biHeight (negative => top-down)
  w16(1);                                       // biPlanes
  w16(1);                                       // biBitCount
  w32(0);                                       // biCompression = BI_RGB
  w32(imageSize);                               // biSizeImage
  w32(0);                                       // biXPelsPerMeter
  w32(0);                                       // biYPelsPerMeter
  w32(2);                                       // biClrUsed (black, white)
  w32(0);                                       // biClrImportant
  const uint8_t black[4] = {0, 0, 0, 0};        // palette index 0 = black
  const uint8_t white[4] = {255, 255, 255, 0};  // palette index 1 = white
  f.write(black, 4);
  f.write(white, 4);
  // Pixel rows (top-down). Nothing here is ever displayed — the themes branch on the 1x1 size
  // before drawing — so the pixel is simply white (bit 1) to keep the file a well-formed BMP.
  for (int y = 0; y < height; y++) {
    memset(row, 0xFF, rowBytes);
    f.write(row, rowBytes);
  }
  f.close();
  return true;
}

namespace {
// True if a thumbnail file exists and is usable. A 0-byte sentinel left by a prior failed
// extraction, a partial BMP left truncated by an interrupted write, OR a BMP larger than its
// slot (older crop mode kept the overfill), must be treated as missing so regeneration retries
// (see ReaderActivity::isCoverThumbComplete).
bool thumbFileValid(const std::string& path, int width, int height) {
  return ReaderActivity::isCoverThumbComplete(path, width, height);
}

// The per-format generators (Epub/Xtc/Txt generateThumbBmp) each short-circuit on an existing,
// COMPLETE thumb file — they know nothing about slot dimensions. When thumbFileValid has just
// rejected an existing file (oversized legacy crop output), the file must be deleted before
// calling them, or the "regeneration" silently no-ops and the stale file survives every pass
// (observed: "larger than slot — regenerating" + "ok" with no decode between, every boot,
// forever). 0-byte sentinels are kept: they encode "structurally absent" for the Epub generator.
// FAT packs the modify date and time so that (date << 16) | time increases
// monotonically, which is all the ordering this needs. False when either stamp
// is unreadable, so an unknown age never triggers a rebuild.
//
// Note the device clock gates this: HalStorage writes 1980-01-01 to every file
// created while the RTC is unsynced, so two such files compare equal and the
// cover is left alone. On hardware with the DS3231 that does not arise.
bool fileModifiedStamp(const std::string& path, uint32_t* out) {
  FsFile f;
  if (!Storage.openFileForRead("COVER", path, f)) return false;
  uint16_t date = 0, time = 0;
  const bool ok = f.getModifyDateTime(&date, &time);
  f.close();
  if (!ok) return false;
  *out = (static_cast<uint32_t>(date) << 16) | time;
  return true;
}

// True when the sidecar has been replaced since the thumb was cached.
bool sidecarNewerThanThumb(const std::string& sidecarPath, const std::string& thumbPath) {
  uint32_t sidecarStamp = 0;
  uint32_t thumbStamp = 0;
  if (!fileModifiedStamp(sidecarPath, &sidecarStamp)) return false;
  if (!fileModifiedStamp(thumbPath, &thumbStamp)) return false;
  return sidecarStamp > thumbStamp;
}

void removeStaleThumb(const std::string& path) {
  FsFile stale;
  if (!Storage.openFileForRead("COVER", path, stale)) return;
  const bool isSentinel = stale.size() == 0;
  stale.close();
  if (!isSentinel) {
    LOG_DBG("COVER", "Removing stale thumb before regeneration: %s", path.c_str());
    Storage.remove(path.c_str());
  }
}

// Migration heal for stale sentinels written by an older build's blunt failure policy (any
// extraction/decode failure — even a transient one — used to leave a permanent 0-byte sentinel).
// Under the current policy a sentinel means "structurally absent"; if one exists yet the book's
// cover.img is present AND a valid image format, the sentinel is stale — clear it so the normal
// generateThumbBmp path gets a fresh chance. No-op unless BOTH a sentinel and a valid cover.img
// exist, so a legitimately-absent cover keeps its sentinel.
void healStaleEpubSentinel(const std::string& bookPath, const std::string& thumbFile) {
  if (!FsHelpers::hasEpubExtension(bookPath)) return;
  FsFile sentinel;
  if (!Storage.openFileForRead("COVER", thumbFile, sentinel)) return;
  const bool isSentinel = sentinel.size() == 0;
  sentinel.close();
  if (!isSentinel) return;

  // coverImageCachedValidOnly() checks cover.img exists + non-empty + a known image format,
  // without triggering any extraction — exactly the "sentinel is stale" condition. loadForCover()
  // gets the cover ref without a full book.bin build.
  Epub epub(bookPath, "/.crosspoint");
  if (epub.loadForCover() && epub.coverImageCachedValidOnly()) {
    LOG_DBG("COVER", "Healing stale sentinel %s (cover.img present & valid)", thumbFile.c_str());
    Storage.remove(thumbFile.c_str());
  }
}
}  // namespace

ThumbResult ReaderActivity::ensureCoverThumb(const std::string& bookPath, int width, int height) {
  const std::string dir = bookCacheDir(bookPath);
  const std::string name = "thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
  const std::string file = dir + "/" + name;

  // Source preference: a sidecar image beside the book wins over the embedded cover.
  const std::string sidecar = sidecarCoverPath(bookPath);

  // Replacing a cover must take effect. Nothing else invalidates this thumb, so
  // without the check the short circuit below serves the previous image forever
  // — the cover editor's whole workflow, and replacing a cover by hand, both
  // looked like they had silently failed.
  if (!sidecar.empty() && sidecarNewerThanThumb(sidecar, file)) {
    LOG_DBG("COVER", "Sidecar newer than cached thumb, regenerating: %s", sidecar.c_str());
    Storage.remove(file.c_str());
  }

  if (thumbFileValid(file, width, height)) return ThumbResult::Ok;
  removeStaleThumb(file);

  if (!sidecar.empty()) {
    // A sidecar may have changed since the sentinel was written — clear it so the
    // sidecar conversion runs fresh.
    if (Storage.exists(file.c_str())) Storage.remove(file.c_str());
    const std::string result = convertSidecarToBmp(dir, sidecar, width, height, name);
    LOG_DBG("COVER", "convertSidecarToBmp(%dx%d) sidecar=%s result=%s", width, height, sidecar.c_str(),
            result.empty() ? "FAILED" : result.c_str());
    if (!result.empty()) return ThumbResult::Ok;
    // Sidecar conversion failed (bailed for input, OOM, decode error) — transient. Let the
    // caller retry; don't fall through to an embedded-cover parse this pass.
    return ThumbResult::TransientFail;
  }

  // No usable sidecar — fall back to the embedded cover.  generateThumbBmp() only
  // DECODES an already-extracted cover.img (abortable, ≤~1 s); it no longer inflates
  // the cover from the ZIP itself.  If cover.img isn't cached yet it reports TransientFail
  // without a sentinel, and the caller's sliced beginCoverExtractSession extracts it
  // across ticks before a later pass decodes it.  This keeps the 35 s ZIP inflate off
  // the per-tick path while still covering both embedded JPEG and PNG covers.
  if (FsHelpers::hasEpubExtension(bookPath)) {
    // Clear any sentinel left permanent by an older build for what was only a transient failure,
    // so a book whose cover.img is actually present & valid gets decoded instead of skipped.
    healStaleEpubSentinel(bookPath, file);
    Epub epub(bookPath, "/.crosspoint");
    // loadForCover(): get the cover reference WITHOUT building the spine/TOC book.bin — showing a
    // thumbnail must never trigger a full-book parse (a 1732-spine book's index build is slow and was
    // a crash site). allowExtract=false: decode only an already-cached cover.img; the sliced
    // beginCoverExtractSession owns the (potentially multi-second) ZIP inflate.
    if (!epub.loadForCover()) return ThumbResult::TransientFail;
    return epub.generateThumbBmp(width, height, /*allowExtract=*/false);
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    return (xtc.load() && xtc.generateThumbBmp(width, height)) ? ThumbResult::Ok : ThumbResult::TransientFail;
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.crosspoint");
    return txt.generateThumbBmp(width, height) ? ThumbResult::Ok : ThumbResult::TransientFail;
  }
  return ThumbResult::TransientFail;
}

ThumbResult ReaderActivity::ensureCoverThumb(const std::string& bookPath, int height) {
  const std::string dir = bookCacheDir(bookPath);
  const std::string name = "thumb_" + std::to_string(height) + ".bmp";
  const std::string file = dir + "/" + name;
  if (thumbFileValid(file, height * 6 / 10, height)) return ThumbResult::Ok;
  removeStaleThumb(file);

  // Embedded single-height thumbnails scale to height*0.6 wide; mirror that for the sidecar.
  const std::string sidecar = sidecarCoverPath(bookPath);
  if (!sidecar.empty()) {
    // Sidecar may have changed — clear sentinel so conversion runs fresh.
    if (Storage.exists(file.c_str())) Storage.remove(file.c_str());
    const std::string result = convertSidecarToBmp(dir, sidecar, height * 6 / 10, height, name);
    LOG_DBG("COVER", "convertSidecarToBmp(h=%d) sidecar=%s result=%s", height, sidecar.c_str(),
            result.empty() ? "FAILED" : result.c_str());
    if (!result.empty()) return ThumbResult::Ok;
    // Sidecar conversion failed (bailed for input, OOM, decode error) — transient. Let the
    // caller retry; don't fall through to an embedded-cover parse this pass.
    return ThumbResult::TransientFail;
  }

  // Embedded EPUB cover: generateThumbBmp() decodes an already-extracted cover.img only
  // (see the width/height overload) — the sliced beginCoverExtractSession handles the
  // ZIP inflate so the 35 s stall stays off the per-tick path.
  if (FsHelpers::hasEpubExtension(bookPath)) {
    // Clear any sentinel left permanent by an older build for what was only a transient failure.
    healStaleEpubSentinel(bookPath, file);
    Epub epub(bookPath, "/.crosspoint");
    // loadForCover(): cover reference only, no full book.bin build (see the width/height overload).
    // allowExtract=false: decode only an already-cached cover.img; the sliced
    // beginCoverExtractSession owns the (potentially multi-second) ZIP inflate.
    if (!epub.loadForCover()) return ThumbResult::TransientFail;
    return epub.generateThumbBmp(height, /*allowExtract=*/false);
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    return (xtc.load() && xtc.generateThumbBmp(height)) ? ThumbResult::Ok : ThumbResult::TransientFail;
  }
  if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.crosspoint");
    return txt.generateThumbBmp(height) ? ThumbResult::Ok : ThumbResult::TransientFail;
  }
  return ThumbResult::TransientFail;
}

namespace {
// Shared core: start a sliced PNG decode into an explicitly-named thumb file. The two public
// overloads differ only in that name ("thumb_<W>x<H>.bmp" vs "thumb_<H>.bmp"), so they both
// funnel through here to avoid duplicating the sidecar/cover.img source selection and setup.
std::unique_ptr<PngDecodeSession> beginPngThumbSessionImpl(const std::string& bookPath, int width, int height,
                                                           const std::string& name,
                                                           ReaderActivity::PngThumbFiles& filesOut) {
  const std::string dir = ReaderActivity::bookCacheDir(bookPath);
  const std::string bmpPath = dir + "/" + name;

  // Already cached (valid, exact slot size) — caller should have checked, but be safe.
  if (ReaderActivity::isCoverThumbComplete(bmpPath, width, height)) return nullptr;

  // Sidecar PNG takes priority over embedded cover.
  std::string srcPath;
  const std::string sidecar = ReaderActivity::sidecarCoverPath(bookPath);
  bool isSidecar = false;
  if (!sidecar.empty() && FsHelpers::hasPngExtension(sidecar)) {
    srcPath = sidecar;
    isSidecar = true;
    // Clear any stale sentinel so the write can proceed.
    if (Storage.exists(bmpPath.c_str())) Storage.remove(bmpPath.c_str());
  } else if (sidecar.empty() && FsHelpers::hasEpubExtension(bookPath)) {
    // Embedded EPUB cover. Do NOT extract here — ensureCoverImageCached() would run a
    // synchronous, possibly multi-MB ZIP inflate in one tick (observed 35 s stalls).
    // Only proceed if cover.img is ALREADY extracted (the sliced beginCoverExtractSession
    // runs first in the caller's ladder and produces it). Otherwise return null so the
    // caller falls through to that sliced extraction.
    Epub epub(bookPath, "/.crosspoint");
    if (!epub.loadForCover()) return nullptr;  // cover ref only, no full book.bin build
    srcPath = epub.getCoverImageCachePath();
    FsFile peek;
    if (!Storage.openFileForRead("PNG", srcPath, peek)) return nullptr;  // not yet extracted
    uint8_t magic[8];
    const bool isPng = peek.read(magic, 8) == 8 && magic[0] == 0x89 && magic[1] == 0x50;
    const bool nonEmpty = peek.size() > 0;
    peek.close();
    if (!nonEmpty || !isPng) return nullptr;
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
}  // namespace

std::unique_ptr<PngDecodeSession> ReaderActivity::beginPngThumbSession(const std::string& bookPath, int width,
                                                                       int height, PngThumbFiles& filesOut) {
  const std::string name = "thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
  return beginPngThumbSessionImpl(bookPath, width, height, name, filesOut);
}

std::unique_ptr<PngDecodeSession> ReaderActivity::beginPngThumbSession(const std::string& bookPath, int height,
                                                                       PngThumbFiles& filesOut) {
  // Single-height thumbs scale to height*0.6 wide (mirrors the synchronous single-height decode).
  const std::string name = "thumb_" + std::to_string(height) + ".bmp";
  return beginPngThumbSessionImpl(bookPath, height * 6 / 10, height, name, filesOut);
}

namespace {

// Progress handler for Epub::load(), installed only for the duration of that call.
//
// load() can spend seconds on a cache-miss CSS rebuild and painted nothing while it did.
// On a wake resume there is no splash behind it either, so the panel keeps showing the
// sleep screen and the device reads as dead — the failure behind issue #155.
//
// Safe to paint from here: load() is called from ReaderActivity::onEnter(), which runs on
// the loop task, and the RenderLock below is the same one the first-open popup a few lines
// down already takes. The handler is scoped to the call (see the RAII installer) so it can
// never fire from a task that does not own the framebuffer.
uint32_t g_openStartMs = 0;
uint32_t g_openLastPaintMs = 0;
// The renderer belongs to the activity, and the handler is a plain function pointer with no
// context argument (see LongTaskProgress.h on why it is not a std::function). The scope
// guard below owns the lifetime of this pointer: it is set on entry and cleared on exit
// together with the handler, so it can never be stale when the handler runs.
GfxRenderer* g_openRenderer = nullptr;

void onOpenProgress(const char* stage) {
  // Thresholds rather than a cadence: each paint is an e-ink refresh, and a warm open
  // finishes well inside the first one, so a healthy book costs nothing. After that first
  // hint every repaint is a liveness update on a long interval — the device saying it is
  // still working and what on, which is what a stalled-looking open needs.
  constexpr uint32_t FIRST_HINT_MS = 2000;
  constexpr uint32_t LIVENESS_INTERVAL_MS = 15000;

  const uint32_t now = millis();
  const uint32_t elapsed = now - g_openStartMs;
  if (elapsed < FIRST_HINT_MS) {
    return;
  }
  if (g_openLastPaintMs != 0 && now - g_openLastPaintMs < LIVENESS_INTERVAL_MS) {
    return;
  }
  g_openLastPaintMs = now;

  // The stage token is the area of work, deliberately untranslated so it reads the same in
  // a bug report from any locale — the same rule the Boot Diagnostics values follow.
  if (g_openRenderer == nullptr) {
    return;
  }
  char message[64];
  snprintf(message, sizeof(message), "%s %s %us", tr(STR_INDEXING), stage, static_cast<unsigned>(elapsed / 1000));
  RenderLock lock;
  GUI.drawPopup(*g_openRenderer, message);
}

// Installs the handler for a scope and guarantees it is cleared — an installed handler
// paints, so it must not outlive the one call site that can safely do so.
struct ScopedOpenProgress {
  // `enabled` is false on the first-open path: it already draws its own popup, and it may
  // have released the secondary framebuffer for indexing headroom — drawPopup() needs that
  // buffer, so painting there would be the crash compileSectionCache warns about. The warm
  // open is the one with no feedback at all, and the one where the cache-miss CSS rebuild
  // sits, so that is where this is armed.
  ScopedOpenProgress(GfxRenderer& renderer, const bool enabled) {
    if (!enabled) {
      return;
    }
    g_openStartMs = millis();
    g_openLastPaintMs = 0;
    g_openRenderer = &renderer;
    LongTaskProgress::setHandler(&onOpenProgress);
  }
  ~ScopedOpenProgress() {
    LongTaskProgress::setHandler(nullptr);
    g_openRenderer = nullptr;
  }
  ScopedOpenProgress(const ScopedOpenProgress&) = delete;
  ScopedOpenProgress& operator=(const ScopedOpenProgress&) = delete;
};

}  // namespace

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
    sync.postAction = KOReaderSyncPostAction::Reader;
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
  // Start the wake-to-page trace. Whether this open is a deep-sleep resume was latched by
  // setup() (WakeTrace::armResume) rather than read from APP_STATE here — see armResume().
  WakeTrace::begin();

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
    const bool firstOpenIndexing = Epub(initialBookPath, "/.crosspoint").needsFirstOpenIndexing();
    if (firstOpenIndexing) {
      RenderLock lock;
      // drawPopup() overlays the indexing box on the displayed frame (it resyncs the write buffer
      // from the screen first), so the box lands on the launcher screen — e.g. the recent-books grid
      // — without the stale-buffer diff toggling the old/new selection cells before the popup appears.
      GUI.drawPopup(renderer, tr(STR_INDEXING));
    }
    // First-open indexing allocates per-spine transient state — the batch-size-lookup vectors
    // (~28 KB contiguous for a 1700-spine book) and the TOC's 32 KB NCX inflate ring — while the
    // ~48 KB secondary framebuffer sits unused. Observed on-device: buildBookBin aborted (bare
    // operator new under -fno-exceptions) at 51 KB free / 30 KB contig, after the TOC pass had
    // already lost its inflate ring and produced 0 TOC entries. Lend the buffer out for the build
    // (the popup above is drawn BEFORE the release — drawPopup needs the active buffer — and
    // load() performs no rendering) and restore it before the reader activity starts. Same
    // pattern as HomeActivity cover loading. If the realloc fails, EpubReaderActivity::onEnter
    // sees the missing buffer (secondaryBufferDegraded_) and recovers once heap allows.
    bool releasedForIndexing = false;
    if (firstOpenIndexing && renderer.hasSecondaryBuffer()) {
      RenderLock lock;
      if (renderer.releaseSecondaryBuffer()) {
        releasedForIndexing = true;
        // Keep X4 fast-differential refresh alive off the controller's RED RAM (seeded by the
        // popup's displayBuffer just above); no-op on X3.
        renderer.setSingleBufferFastDiff(true);
        LOG_INF("READER", "Released secondary framebuffer for first-open indexing (free=%lu)",
                static_cast<unsigned long>(esp_get_free_heap_size()));
      }
    }
    // Armed only for the warm open — see ScopedOpenProgress. Covers Epub::load()'s
    // milestones, the slowest of which (a cache-miss CSS rebuild) previously painted nothing.
    ScopedOpenProgress openProgress(renderer, !firstOpenIndexing);
    auto epub = loadEpub(initialBookPath);
    if (releasedForIndexing) {
      RenderLock lock;
      bool restored = renderer.reallocSecondaryBuffer();
      if (!restored && epub) {
        // The indexing pass ran with the framebuffer's block free, so some of the Epub's
        // book-lifetime allocations (spine/TOC vectors, CSS index) can now sit inside it —
        // unevictable while the object lives, and the reason the realloc just missed.
        // The indexing caches were written to SD above, so drop the object, reclaim the
        // block, and reload on the warm-cache path (~50 ms, needs no released headroom).
        // Field-observed on X3: this exact miss previously cost a heap-recovery restart.
        epub.reset();
        restored = renderer.reallocSecondaryBuffer();
        LOG_INF("READER", "Dropped ePub to unpin framebuffer block (realloc %s); reloading from warm cache",
                restored ? "ok" : "still failing");
        epub = loadEpub(initialBookPath);
      }
      if (restored) {
        renderer.setSingleBufferFastDiff(false);
        LOG_INF("READER", "Restored secondary framebuffer after first-open indexing");
      } else {
        LOG_ERR("READER", "Secondary framebuffer realloc failed after indexing (free=%lu); reader will recover",
                static_cast<unsigned long>(esp_get_free_heap_size()));
      }
    }
    if (!epub) {
      onGoBack();
      return;
    }
    // Stamped after every load path above (warm cache, first-open index build, and the
    // drop-and-reload retry) so `book` is the true cost of getting a usable Epub.
    WakeTrace::mark(WakeTrace::Phase::BookLoaded);
    onGoToEpubReader(std::move(epub));
  }
}

void ReaderActivity::onGoBack() { finish(); }
