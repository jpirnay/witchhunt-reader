#include "Lyra3CoversTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "UiFontScale.h"
#include "components/BookProgressPresentation.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "components/themes/TapTargets.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
}  // namespace

void Lyra3CoversTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = (rect.width - 2 * Lyra3CoversMetrics::values.contentSidePadding) / 3;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  const int maxLineWidth = tileWidth - 2 * hPaddingInSelection;
  const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  // A row held back under the title for the compact reading history. Reserved whether or not a
  // given book has one, so the three covers stay the same size as the selection moves across
  // them. The non-scaling small face keeps that reservation honest at any UI font size.
  const int historyLineHeight = renderer.getLineHeight(FIT_SMALL_FONT_ID);

  const int tileCount = hasContinueReading ? std::min(static_cast<int>(recentBooks.size()),
                                                      Lyra3CoversMetrics::values.homeRecentBooksCount)
                                           : 0;

  // Both the cover height and the block under it come from Lyra3CoversMetrics, because
  // HomeActivity has to arrive at the same cover height to name the thumbnail file it writes.
  const int coverHeight = Lyra3CoversMetrics::coverRenderHeight(rect.height);
  const int textBoxHeight = rect.height - hPaddingInSelection - coverHeight;

  // Frame the cover at the proportions a cover actually has, not at the full width of the tile.
  //
  // Thumbnails are generated 6:10, so a cover this tall is only 0.6 * coverHeight wide. The frame
  // used to span the whole tile regardless, which was invisible while covers were tall enough to
  // overflow it and get cropped, and became a band of white beside every cover once the block
  // under them grew. Take the narrower of the two and centre it: a correctly proportioned cover
  // in the middle of the tile reads as deliberate, one in the corner of a wide box reads broken.
  const int coverBoxWidth = std::min(tileWidth - 2 * hPaddingInSelection, coverHeight * 6 / 10);
  const int coverBoxInset = (tileWidth - 2 * hPaddingInSelection - coverBoxWidth) / 2;

  // Wrap each title to the number of lines the block can actually hold rather than to three
  // regardless. Three lines of a large UI font do not fit under a cover sized for them plus the
  // history row, and the lines that did not fit were printing over the menu below.
  const int titleAreaHeight = textBoxHeight - historyLineHeight - 5;
  const int titleLineBudget = std::max(
      1, std::min(Lyra3CoversMetrics::titleLineBudget, titleLineHeight > 0 ? titleAreaHeight / titleLineHeight : 1));
  std::vector<std::vector<std::string>> titleLinesPerTile;
  titleLinesPerTile.reserve(static_cast<size_t>(tileCount));
  for (int i = 0; i < tileCount; i++) {
    titleLinesPerTile.push_back(
        renderer.wrappedText(SMALL_FONT_ID, recentBooks[i].title.c_str(), maxLineWidth, titleLineBudget));
  }

  // The three tiles published for touch. Recorded ahead of the draw loop, which is skipped on a
  // cached repaint (coverRendered) while the geometry above is recomputed every call. Values are
  // book indices, matching how HomeActivity numbers the covers in its selector.
  {
    TapTargets::Recorder::Builder coverTargets;
    for (int i = 0; i < tileCount; i++) {
      const int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;
      coverTargets.add(tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,
                       coverHeight, i);
    }
    TapTargets::homeCovers().record(coverTargets);
  }

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    if (!coverRendered) {
      bool anyPending = false;
      for (int i = 0;
           i < std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount); i++) {
        std::string coverPath = recentBooks[i].coverBmpPath;
        bool hasCover = true;
        bool tilePending = false;
        int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;
        if (coverPath.empty()) {
          hasCover = false;
        } else {
          const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, coverHeight);

          // First time: load cover from SD and render
          FsFile file;
          if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            // Never draw the no-cover marker: scaling a 1x1 BMP into the slot paints a solid block.
            // Falling through to the existing no-cover branch shows the theme's own tile instead.
            if (bitmap.parseHeaders() == BmpReaderError::Ok &&
                !UITheme::isCoverPlaceholderBmp(bitmap.getWidth(), bitmap.getHeight())) {
              const float bitmapHeight = static_cast<float>(bitmap.getHeight());
              const float bitmapWidth = static_cast<float>(bitmap.getWidth());
              const float ratio = bitmapWidth / bitmapHeight;
              const float tileRatio = static_cast<float>(coverBoxWidth) / static_cast<float>(coverHeight);
              const float cropX = std::max(0.0f, 1.0f - (tileRatio / ratio));

              // Clear tile to white before drawing: 1-bit BMPs only draw dark pixels,
              // leaving white pixels transparent — any stale dark content shows through.
              renderer.fillRect(tileX + hPaddingInSelection + coverBoxInset, tileY + hPaddingInSelection, coverBoxWidth,
                                coverHeight, false);
              renderer.drawBitmap(bitmap, tileX + hPaddingInSelection + coverBoxInset, tileY + hPaddingInSelection,
                                  coverBoxWidth, coverHeight, cropX);
            } else {
              hasCover = false;
            }
            file.close();
          } else {
            hasCover = false;
            tilePending = true;  // path exists but BMP not ready yet
            anyPending = true;
          }
        }
        // Draw either way
        renderer.drawRect(tileX + hPaddingInSelection + coverBoxInset, tileY + hPaddingInSelection, coverBoxWidth,
                          coverHeight, true);

        if (!hasCover) {
          if (tilePending) {
            // Cover is being generated — show a loading label centred in the tile
            const char* loadingText = tr(STR_LOADING);
            const int textW = renderer.getTextWidth(SMALL_FONT_ID, loadingText);
            const int textH = renderer.getLineHeight(SMALL_FONT_ID);
            renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection + coverBoxInset + (coverBoxWidth - textW) / 2,
                              tileY + hPaddingInSelection + (coverHeight - textH) / 2, loadingText, true);
          } else {
            // No cover at all — render empty cover placeholder
            renderer.fillRect(tileX + hPaddingInSelection + coverBoxInset,
                              tileY + hPaddingInSelection + (coverHeight / 3), coverBoxWidth, 2 * coverHeight / 3,
                              true);
            renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + coverBoxInset + (coverBoxWidth - 32) / 2,
                              tileY + hPaddingInSelection + 24, 32, 32);
          }
        }
      }

      // Only cache the frame buffer once all tiles are definitively resolved.
      // If any cover is still being generated we keep coverRendered=false so the next render will retry.
      if (!anyPending) {
        coverBufferStored = storeCoverBuffer();
        coverRendered = coverBufferStored;
      }
    }

    for (int i = 0; i < tileCount; i++) {
      bool bookSelected = (selectorIndex == i);
      const int progressPercent = getRecentBookProgressPercent(recentBooks[i]);

      int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;

      const auto& titleLines = titleLinesPerTile[static_cast<size_t>(i)];

      // A third of the screen wide is no room for a sentence, so this layout gets the short form
      // -- time in the book and the number of days it spans, nothing else.
      const std::string history = BookProgressPresentation::historyLineCompact(recentBooks[i]);

      // One height for all three boxes, from the tallest title, so the row reads as a row rather
      // than three cards of different depths.
      const int dynamicTitleBoxHeight = textBoxHeight;

      if (bookSelected) {
        // Draw selection box
        renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                                 Color::LightGray);
        renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection, coverHeight, Color::LightGray);
        renderer.fillRectDither(tileX + tileWidth - hPaddingInSelection, tileY + hPaddingInSelection,
                                hPaddingInSelection, coverHeight, Color::LightGray);
        renderer.fillRoundedRect(tileX, tileY + coverHeight + hPaddingInSelection, tileWidth, dynamicTitleBoxHeight,
                                 cornerRadius, false, false, true, true, Color::LightGray);
      }

      BookProgressPresentation::drawIndicator(
          static_cast<const GfxRenderer&>(renderer),
          Rect{tileX + hPaddingInSelection + coverBoxInset, tileY + hPaddingInSelection, coverBoxWidth, coverHeight},
          progressPercent);

      int currentY = tileY + coverHeight + hPaddingInSelection + 5;
      for (const auto& line : titleLines) {
        renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection, currentY, line.c_str(), true);
        currentY += titleLineHeight;
      }
      if (!history.empty()) {
        const std::string historyTrunc = renderer.truncatedText(FIT_SMALL_FONT_ID, history.c_str(), maxLineWidth);
        renderer.drawText(FIT_SMALL_FONT_ID, tileX + hPaddingInSelection, currentY, historyTrunc.c_str(), true);
      }
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}
