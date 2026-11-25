#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "DirectPixelWriter.h"

// The rows of a .pxc (PixelCache.h) drawn into the frame under the renderer's current mode: ImageBlock's picture caches
// and the sleep screen's PNG cache replay through this one loop.
//
// Several rows a read: an image page replays its cache up to three times a page turn (the B/W plane and both gray
// planes), and a read per row was hundreds of small reads through the storage lock and SdFat each time. About 4 KB a
// read (DirectPixelWriter::rowsPerBatch()), ending on a framebuffer byte column's edge where drawRows() draws byte
// columns. The batching was ported from upstream commit d9bcef7a (crosspoint-reader#2230).
namespace pixel_cache {

// `rows` rows of a `width`-pixel picture from where `file` stands, the first at logical (x, y). False on a short read
// (the rows before it are drawn) or when not even one row's buffer can be had.
inline bool replayRows(FsFile& file, const GfxRenderer& renderer, const int width, const int rows, const int x,
                       const int y) {
  if (rows <= 0 || width <= 0) return true;
  const int bytesPerRow = (width + 3) / 4;  // 2 bits per pixel, MSB first
  DirectPixelWriter pw;
  pw.init(renderer);
  int rowsPerRead = pw.rowsPerBatch(bytesPerRow, rows);
  auto buffer = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(rowsPerRead) * bytesPerRow);
  // A fragmented heap: eight rows still draw a framebuffer byte at a time in portrait; one row a read is the last
  // resort, and no faster than drawing a pixel at a time.
  for (const int fewer : {8, 1}) {
    if (buffer) break;
    if (fewer >= rowsPerRead) continue;
    rowsPerRead = fewer;
    buffer = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(rowsPerRead) * bytesPerRow);
  }
  if (!buffer) {
    LOG_ERR("PXC", "No memory for a pixel cache row (%d bytes)", bytesPerRow);
    return false;
  }

  for (int row = 0; row < rows;) {
    int toRead = std::min(rows - row, rowsPerRead);
    // A batch that ends inside a framebuffer byte column leaves the rest of it to a second pass over the same bytes.
    if (toRead < rows - row) toRead = pw.alignedRowCount(y + row, toRead);
    const size_t bytes = static_cast<size_t>(toRead) * bytesPerRow;
    const int got = file.read(buffer.get(), bytes);
    if (got < 0 || static_cast<size_t>(got) != bytes) {
      LOG_ERR("PXC", "Pixel cache read error at row %d", row);
      return false;
    }
    pw.drawRows(buffer.get(), bytesPerRow, toRead, x, y + row, width);
    row += toRead;
  }
  return true;
}

}  // namespace pixel_cache
