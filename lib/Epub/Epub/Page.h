#pragma once
#include <HalStorage.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class BuildArena;  // lib/Memory -- optional storage for TextBlock bytes on deserialize
namespace serialization {
class BufferedFileWriter;  // lib/Serialization -- where a block writes itself
}

#include "FootnoteEntry.h"
#include "blocks/ImageBlock.h"
#include "blocks/TextBlock.h"
#include "css/CssStyle.h"

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
  TAG_PageBox = 5,
};

// represents something that has been added to a page
class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;
  explicit PageElement(const int16_t xPos, const int16_t yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) = 0;
  virtual bool serialize(serialization::BufferedFileWriter& out) = 0;
  virtual PageElementTag getTag() const = 0;  // Add type identification
};

// a line from a block element
class PageLine final : public PageElement {
  std::unique_ptr<TextBlock> block;

 public:
  PageLine(std::unique_ptr<TextBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  const TextBlock* getBlock() const { return block.get(); }
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(serialization::BufferedFileWriter& out) override;
  PageElementTag getTag() const override { return TAG_PageLine; }
  static std::unique_ptr<PageLine> deserialize(FsFile& file, BuildArena* scratch = nullptr);
};

// New PageImage class
class PageImage final : public PageElement {
  std::unique_ptr<ImageBlock> imageBlock;
  // Set in a line of text (an inline image), not a block of its own.
  bool inLine_ = false;
  // The internal link the image is inside (linkHrefHash of its href); 0: none.
  uint32_t linkHash_ = 0;
  // The serialized flags byte.
  static constexpr uint8_t kInLine = 0x01;
  static constexpr uint8_t kLinked = 0x02;

 public:
  PageImage(std::unique_ptr<ImageBlock> block, const int16_t xPos, const int16_t yPos, const bool inLine = false,
            const uint32_t linkHash = 0)
      : PageElement(xPos, yPos), imageBlock(std::move(block)), inLine_(inLine), linkHash_(linkHash) {}
  bool isInLine() const { return inLine_; }
  uint32_t linkHash() const { return linkHash_; }
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  void renderWithForceLoad(GfxRenderer& renderer, int xOffset, int yOffset, bool forceLoad,
                           bool monochromeOutput = true, bool alsoCacheOtherVariant = false);
  // The image's gray variant (ImageBlock::GrayVariant), decoded and cached where render() draws it.
  void warmGrayVariant(GfxRenderer& renderer, int xOffset, int yOffset, bool forceLoad);
  bool serialize(serialization::BufferedFileWriter& out) override;
  PageElementTag getTag() const override { return TAG_PageImage; }
  static std::unique_ptr<PageImage> deserialize(FsFile& file);
  const ImageBlock& getImageBlock() const { return *imageBlock; }
};

class PageHR final : public PageElement {
  int16_t width;

 public:
  PageHR(const int16_t xPos, const int16_t yPos, const int16_t width) : PageElement(xPos, yPos), width(width) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(serialization::BufferedFileWriter& out) override;
  PageElementTag getTag() const override { return TAG_PageHR; }
  static std::unique_ptr<PageHR> deserialize(FsFile& file);
};

// What a page's pictures came to on the gray canvas (Page::renderImagesGray8).
struct Gray8Pictures {
  uint16_t offered = 0;  // pictures on the page, in-cell graphics included
  uint16_t inGray = 0;   // of those, the ones whose gray is on the canvas; the rest keep what the B/W pass drew
};

// The border and background of a block's slice on this page, as CSS draws them (CssBox): the parser's box scope places
// one before the content it frames, so the shade goes under it. A box a page break cuts is left open there: no bottom
// edge before the break, no top edge after it. Drawn in the black-and-white pass only.
// Ported from crosspoint-reader PR #3891 (Justin Mitchell, @itsthisjustin), its PageBorderBox.
class PageBox final : public PageElement {
 public:
  struct Edge {
    uint8_t width = 0;  // pixels; 0 draws nothing
    CssBorderStyle style = CssBorderStyle::None;
  };
  // Edges top, right, bottom, left.
  PageBox(const int16_t xPos, const int16_t yPos, const uint16_t width, const uint16_t height, const Edge (&edges)[4],
          const bool shaded)
      : PageElement(xPos, yPos),
        width(width),
        height(height),
        edges{edges[0], edges[1], edges[2], edges[3]},
        shaded(shaded) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(serialization::BufferedFileWriter& out) override;
  PageElementTag getTag() const override { return TAG_PageBox; }
  static std::unique_ptr<PageBox> deserialize(FsFile& file);
  uint16_t getWidth() const { return width; }
  uint16_t getHeight() const { return height; }
  const Edge& getEdge(const CssBox::Side side) const { return edges[side]; }
  bool isShaded() const { return shaded; }

 private:
  uint16_t width;
  uint16_t height;
  Edge edges[4];
  bool shaded;
};

struct TableCell {
  std::vector<std::unique_ptr<TextBlock>> lines;
  std::unique_ptr<ImageBlock> image;  // optional in-cell graphic, drawn below any cell text
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
  bool serialize(serialization::BufferedFileWriter& out) override;
  static std::unique_ptr<PageTableFragment> deserialize(FsFile& file, BuildArena* scratch = nullptr);
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
  // False when the page's warm ends here: a decode stopped for input, or one refused for memory beside a section build
  // (see Page::warmImageCaches).
  bool warmCellImages(GfxRenderer& renderer, bool forceLoad, bool monochromeOutput,
                      bool alsoWarmGrayscale = false) const;
  // Page::renderImagesGray8 for the in-cell graphics, each where render() draws it.
  void renderImagesGray8(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, uint8_t* canvas, uint16_t stride,
                         Gray8Pictures& count) const;
};

class Page {
 public:
  // the list of block index and line numbers on this page
  std::vector<std::unique_ptr<PageElement>> elements;
  // What the parser reserves for a fresh page: a text page holds ~28 lines plus the odd image
  // or rule. Left to grow by doubling, `elements` cost eight allocations per page on the host
  // census (memory audit 2026-09, R2); a denser page still grows past this normally.
  static constexpr size_t TYPICAL_ELEMENTS = 32;
  std::vector<FootnoteEntry> footnotes;
  // A page of a vertical-rl section: the block extent its columns fill, the reader's viewport width; 0 on a
  // horizontal page. Such a page draws in the turned frame (spec 5.2).
  uint16_t verticalBlockExtent = 0;
  bool isVertical() const { return verticalBlockExtent != 0; }
  // Footnote links kept per page. 16 -> 64 (memory audit 2026-09, R4): an endnotes page can
  // carry a link per line, and the 17th used to be dropped without a word. A FootnoteEntry is
  // 128 B, so a full page costs 8 KB on load -- only pages that have that many links pay it.
  // The parser reports a refusal (addFootnote returns false) as a cap overflow, which ends up
  // in the section's status byte.
  static constexpr uint16_t MAX_FOOTNOTES_PER_PAGE = 64;
  // Load-side sanity bound on the element count, and now the build-side bound too: a page is
  // laid out by height, so a real page never comes near it (~28 lines, plus images and rules).
  // Page::serialize clamps to it and Section::onPageComplete reports the overflow, so a page
  // can no longer be built that cannot be loaded (audit §4.2).
  static constexpr uint16_t MAX_ELEMENTS = 1024;
  // Load-side sanity bound on a section's page count (Section::loadSectionFile), applied at
  // build time too: ChapterHtmlSlimParser stops the parse at this many pages with the truncated
  // status, so a monster spine is cached once instead of rebuilt on every open (audit §4.2).
  static constexpr uint16_t MAX_PAGES_PER_SECTION = 10000;

  // False when the page already holds MAX_FOOTNOTES_PER_PAGE entries (the link is dropped).
  bool addFootnote(const char* number, const char* href) {
    if (footnotes.size() >= MAX_FOOTNOTES_PER_PAGE) return false;
    FootnoteEntry entry;
    strncpy(entry.number, number, sizeof(entry.number) - 1);
    entry.number[sizeof(entry.number) - 1] = '\0';
    strncpy(entry.href, href, sizeof(entry.href) - 1);
    entry.href[sizeof(entry.href) - 1] = '\0';
    footnotes.push_back(entry);
    return true;
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
  // On a panel that keeps a deeper gray buffer of its own: each picture of the page, in-cell graphics included, as its
  // 8-bit samples into that buffer (`canvas`, `stride` bytes a row) where render() draws it
  // (ImageBlock::renderGray8). Run after render(), whose B/W pass is what a picture without its gray keeps.
  Gray8Pictures renderImagesGray8(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, uint8_t* canvas,
                                  uint16_t stride) const;
  // Decode any missing .pxc pixel caches for images on this page. Called before the
  // BW render so the large (~60 KB contiguous) PNG decoder allocation runs while heap
  // contig is at its peak — before font prewarm and BW backup chunks fragment it.
  // Writes pixels to the framebuffer as a side effect (decoder requirement); callers
  // must clearScreen() afterward if the framebuffer needs to be clean.
  // monochromeOutput selects which cache variant to warm (BW or grayscale).
  // alsoWarmGrayscale additionally warms the 4-level Bayer cache that the AA grayscale
  // planes replay on top of the BW frame. Both variants are needed with AA on: the BW
  // plane draws 1-bit Atkinson, the gray planes lift levels 1/2 back to real greys.
  // redecodeCoarse: a .pxc stamped coarse counts as missing, and stays unless a decode replaces it
  // (ImageBlock::CoarseRedecodeScope).
  // Beside a section build (image_scratch::canWait) the first image refused for memory ends the warm: the images after
  // it wait with it for the lane after the build.
  void warmImageCaches(GfxRenderer& renderer, int xOffset, int yOffset, bool forceLoadLargeImages,
                       bool monochromeOutput = true, bool alsoWarmGrayscale = false, bool redecodeCoarse = false) const;
  bool hasPlaceholderImages(bool forceLoadLargeImages, bool monochromeOutput) const;
  bool allImagesArePlaceholders(bool forceLoadLargeImages, bool monochromeOutput) const;
  bool serialize(serialization::BufferedFileWriter& out) const;
  // `scratch`: every TextBlock on the page takes its bytes from it when given (see
  // TextBlock::deserialize); the page must then die before the caller's arena block does.
  static std::unique_ptr<Page> deserialize(FsFile& file, BuildArena* scratch = nullptr);

  // Check if page contains any images (used to force full refresh)
  bool hasImages() const {
    return std::any_of(elements.begin(), elements.end(), [](const std::unique_ptr<PageElement>& el) {
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
    return std::any_of(elements.begin(), elements.end(), [&](const std::unique_ptr<PageElement>& el) {
      if (el->getTag() == TAG_PageTable)
        return static_cast<const PageTableFragment&>(*el).hasUncachedImages(forceLoadLargeImages, monochromeOutput,
                                                                            alsoWarmGrayscale);
      if (el->getTag() != TAG_PageImage) return false;
      const auto& ib = static_cast<const PageImage&>(*el).getImageBlock();
      if (ib.wouldShowPlaceholder(forceLoadLargeImages, monochromeOutput)) return false;
      if (!(monochromeOutput ? ib.hasPixelCache() : ib.hasGrayscaleCache())) return true;
      return alsoWarmGrayscale && monochromeOutput && !ib.hasGrayVariantCache();
    });
  }

  // True when the page has elements and every one is an image of its own: an image set in a line of text is that
  // line's, and a page that ends on such a line is not a picture page.
  bool isImageOnly() const {
    return !elements.empty() &&
           std::all_of(elements.begin(), elements.end(), [](const std::unique_ptr<PageElement>& element) {
             return element && element->getTag() == TAG_PageImage &&
                    !static_cast<const PageImage&>(*element).isInLine();
           });
  }

  // How far down an image-only page's images move to sit centred in `extent`; 0 when the page has other elements, no
  // image, or an image as tall as the extent.
  int imageOnlyCentringOffset(const int extent) const {
    int16_t imgX, imgY, imgW, imgH;
    if (!isImageOnly() || !getImageBoundingBox(imgX, imgY, imgW, imgH) || imgH >= extent) return 0;
    return std::max(0, (extent - imgH) / 2 - imgY);
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
