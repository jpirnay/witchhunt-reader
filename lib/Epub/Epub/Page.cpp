#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
}

bool PageLine::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto tb = TextBlock::deserialize(file);
  return std::unique_ptr<PageLine>(new PageLine(std::move(tb), xPos, yPos));
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
}

void PageImage::renderWithForceLoad(GfxRenderer& renderer, const int xOffset, const int yOffset, const bool forceLoad,
                                    const bool monochromeOutput) {
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset, forceLoad, monochromeOutput);
}

bool PageImage::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto ib = ImageBlock::deserialize(file);
  return std::unique_ptr<PageImage>(new PageImage(std::move(ib), xPos, yPos));
}

void PageHR::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  renderer.drawLine(xPos + xOffset, yPos + yOffset, xPos + xOffset + width - 1, yPos + yOffset);
}

bool PageHR::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, width);
  return true;
}

std::unique_ptr<PageHR> PageHR::deserialize(FsFile& file) {
  int16_t xPos, yPos, width;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);
  serialization::readPod(file, width);
  return std::unique_ptr<PageHR>(new PageHR(xPos, yPos, width));
}

void PageTableFragment::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  const int drawX = xPos + xOffset;
  const int drawY = yPos + yOffset;

  if (hasBorder) {
    renderer.drawRect(drawX, drawY, totalWidth, totalHeight, true);
  }

  // Pre-compute column X positions using integer division to avoid rounding drift across columns.
  // Idea from uxjulia/CrossInk; rewritten for our layout model.
  std::array<int, MAX_TABLE_COLS + 1> colX = {};
  colX[0] = drawX;
  for (uint8_t c = 0; c < columnCount; c++) {
    colX[c + 1] = drawX + static_cast<int>((static_cast<uint32_t>(totalWidth) * (c + 1)) / columnCount);
  }

  if (hasBorder) {
    for (uint8_t c = 1; c < columnCount; c++) {
      renderer.drawLine(colX[c], drawY, colX[c], drawY + totalHeight - 1, true);
    }
  }

  // Rows: text content + horizontal separators
  int rowY = drawY;
  for (size_t r = 0; r < rows.size(); r++) {
    const TableRow& row = rows[r];
    for (uint8_t c = 0; c < columnCount && c < static_cast<uint8_t>(row.cells.size()); c++) {
      const TableCell& cell = row.cells[c];
      int lineY = rowY + TABLE_CELL_PADDING;
      for (const auto& line : cell.lines) {
        line->render(renderer, fontId, colX[c] + TABLE_CELL_PADDING, lineY);
        lineY += renderer.getLineHeight(fontId);
      }
      // In-cell graphic, drawn below the cell text. Always 1-bit (BW) thumbnails:
      // the decode/cache machinery lives in ImageBlock; the BW cache is built during
      // the warm pass and simply blitted here (and again in grayscale strip passes).
      if (cell.image) {
        cell.image->render(renderer, colX[c] + TABLE_CELL_PADDING, lineY, /*forceLoad=*/true,
                           /*monochromeOutput=*/true);
      }
    }
    rowY += row.height;
    // Draw horizontal separator between rows (skip after last row)
    if (hasBorder && r + 1 < rows.size()) {
      const int sepLineWidth = row.isHeaderRow ? 2 : 1;
      renderer.drawLine(drawX, rowY, drawX + totalWidth - 1, rowY, sepLineWidth, true);
    }
  }
}

bool PageTableFragment::hasImages() const {
  for (const auto& row : rows) {
    for (const auto& cell : row.cells) {
      if (cell.image) return true;
    }
  }
  return false;
}

bool PageTableFragment::hasUncachedImages(const bool forceLoad, const bool monochromeOutput) const {
  for (const auto& row : rows) {
    for (const auto& cell : row.cells) {
      if (!cell.image) continue;
      if (cell.image->wouldShowPlaceholder(forceLoad, monochromeOutput)) continue;
      const bool cached = monochromeOutput ? cell.image->hasPixelCache() : cell.image->hasGrayscaleCache();
      if (!cached) return true;
    }
  }
  return false;
}

void PageTableFragment::warmCellImages(GfxRenderer& renderer, const bool forceLoad, const bool monochromeOutput) const {
  for (const auto& row : rows) {
    for (const auto& cell : row.cells) {
      if (!cell.image) continue;
      if (cell.image->wouldShowPlaceholder(forceLoad, monochromeOutput)) continue;
      const bool cached = monochromeOutput ? cell.image->hasPixelCache() : cell.image->hasGrayscaleCache();
      if (cached) continue;
      // The decode is the heap-hungry work; do it now (caller has freed the secondary buffer).
      // The cache is position-independent, so warm at the origin — the framebuffer write is
      // discarded by the caller's clearScreen().
      cell.image->render(renderer, 0, 0, forceLoad, monochromeOutput);
    }
  }
}

bool PageTableFragment::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, columnCount);
  serialization::writePod(file, totalWidth);
  serialization::writePod(file, totalHeight);
  serialization::writePod(file, hasBorder);
  const uint16_t rowCount = static_cast<uint16_t>(rows.size());
  serialization::writePod(file, rowCount);
  for (const auto& row : rows) {
    serialization::writePod(file, row.height);
    serialization::writePod(file, row.isHeaderRow);
    const uint8_t cellCount = static_cast<uint8_t>(row.cells.size());
    serialization::writePod(file, cellCount);
    for (const auto& cell : row.cells) {
      serialization::writePod(file, cell.isHeader);
      const uint8_t lineCount = static_cast<uint8_t>(cell.lines.size());
      serialization::writePod(file, lineCount);
      for (const auto& line : cell.lines) {
        if (!line->serialize(file)) return false;
      }
      const bool hasImage = static_cast<bool>(cell.image);
      serialization::writePod(file, hasImage);
      if (hasImage && !cell.image->serialize(file)) return false;
    }
  }
  return true;
}

std::unique_ptr<PageTableFragment> PageTableFragment::deserialize(FsFile& file) {
  int16_t xPos, yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  uint8_t columnCount;
  uint16_t totalWidth, totalHeight;
  serialization::readPod(file, columnCount);
  serialization::readPod(file, totalWidth);
  serialization::readPod(file, totalHeight);

  if (columnCount == 0 || columnCount > MAX_TABLE_COLS) {
    LOG_ERR("PGE", "TableFragment: invalid columnCount %u", columnCount);
    return nullptr;
  }

  bool hasBorder;
  serialization::readPod(file, hasBorder);

  uint16_t rowCount;
  serialization::readPod(file, rowCount);
  if (rowCount > MAX_TABLE_ROWS) {
    LOG_ERR("PGE", "TableFragment: invalid rowCount %u", rowCount);
    return nullptr;
  }

  std::vector<TableRow> rows;
  rows.reserve(rowCount);
  for (uint16_t r = 0; r < rowCount; r++) {
    TableRow row;
    serialization::readPod(file, row.height);
    serialization::readPod(file, row.isHeaderRow);
    uint8_t cellCount;
    serialization::readPod(file, cellCount);
    if (cellCount > MAX_TABLE_COLS) {
      LOG_ERR("PGE", "TableFragment: invalid cellCount %u in row %u", cellCount, r);
      return nullptr;
    }
    row.cells.reserve(cellCount);
    for (uint8_t c = 0; c < cellCount; c++) {
      TableCell cell;
      serialization::readPod(file, cell.isHeader);
      uint8_t lineCount;
      serialization::readPod(file, lineCount);
      if (lineCount > MAX_CELL_LINES) {
        LOG_ERR("PGE", "TableFragment: invalid lineCount %u at row %u cell %u", lineCount, r, c);
        return nullptr;
      }
      cell.lines.reserve(lineCount);
      for (uint8_t l = 0; l < lineCount; l++) {
        auto tb = TextBlock::deserialize(file);
        if (!tb) {
          LOG_ERR("PGE", "TableFragment: TextBlock deserialize failed at row %u cell %u line %u", r, c, l);
          return nullptr;
        }
        cell.lines.push_back(std::move(tb));
      }
      bool hasImage = false;
      serialization::readPod(file, hasImage);
      if (hasImage) {
        cell.image = ImageBlock::deserialize(file);
        if (!cell.image) {
          LOG_ERR("PGE", "TableFragment: ImageBlock deserialize failed at row %u cell %u", r, c);
          return nullptr;
        }
      }
      row.cells.push_back(std::move(cell));
    }
    rows.push_back(std::move(row));
  }

  return std::unique_ptr<PageTableFragment>(
      new PageTableFragment(columnCount, totalWidth, totalHeight, std::move(rows), xPos, yPos, hasBorder));
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                  const bool forceLoadLargeImages, const bool monochromeOutput) const {
  for (auto& element : elements) {
    if (element->getTag() == TAG_PageImage) {
      static_cast<PageImage&>(*element).renderWithForceLoad(renderer, xOffset, yOffset, forceLoadLargeImages,
                                                            monochromeOutput);
    } else {
      element->render(renderer, fontId, xOffset, yOffset);
    }
  }
}

void Page::renderImagesFromGrayscaleCache(GfxRenderer& renderer, const int xOffset, const int yOffset) const {
  for (const auto& element : elements) {
    if (element->getTag() != TAG_PageImage) continue;
    const auto& pi = static_cast<const PageImage&>(*element);
    const auto& ib = pi.getImageBlock();
    ib.renderGrayscaleFromCache(renderer, pi.xPos + xOffset, pi.yPos + yOffset);
  }
}

void Page::warmImageCaches(GfxRenderer& renderer, const int xOffset, const int yOffset, const bool forceLoadLargeImages,
                           const bool monochromeOutput) const {
  // Only do the costly decode pass when there's at least one image that would
  // actually require a PNG/JPG decoder allocation. Cached and placeholder paths
  // do not need the contiguous heap headroom, so skipping the iteration entirely
  // saves the no-op overhead on text-only pages (the common case).
  for (auto& element : elements) {
    if (element->getTag() == TAG_PageTable) {
      static_cast<const PageTableFragment&>(*element).warmCellImages(renderer, forceLoadLargeImages, monochromeOutput);
      continue;
    }
    if (element->getTag() != TAG_PageImage) continue;
    const auto& ib = static_cast<const PageImage&>(*element).getImageBlock();
    if (ib.wouldShowPlaceholder(forceLoadLargeImages, monochromeOutput)) continue;
    // Check whether the appropriate cache already exists
    const bool alreadyCached = monochromeOutput ? ib.hasPixelCache() : ib.hasGrayscaleCache();
    if (alreadyCached) continue;
    static_cast<PageImage&>(*element).renderWithForceLoad(renderer, xOffset, yOffset, forceLoadLargeImages,
                                                          monochromeOutput);
  }
}

bool Page::hasPlaceholderImages(const bool forceLoadLargeImages, const bool monochromeOutput) const {
  for (const auto& el : elements) {
    if (el->getTag() == TAG_PageImage) {
      if (static_cast<const PageImage&>(*el).getImageBlock().wouldShowPlaceholder(forceLoadLargeImages,
                                                                                  monochromeOutput)) {
        return true;
      }
    }
  }
  return false;
}

bool Page::allImagesArePlaceholders(const bool forceLoadLargeImages, const bool monochromeOutput) const {
  bool anyImage = false;
  for (const auto& el : elements) {
    if (el->getTag() == TAG_PageImage) {
      anyImage = true;
      if (!static_cast<const PageImage&>(*el).getImageBlock().wouldShowPlaceholder(forceLoadLargeImages,
                                                                                   monochromeOutput)) {
        return false;
      }
    }
  }
  return anyImage;
}

void Page::renderTextOnly(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  for (auto& element : elements) {
    // Scan every non-image element so prewarm covers text from table fragments
    // and other composite text containers, not only plain PageLine entries.
    if (element->getTag() == TAG_PageImage) {
      continue;
    }
    element->render(renderer, fontId, xOffset, yOffset);
  }
}

bool Page::serialize(FsFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    serialization::writePod(file, static_cast<uint8_t>(el->getTag()));

    if (!el->serialize(file)) {
      return false;
    }
  }

  // Serialize footnotes (clamp to MAX_FOOTNOTES_PER_PAGE to match addFootnote/deserialize limits)
  const uint16_t fnCount = std::min<uint16_t>(footnotes.size(), MAX_FOOTNOTES_PER_PAGE);
  serialization::writePod(file, fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    const auto& fn = footnotes[i];
    if (file.write(fn.number, sizeof(fn.number)) != sizeof(fn.number) ||
        file.write(fn.href, sizeof(fn.href)) != sizeof(fn.href)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  auto page = std::unique_ptr<Page>(new Page());

  uint16_t count;
  serialization::readPod(file, count);

  page->elements.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(file, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      page->elements.push_back(std::move(pi));
    } else if (tag == TAG_PageTable) {
      auto pt = PageTableFragment::deserialize(file);
      if (!pt) return nullptr;
      page->elements.push_back(std::move(pt));
    } else if (tag == TAG_PageHR) {
      auto hr = PageHR::deserialize(file);
      if (!hr) return nullptr;
      page->elements.push_back(std::move(hr));
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  // Deserialize footnotes
  uint16_t fnCount;
  serialization::readPod(file, fnCount);
  if (fnCount > MAX_FOOTNOTES_PER_PAGE) {
    LOG_ERR("PGE", "Invalid footnote count %u", fnCount);
    return nullptr;
  }
  page->footnotes.resize(fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    auto& entry = page->footnotes[i];
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number) ||
        file.read(entry.href, sizeof(entry.href)) != sizeof(entry.href)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    entry.number[sizeof(entry.number) - 1] = '\0';
    entry.href[sizeof(entry.href) - 1] = '\0';
  }

  return page;
}
