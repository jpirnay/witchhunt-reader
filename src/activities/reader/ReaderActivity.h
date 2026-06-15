#pragma once
#include <memory>

#include "../Activity.h"
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
  static bool ensureCoverThumb(const std::string& bookPath, int width, int height);
  static bool ensureCoverThumb(const std::string& bookPath, int height);

  // Render a sidecar image (or copy a sidecar BMP) into a scaled 1-bit BMP at
  // "<cacheDir>/<fileName>". Returns the written path, or "" on failure.
  static std::string convertSidecarToBmp(const std::string& cacheDir, const std::string& sidecarPath, int width,
                                         int height, const std::string& fileName);

  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath)
      : Activity("Reader", renderer, mappedInput), initialBookPath(std::move(initialBookPath)) {}
  void onEnter() override;
  bool isReaderActivity() const override { return true; }
};
