#pragma once
#include <PngToBmpConverter.h>
#include <ZipFile.h>

#include <memory>

#include "../Activity.h"
#include "Epub/ThumbResult.h"
#include "activities/home/FileBrowserActivity.h"

class Epub;
class Xtc;
class Txt;

class ReaderActivity final : public Activity {
  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  static std::unique_ptr<Epub> loadEpub(const std::string& path);
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static std::unique_ptr<Txt> loadTxt(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool isMdFile(const std::string& path);
  static bool isImageFile(const std::string& path);

  static std::string extractFolderPath(const std::string& filePath);
  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToTxtReader(std::unique_ptr<Txt> txt);
  void onGoToMdReader(std::unique_ptr<Txt> txt);
  void onGoToBmpViewer(const std::string& path);

  void onGoBack();

 public:
  static std::string sidecarCoverPath(const std::string& bookPath);
  static std::string bookCacheDir(const std::string& bookPath);

  // Grid cover thumbnails. The on-disk result is source-agnostic and identical no matter
  // where the cover came from: "<bookCacheDir>/thumb_<W>x<H>.bmp" (or "thumb_<H>.bmp" for the
  // single-height themes), registered in RecentBooks via the template coverThumbPlaceholder()
  // returns. A sidecar image beside the book always takes precedence over the embedded cover
  // as the *source*; the embedded cover is only parsed when no sidecar exists.
  static std::string coverThumbPlaceholder(const std::string& bookPath);
  // Produce (or reuse) the cover thumbnail BMP for a book. Returns ThumbResult so the caller can
  // tell a structural absence (no cover / unsupported — safe to record permanently) from a
  // transient failure (retry next pass/boot). A sidecar image beside the book, when present and
  // convertible, always yields Ok and clears any stale sentinel first.
  static ThumbResult ensureCoverThumb(const std::string& bookPath, int width, int height);
  static ThumbResult ensureCoverThumb(const std::string& bookPath, int height);
  // True only if a cover thumbnail BMP exists AND holds all its declared pixel rows. A thumbnail
  // whose write was interrupted (reboot/abort mid-decode) is left truncated on the SD card; it
  // passes a naive size>0 check but fails to draw partway (GFX "Failed to read row N"). Treating
  // such a file as invalid lets the caller regenerate it instead of drawing/keeping it forever.
  // When expectedWidth/Height are given (> 0), a BMP LARGER than that in either dimension is
  // also invalid: older builds' crop mode kept the overfill dimension, and drawing such a thumb
  // 1:1-sized slots forces a rescale of the dithered image (visible grid). Regeneration with the
  // current converters yields an exact-size thumb.
  static bool isCoverThumbComplete(const std::string& path, int expectedWidth = 0, int expectedHeight = 0);
  // Write a minimal but VALID 1-bit BMP (white box with a black frame) at the thumbnail path, used
  // to mark a book that has no extractable cover. Being a complete BMP it passes
  // isCoverThumbComplete(), so the cover loops treat the book as resolved and never re-open the EPUB
  // to rediscover the absence — without the 0-byte-file "mess" (we now treat empty files as invalid).
  static bool writeCoverPlaceholderBmp(const std::string& path);

  // Sliced extraction of a ZIP entry to a file, one chunk per continueStep() call.
  // Used to extract an embedded PNG cover (cover.img) without blocking loop() for
  // the full ~35-second decompress. Owns the ZipFile (which EntryReader references).
  class CoverExtractSession {
   public:
    enum class Status { Running, Done, Error };

    // Begin extracting zipEntryPath from epubPath into destPath.
    // Returns false if the entry cannot be opened.
    bool begin(const std::string& epubPath, const std::string& zipEntryPath, const std::string& destPath);

    // Decompress up to chunkBytes into destPath. Call repeatedly until not Running.
    Status continueStep(size_t chunkBytes = 4096);

    size_t bytesProduced() const;
    size_t totalBytes() const;

    ~CoverExtractSession();

   private:
    std::unique_ptr<ZipFile> zip_;
    std::unique_ptr<ZipFile::EntryReader> reader_;
    FsFile dst_;
    std::string destPath_;
    uint8_t* buf_ = nullptr;
    size_t chunkBytes_ = 0;
  };

  // Begin a sliced ZIP extraction for the embedded cover of bookPath.
  // Returns nullptr if the book has no extractable embedded PNG cover, or if
  // cover.img is already cached. On success the caller drives the session via
  // continueStep() each loop() tick until Done, then calls beginPngThumbSession.
  static std::unique_ptr<CoverExtractSession> beginCoverExtractSession(const std::string& bookPath);

  // Open FsFiles that must outlive a PngDecodeSession (session borrows pointers to them).
  struct PngThumbFiles {
    FsFile src;  // source PNG (sidecar or cover.img)
    FsFile dst;  // destination BMP (opened for write)
    void close() {
      if (src.isOpen()) src.close();
      if (dst.isOpen()) dst.close();
    }
  };

  // Start a sliced PNG decode for bookPath at the given thumb dimensions. Writes the
  // "thumb_<W>x<H>.bmp" (multi-size) form. Returns nullptr if the cover is not a PNG, is already
  // cached, or setup fails. On success, *filesOut owns the open FsFiles; caller must keep them
  // alive until the session completes and then close them.
  // On failure (nullptr return), the thumb file is left as a 0-byte sentinel.
  static std::unique_ptr<PngDecodeSession> beginPngThumbSession(const std::string& bookPath, int width, int height,
                                                                PngThumbFiles& filesOut);

  // Single-height variant: writes the "thumb_<H>.bmp" form used by the non-carousel themes, at
  // width = H*0.6 (matching the synchronous single-height decode). Same contract as above.
  static std::unique_ptr<PngDecodeSession> beginPngThumbSession(const std::string& bookPath, int height,
                                                                PngThumbFiles& filesOut);

  // Render a sidecar image (or copy a sidecar BMP) into a scaled 1-bit BMP at
  // "<cacheDir>/<fileName>". Returns the written path, or "" on failure.
  static std::string convertSidecarToBmp(const std::string& cacheDir, const std::string& sidecarPath, int width,
                                         int height, const std::string& fileName);

  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath)
      : Activity("Reader", renderer, mappedInput), initialBookPath(std::move(initialBookPath)) {}
  void onEnter() override;
  bool isReaderActivity() const override { return true; }
};
