#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/EpubImageManifest.h"
#include "Epub/css/CssParser.h"

class ZipFile;

enum class OpfCacheMode { Disabled, Enabled };

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // the page-map.xml file (EPUB 2.01 printed page list, separate from NCX <pageList>)
  std::string pageMapItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // Image manifest (dimensions + ZIP stat for every image in the epub)
  std::unique_ptr<EpubImageManifest> imageManifest;
  // CSS files
  std::vector<std::string> cssFiles;
  // -1 unknown, 0 unreliable, 1 reliable
  mutable int tocReliabilityState = -1;
  // Library-level option: app code can override this per-book instance.
  bool syntheticTocFallbackEnabled = false;

  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, OpfCacheMode cacheMode);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  bool parsePageMapFile() const;
  void parseCssFiles() const;
  void discoverCssFilesFromZip();
  void buildImageManifest(ZipFile& zf);

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
    // create a cache key based on the filepath
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  bool clearCache(bool preserveThumbs = false) const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  const std::string& getSeries() const;
  const std::string& getSeriesIndex() const;
  const std::string& getDescription() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  std::string getCoverImageCachePath() const;
  bool ensureCoverImageCached() const;
  bool generateCoverBmp(bool cropped = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  std::string getThumbBmpPath(int width, int height) const;
  bool generateThumbBmp(int height) const;
  bool generateThumbBmp(int width, int height) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;
  // Read up to maxBytes decompressed bytes from a ZIP entry — no SD write, header-only use.
  size_t readItemHeaderBytes(const std::string& itemHref, uint8_t* outBuf, size_t maxBytes) const;
  // Extract a ZIP entry to a local SD file. Used for lazy image extraction at render time.
  bool extractItemToFile(const std::string& itemHref, const std::string& destPath) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  bool getSpineItemInflatedSize(int spineIndex, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  bool hasReliableToc() const;
  void setSyntheticTocFallbackEnabled(bool enabled) { syntheticTocFallbackEnabled = enabled; }
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  // Load (or build) the image manifest. Call after load() when images will be rendered.
  // Skipping this is valid for text-only or placeholder rendering modes.
  void loadImageManifest();
  const EpubImageManifest* getImageManifest() const { return imageManifest.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;

  // Printed-page list (from NCX <pageList> / EPUB 3 nav page-list / EPUB 2.01 page-map.xml).
  // One entry per printed-page anchor: spine href + fragment id + visible label.
  struct PrintedPageEntry {
    std::string href;
    std::string anchor;
    std::string label;
  };
  // Reads <cachePath>/pagelist.bin. Returns empty vector when the book has no printed-page data.
  // Inexpensive — only invoked from menu paths, not page-turn hot paths.
  std::vector<PrintedPageEntry> loadPrintedPageList() const;
};
