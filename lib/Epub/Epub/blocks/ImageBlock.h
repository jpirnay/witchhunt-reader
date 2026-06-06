#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

// Source pixel area above which an image is considered "large" and rendered
// as a placeholder until the user explicitly requests it.
// 800x600 covers most full-page illustrations that take >1s to dither on ESP32.
static constexpr int32_t LARGE_IMAGE_PIXEL_THRESHOLD = 800 * 600;

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height, const std::string& altText = "");
  // Extended constructor used when lazy extraction is desired:
  // imagePath is the SD cache destination; epubFilePath + epubEntryPath are the source.
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height, const std::string& altText,
             const std::string& epubFilePath, const std::string& epubEntryPath);
  ~ImageBlock() override = default;

  // Return a new ImageBlock that renders a vertical crop of this image.
  // srcYOffset: first source row to render; srcHeight: number of rows (must be > 0).
  // The new block shares the same imagePath (and thus the same decoded cache file);
  // only the rendering window differs.
  std::unique_ptr<ImageBlock> makeCrop(int16_t srcYOffset, int16_t srcHeight) const;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }
  // Rendered height: equals srcHeight when a crop is active, otherwise height.
  int16_t getRenderedHeight() const { return srcHeight_ > 0 ? srcHeight_ : height; }
  const std::string& getAltText() const { return altText; }

  bool imageExists() const;

  // Returns true if the source image dimensions exceed LARGE_IMAGE_PIXEL_THRESHOLD.
  // Result is cached after the first call to avoid repeated header reads.
  bool isLargeImage() const;

  // Returns true if this image would be shown as a placeholder given forceLoad.
  // False when: forceLoad is true, image is not large, or the mode-specific cache exists.
  // monochromeOutput selects which cache to check (BW or grayscale), matching render().
  bool wouldShowPlaceholder(bool forceLoad, bool monochromeOutput) const;

  // True when the 1-bit .pxc pixel cache exists (BW plane rendering).
  // Used by warm-cache paths to skip already-cached images.
  bool hasPixelCache() const;

  // True when the 4-level .pxc pixel cache exists (grayscale AA rendering).
  bool hasGrayscaleCache() const;

  // Render the 4-level cache into the framebuffer using the renderer's current
  // mode (GRAYSCALE_LSB or GRAYSCALE_MSB). No-op if no grayscale cache exists.
  // Called by the AA grayscale passes to give images proper gray tones.
  void renderGrayscaleFromCache(GfxRenderer& renderer, int x, int y) const;

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  // monochromeOutput=true: 1-bit Atkinson dither → BW-plane rendering (AA off)
  // monochromeOutput=false: 4-level Bayer dither → also replayed in grayscale passes (AA on)
  void render(GfxRenderer& renderer, int x, int y, bool forceLoad = true, bool monochromeOutput = true);
  bool serialize(FsFile& file);
  static std::unique_ptr<ImageBlock> deserialize(FsFile& file);

 private:
  std::string imagePath;  // SD card destination path (may not exist until first render)
  std::string altText;
  int16_t width;
  int16_t height;
  // Vertical crop window into the decoded image. srcYOffset_==0 && srcHeight_==0 means full image.
  int16_t srcYOffset_ = 0;
  int16_t srcHeight_ = 0;
  std::string epubFilePath_;   // source EPUB on SD (empty if already extracted)
  std::string epubEntryPath_;  // internal EPUB entry path (e.g. "OEBPS/images/foo.jpg")

  // Ensure the SD cache file exists, extracting from the EPUB if necessary.
  // Returns true if the file is ready for decoding.
  bool ensureExtracted() const;

  void renderPlaceholder(GfxRenderer& renderer, int x, int y) const;
};
