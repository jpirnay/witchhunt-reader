#pragma once
#include <HalStorage.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

#include "FootnoteEntry.h"
#include "blocks/ImageBlock.h"
#include "blocks/TextBlock.h"

static constexpr uint8_t MAX_TABLE_COLS = 8;
static constexpr uint16_t MAX_TABLE_ROWS = 48;
static constexpr uint8_t TABLE_CELL_PADDING = 5;
static constexpr uint16_t MIN_COL_INNER_WIDTH = 24;
static constexpr uint8_t MAX_CELL_LINES = 64;
// Cap on the rendered height of an in-cell graphic, so a single image never blows
// a row past the viewport (which would force the paragraph fallback and drop it).
static constexpr uint16_t MAX_CELL_IMAGE_HEIGHT = 240;

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
  TAG_PageImage = 2,
  TAG_PageTable = 3,
  TAG_PageHR = 4,
};

// represents something that has been added to a page
class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;
  explicit PageElement(const int16_t xPos, const int16_t yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) = 0;
  virtual bool serialize(FsFile& file) = 0;
  virtual PageElementTag getTag() const = 0;  // Add type identification
};

// a line from a block element
class PageLine final : public PageElement {
  std::shared_ptr<TextBlock> block;

 public:
  PageLine(std::shared_ptr<TextBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  const std::shared_ptr<TextBlock>& getBlock() const { return block; }
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_PageLine; }
  static std::unique_ptr<PageLine> deserialize(FsFile& file);
};

// New PageImage class
class PageImage final : public PageElement {
  std::shared_ptr<ImageBlock> imageBlock;

 public:
  PageImage(std::shared_ptr<ImageBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), imageBlock(std::move(block)) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  void renderWithForceLoad(GfxRenderer& renderer, int xOffset, int yOffset, bool forceLoad,
                           bool monochromeOutput = true, bool alsoCacheOtherVariant = false);
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_PageImage; }
  static std::unique_ptr<PageImage> deserialize(FsFile& file);
  const ImageBlock& getImageBlock() const { return *imageBlock; }
};

class PageHR final : public PageElement {
  int16_t width;

 public:
  PageHR(const int16_t xPos, const int16_t yPos, const int16_t width) : PageElement(xPos, yPos), width(width) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_PageHR; }
  static std::unique_ptr<PageHR> deserialize(FsFile& file);
};

struct TableCell {
  std::vector<std::shared_ptr<TextBlock>> lines;
  std::shared_ptr<ImageBlock> image;  // optional in-cell graphic, drawn below any cell text
  bool isHeader = false;
  // Number of grid columns this cell occupies. A row's spans always sum to the fragment's
  // columnCount (layout pads short rows), so the renderer can walk cells and accumulate.
  uint8_t colSpan = 1;
};

struct TableRow {
  std::vector<TableCell> cells;
  uint16_t height = 0;       // pixel height including 2×CELL_PADDING
  bool isHeaderRow = false;  // drives 2px separator below this row
};

class PageTableFragment final : public PageElement {
  uint8_t columnCount = 0;
  uint16_t totalWidth = 0;
  uint16_t totalHeight = 0;
  std::vector<TableRow> rows;
  bool hasBorder = true;

 public:
  PageTableFragment(uint8_t colCount, uint16_t totalWidth, uint16_t totalHeight, std::vector<TableRow> rows,
                    int16_t xPos, int16_t yPos, bool hasBorder = true)
      : PageElement(xPos, yPos),
        columnCount(colCount),
        totalWidth(totalWidth),
        totalHeight(totalHeight),
        rows(std::move(rows)),
        hasBorder(hasBorder) {}

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(FsFile& file) override;
  static std::unique_ptr<PageTableFragment> deserialize(FsFile& file);
  PageElementTag getTag() const override { return TAG_PageTable; }
  uint16_t getTotalHeight() const { return totalHeight; }
  uint16_t getTotalWidth() const { return totalWidth; }
  uint8_t getColumnCount() const { return columnCount; }
  bool getHasBorder() const { return hasBorder; }
  // Read-only view of the laid-out cells. Exists for the pipeline dump: without it no golden can
  // see table text, which is how a cell-truncation bug once survived a full corpus of goldens.
  const std::vector<TableRow>& getRows() const { return rows; }

  // In-cell graphics participate in the Page-level image passes below.
  bool hasImages() const;
  bool hasUncachedImages(bool forceLoad, bool monochromeOutput, bool alsoWarmGrayscale = false) const;
  // Decode any missing cell-image pixel caches. Position is irrelevant to the cache
  // (it is position-independent); images are warmed at the origin and the framebuffer
  // garbage is discarded by the caller's clearScreen(), mirroring the Page warm pass.
  void warmCellImages(GfxRenderer& renderer, bool forceLoad, bool monochromeOutput,
                      bool alsoWarmGrayscale = false) const;
};

class Page {
 public:
  // the list of block index and line numbers on this page
  std::vector<std::shared_ptr<PageElement>> elements;
  std::vector<FootnoteEntry> footnotes;
  static constexpr uint16_t MAX_FOOTNOTES_PER_PAGE = 16;

  void addFootnote(const char* number, const char* href) {
    if (footnotes.size() >= MAX_FOOTNOTES_PER_PAGE) return;  // Cap per-page footnotes
    FootnoteEntry entry;
    strncpy(entry.number, number, sizeof(entry.number) - 1);
    entry.number[sizeof(entry.number) - 1] = '\0';
    strncpy(entry.href, href, sizeof(entry.href) - 1);
    entry.href[sizeof(entry.href) - 1] = '\0';
    footnotes.push_back(entry);
  }

  // monochromeOutput=true: 1-bit Atkinson BW cache (AA off); false: 4-level Bayer cache (AA on)
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool forceLoadLargeImages = true,
              bool monochromeOutput = true) const;
  // Renders every non-image element. With abortable=true the element loop polls
  // CooperativeAbort::shouldAbortLongTask() and stops early, returning false; otherwise it
  // always runs to completion and returns true.
  //
  // Only the anti-aliasing plane renders may pass true. The prewarm SCAN passes must never
  // abort: they exist so the following replay finds every glyph bitmap resident, and a partial
  // scan leaves the replay dereferencing a metadata-only entry (null bitmap base). Nor may the
  // pre-render pass abort — a half-drawn look-ahead page still gets marked ready and would be
  // flushed to the panel by the next buffer-display turn.
  bool renderTextOnly(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool abortable = false) const;
  // Replay the 4-level grayscale image cache into the current renderer mode (GRAYSCALE_LSB / MSB).
  // Called by AA grayscale passes after renderTextOnly() so images get proper gray tones.
  // No-op for images without a grayscale cache (they degrade gracefully to BW-only).
  void renderImagesFromGrayscaleCache(GfxRenderer& renderer, int xOffset, int yOffset) const;
  // Decode any missing .pxc pixel caches for images on this page. Called before the
  // BW render so the large (~60 KB contiguous) PNG decoder allocation runs while heap
  // contig is at its peak — before font prewarm and BW backup chunks fragment it.
  // Writes pixels to the framebuffer as a side effect (decoder requirement); callers
  // must clearScreen() afterward if the framebuffer needs to be clean.
  // monochromeOutput selects which cache variant to warm (BW or grayscale).
  // alsoWarmGrayscale additionally warms the 4-level Bayer cache that the AA grayscale
  // planes replay on top of the BW frame. Both variants are needed with AA on: the BW
  // plane draws 1-bit Atkinson, the gray planes lift levels 1/2 back to real greys.
  void warmImageCaches(GfxRenderer& renderer, int xOffset, int yOffset, bool forceLoadLargeImages,
                       bool monochromeOutput = true, bool alsoWarmGrayscale = false) const;
  bool hasPlaceholderImages(bool forceLoadLargeImages, bool monochromeOutput) const;
  bool allImagesArePlaceholders(bool forceLoadLargeImages, bool monochromeOutput) const;
  bool serialize(FsFile& file) const;
  static std::unique_ptr<Page> deserialize(FsFile& file);

  // Check if page contains any images (used to force full refresh)
  bool hasImages() const {
    return std::any_of(elements.begin(), elements.end(), [](const std::shared_ptr<PageElement>& el) {
      if (el->getTag() == TAG_PageImage) return true;
      return el->getTag() == TAG_PageTable && static_cast<const PageTableFragment&>(*el).hasImages();
    });
  }

  // Returns true if any image on this page would require a decoder allocation —
  // i.e. is not a placeholder AND does not already have a pixel cache on disk.
  // Used to decide whether to release the secondary frame buffer before warm.
  // alsoWarmGrayscale mirrors warmImageCaches(): a missing .bayer.pxc is decode work too,
  // so it must count here or the secondary buffer stays allocated through that decode.
  bool hasUncachedImages(bool forceLoadLargeImages, bool monochromeOutput, bool alsoWarmGrayscale = false) const {
    return std::any_of(elements.begin(), elements.end(), [&](const std::shared_ptr<PageElement>& el) {
      if (el->getTag() == TAG_PageTable)
        return static_cast<const PageTableFragment&>(*el).hasUncachedImages(forceLoadLargeImages, monochromeOutput,
                                                                            alsoWarmGrayscale);
      if (el->getTag() != TAG_PageImage) return false;
      const auto& ib = static_cast<const PageImage&>(*el).getImageBlock();
      if (ib.wouldShowPlaceholder(forceLoadLargeImages, monochromeOutput)) return false;
      if (!(monochromeOutput ? ib.hasPixelCache() : ib.hasGrayscaleCache())) return true;
      return alsoWarmGrayscale && monochromeOutput && !ib.hasGrayscaleCache();
    });
  }

  // Get bounding box of all images on the page (union of image rects)
  // Returns false if no images. Coordinates are relative to page origin.
  bool getImageBoundingBox(int16_t& outX, int16_t& outY, int16_t& outW, int16_t& outH) const {
    bool found = false;
    int16_t minX = INT16_MAX, minY = INT16_MAX, maxX = INT16_MIN, maxY = INT16_MIN;
    for (const auto& el : elements) {
      if (el->getTag() == TAG_PageImage) {
        const auto& img = static_cast<const PageImage&>(*el);
        int16_t x = img.xPos;
        int16_t y = img.yPos;
        int16_t right = x + img.getImageBlock().getWidth();
        int16_t bottom = y + img.getImageBlock().getRenderedHeight();
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, right);
        maxY = std::max(maxY, bottom);
        found = true;
      }
    }
    if (found) {
      outX = minX;
      outY = minY;
      outW = maxX - minX;
      outH = maxY - minY;
    }
    return found;
  }
};
