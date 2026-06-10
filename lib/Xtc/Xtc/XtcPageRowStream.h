/**
 * XtcPageRowStream.h
 *
 * Streams an XTC page in row-major order without holding the whole page in heap.
 *
 * A full XTC page is large and contiguous (e.g. 480x800 XTH = 96000 bytes packed)
 * and routinely fails to allocate on the fragmented ESP32-C3 heap (largest free
 * block is often ~60KB). XTH (2-bit) is also stored column-major while every
 * consumer walks the page row-major, so it cannot be streamed in one pass.
 *
 * This class bridges both problems by transposing the page once into row-major
 * cache files, then serving pixels from a small cached row. To keep the common
 * black/white passes cheap it splits the page into two planes:
 *
 *   - BW plane  (1 bit/px, ~24KB) : 1 = ink (non-white). Drives the BW passes,
 *                                   the whole XTG render, and the sleep preview.
 *   - gray plane (2 bit/px, ~96KB): the 0..3 value. Read only by the XTH gray
 *                                   (LSB/MSB) passes.
 *
 * XTG (1-bit) is already row-major and only ever needs ink, so it skips the
 * transpose entirely and reads source rows straight from the page file.
 *
 * The cache files are written under paths the caller chooses. The caller decides
 * whether to keep them (reader: persisted + LRU-capped, so revisiting a page is
 * a pure read) or drop them on close (one-shot cover/thumb generation).
 */

#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "XtcParser.h"

namespace xtc {

class XtcPageRowStream {
 public:
  XtcPageRowStream() = default;
  XtcPageRowStream(const XtcPageRowStream&) = delete;
  XtcPageRowStream& operator=(const XtcPageRowStream&) = delete;
  ~XtcPageRowStream() { close(); }

  // Prepare streaming for one page.
  //   bitDepth : 1 (XTG) or 2 (XTH)
  //   bwPath   : cache file for the 1-bit ink plane (both formats)
  //   grayPath : cache file for the 2-bit gray plane (XTH only; ignored for XTG)
  //   keep     : if true the cache files are left on disk at close() (caller owns
  //              their lifetime/eviction); if false they are removed at close().
  // If the requested cache files already exist they are reused and no transpose
  // is performed. Returns false on failure, leaving the object unusable.
  bool open(XtcParser* parser, uint32_t pageIndex, uint16_t w, uint16_t h, uint8_t bitDepth, const std::string& bwPath,
            const std::string& grayPath, bool keep) {
    close();
    m_parser = parser;
    m_pageIndex = pageIndex;
    m_width = w;
    m_height = h;
    m_bitDepth = bitDepth;
    m_bwRowBytes = (w + 7) / 8;
    m_grayRowBytes = (w + 3) / 4;
    m_bwPath = bwPath;
    m_grayPath = grayPath;
    m_keep = keep;
    m_bwCachedY = -1;
    m_grayCachedY = -1;

    m_bwRow = static_cast<uint8_t*>(malloc(m_bwRowBytes));  // freed in close()
    if (!m_bwRow) {
      LOG_ERR("XTC", "OOM bw row cache (%d bytes)", m_bwRowBytes);
      close();
      return false;
    }
    if (bitDepth == 2) {
      m_grayRow = static_cast<uint8_t*>(malloc(m_grayRowBytes));  // freed in close()
      if (!m_grayRow) {
        LOG_ERR("XTC", "OOM gray row cache (%d bytes)", m_grayRowBytes);
        close();
        return false;
      }
      // Reuse existing cache files when present; otherwise transpose once.
      if (!(Storage.exists(bwPath.c_str()) && Storage.exists(grayPath.c_str()))) {
        if (!buildTranspose()) {
          close();
          return false;
        }
      }
      if (!Storage.openFileForRead("XTC", bwPath, m_bwFile) || !Storage.openFileForRead("XTC", grayPath, m_grayFile)) {
        LOG_ERR("XTC", "Failed to open page cache for read");
        close();
        return false;
      }
    }
    m_ok = true;
    return true;
  }

  bool ok() const { return m_ok; }

  // True if the pixel at (x, y) is ink (non-white). Cheap for row-major sweeps.
  bool isInk(uint16_t x, uint16_t y) {
    if (!m_ok) return false;
    if (m_bitDepth == 1) {
      // XTG read straight from the page: bit 1 = white, bit 0 = black (ink).
      if (y != m_bwCachedY) {
        if (m_parser->loadPageRange(m_pageIndex, static_cast<size_t>(y) * m_bwRowBytes, m_bwRow, m_bwRowBytes) !=
            static_cast<size_t>(m_bwRowBytes)) {
          return false;
        }
        m_bwCachedY = y;
      }
      return ((m_bwRow[x / 8] >> (7 - (x % 8))) & 1) == 0;
    }
    // XTH: BW plane bit 1 = ink.
    if (y != m_bwCachedY) {
      if (!m_bwFile.seek64(static_cast<uint64_t>(y) * m_bwRowBytes) ||
          m_bwFile.read(m_bwRow, m_bwRowBytes) != m_bwRowBytes) {
        return false;
      }
      m_bwCachedY = y;
    }
    return ((m_bwRow[x / 8] >> (7 - (x % 8))) & 1) != 0;
  }

  // Read one packed 1bpp framebuffer row (MSB-first, 0 = black, 1 = white).
  // XTG rows already use that convention and stream directly from the page.
  // XTH's cached BW plane stores 1 = ink, so callers should invert those bits
  // when writing the row to the framebuffer.
  bool readBwRow(uint16_t y, uint8_t* out, size_t outSize, bool* invertBits) {
    if (!m_ok || !out || outSize < static_cast<size_t>(m_bwRowBytes)) return false;
    if (invertBits) *invertBits = (m_bitDepth == 2);
    if (m_bitDepth == 1) {
      return m_parser->loadPageRange(m_pageIndex, static_cast<size_t>(y) * m_bwRowBytes, out, m_bwRowBytes) ==
             static_cast<size_t>(m_bwRowBytes);
    }
    return m_bwFile.seek64(static_cast<uint64_t>(y) * m_bwRowBytes) && m_bwFile.read(out, m_bwRowBytes) == m_bwRowBytes;
  }

  enum class GrayMask { DarkOnly, LightOrDark };

  // Build the packed grayscale control row used by the existing XTH gray
  // overlay pipeline. Bits are 1 where the old drawPixel(..., false) loop
  // would have written a white/untouched bit into an otherwise 0x00 plane.
  bool readGrayMaskRow(uint16_t y, GrayMask mask, uint8_t* out, size_t outSize, uint16_t maxWidth) {
    if (!m_ok || m_bitDepth != 2 || !out || outSize < static_cast<size_t>(m_bwRowBytes)) return false;
    if (y != m_grayCachedY) {
      if (!m_grayFile.seek64(static_cast<uint64_t>(y) * m_grayRowBytes) ||
          m_grayFile.read(m_grayRow, m_grayRowBytes) != m_grayRowBytes) {
        return false;
      }
      m_grayCachedY = y;
    }
    memset(out, 0, m_bwRowBytes);
    const uint16_t w = (maxWidth < m_width) ? maxWidth : m_width;
    const uint16_t groups = (w + 3) / 4;
    for (uint16_t g = 0; g < groups; g++) {
      const uint8_t grayByte = m_grayRow[g];
      const uint8_t pixelsInGroup = static_cast<uint8_t>((w - static_cast<uint16_t>(g * 4) >= 4) ? 4 : (w - g * 4));
      uint8_t nibble = 0;
      for (uint8_t p = 0; p < pixelsInGroup; p++) {
        const uint8_t v = static_cast<uint8_t>((grayByte >> (6 - p * 2)) & 0x03);
        const bool set = (mask == GrayMask::DarkOnly) ? (v == 1) : (v == 1 || v == 2);
        if (set) nibble |= static_cast<uint8_t>(0x08 >> p);
      }
      if ((g & 1) == 0) {
        out[g / 2] |= static_cast<uint8_t>(nibble << 4);
      } else {
        out[g / 2] |= nibble;
      }
    }
    return true;
  }

  // Gray value 0..3 (0=white .. 3=black) at (x, y). XTH only; for XTG returns
  // 3 (black) / 0 (white) so callers need not special-case the format.
  uint8_t grayValue(uint16_t x, uint16_t y) {
    if (!m_ok) return 0;
    if (m_bitDepth == 1) return isInk(x, y) ? 3 : 0;
    if (y != m_grayCachedY) {
      if (!m_grayFile.seek64(static_cast<uint64_t>(y) * m_grayRowBytes) ||
          m_grayFile.read(m_grayRow, m_grayRowBytes) != m_grayRowBytes) {
        return 0;
      }
      m_grayCachedY = y;
    }
    const int shift = 6 - (x % 4) * 2;
    return static_cast<uint8_t>((m_grayRow[x / 4] >> shift) & 0x03);
  }

  void close() {
    m_ok = false;
    if (m_bwFile.isOpen()) m_bwFile.close();
    if (m_grayFile.isOpen()) m_grayFile.close();
    if (!m_keep) {
      if (!m_bwPath.empty()) Storage.remove(m_bwPath.c_str());
      if (!m_grayPath.empty()) Storage.remove(m_grayPath.c_str());
    }
    if (m_parser && m_bitDepth == 1) m_parser->endPageRange();
    if (m_bwRow) {
      free(m_bwRow);
      m_bwRow = nullptr;
    }
    if (m_grayRow) {
      free(m_grayRow);
      m_grayRow = nullptr;
    }
    m_bwPath.clear();
    m_grayPath.clear();
    m_parser = nullptr;
    m_bwCachedY = -1;
    m_grayCachedY = -1;
  }

 private:
  // Transpose the column-major XTH page into row-major BW (1-bit) and gray
  // (2-bit) cache files in a single band pass, using a bounded RAM band and
  // large chunked source reads.
  bool buildTranspose() {
    static constexpr int BAND_ROWS = 64;      // output rows held in RAM per band
    static constexpr int COLS_PER_READ = 32;  // source columns per file read

    const size_t colBytes = (m_height + 7) / 8;
    const size_t planeSize = (static_cast<size_t>(m_width) * m_height + 7) / 8;
    const size_t bwBandBytes = static_cast<size_t>(BAND_ROWS) * m_bwRowBytes;
    const size_t grayBandBytes = static_cast<size_t>(BAND_ROWS) * m_grayRowBytes;
    const size_t chunkBytes = static_cast<size_t>(COLS_PER_READ) * colBytes;

    uint8_t* bwBand = static_cast<uint8_t*>(malloc(bwBandBytes));
    uint8_t* grayBand = static_cast<uint8_t*>(malloc(grayBandBytes));
    uint8_t* column = static_cast<uint8_t*>(malloc(chunkBytes * 2));  // plane1|plane2 chunk
    FsFile bwOut, grayOut;
    bool failed = !bwBand || !grayBand || !column;
    if (failed) {
      LOG_ERR("XTC", "OOM XTH transpose scratch");
    } else if (!Storage.openFileForWrite("XTC", m_bwPath, bwOut) ||
               !Storage.openFileForWrite("XTC", m_grayPath, grayOut)) {
      LOG_ERR("XTC", "Failed to open page cache for write");
      failed = true;
    }

    for (int bandY = 0; bandY < m_height && !failed; bandY += BAND_ROWS) {
      const int rowsInBand = (m_height - bandY < BAND_ROWS) ? (m_height - bandY) : BAND_ROWS;
      memset(bwBand, 0, static_cast<size_t>(rowsInBand) * m_bwRowBytes);
      memset(grayBand, 0, static_cast<size_t>(rowsInBand) * m_grayRowBytes);

      for (int colBase = 0; colBase < m_width && !failed; colBase += COLS_PER_READ) {
        const int colsInChunk = (m_width - colBase < COLS_PER_READ) ? (m_width - colBase) : COLS_PER_READ;
        const size_t readBytes = static_cast<size_t>(colsInChunk) * colBytes;
        if (m_parser->loadPageRange(m_pageIndex, static_cast<size_t>(colBase) * colBytes, column, readBytes) !=
                readBytes ||
            m_parser->loadPageRange(m_pageIndex, planeSize + static_cast<size_t>(colBase) * colBytes,
                                    column + readBytes, readBytes) != readBytes) {
          LOG_ERR("XTC", "XTH transpose read error at colBase %d band %d", colBase, bandY);
          failed = true;
          break;
        }

        for (int c = 0; c < colsInChunk; c++) {
          const size_t colIndex = colBase + c;
          const size_t x = m_width - 1 - colIndex;  // right-to-left column mapping
          const uint8_t* col1 = column + static_cast<size_t>(c) * colBytes;
          const uint8_t* col2 = column + readBytes + static_cast<size_t>(c) * colBytes;
          const int grayShift = 6 - (x % 4) * 2;  // MSB-first, pixel 0 at bits 6-7
          const uint8_t bwBit = static_cast<uint8_t>(1 << (7 - (x % 8)));
          for (int r = 0; r < rowsInBand; r++) {
            const int y = bandY + r;
            const size_t byteInCol = y / 8;
            const size_t bitInByte = 7 - (y % 8);
            const uint8_t b1 = (col1[byteInCol] >> bitInByte) & 1;
            const uint8_t b2 = (col2[byteInCol] >> bitInByte) & 1;
            const uint8_t v = static_cast<uint8_t>((b1 << 1) | b2);  // 0..3
            grayBand[static_cast<size_t>(r) * m_grayRowBytes + x / 4] |= static_cast<uint8_t>((v & 0x03) << grayShift);
            if (v != 0) bwBand[static_cast<size_t>(r) * m_bwRowBytes + x / 8] |= bwBit;
          }
        }
      }

      if (!failed) {
        const size_t bwWrite = static_cast<size_t>(rowsInBand) * m_bwRowBytes;
        const size_t grayWrite = static_cast<size_t>(rowsInBand) * m_grayRowBytes;
        if (bwOut.write(bwBand, bwWrite) != bwWrite || grayOut.write(grayBand, grayWrite) != grayWrite) {
          LOG_ERR("XTC", "XTH transpose write error at band %d", bandY);
          failed = true;
        }
      }
    }

    m_parser->endPageRange();
    if (bwOut.isOpen()) bwOut.close();
    if (grayOut.isOpen()) grayOut.close();
    free(bwBand);
    free(grayBand);
    free(column);

    if (failed) {
      Storage.remove(m_bwPath.c_str());
      Storage.remove(m_grayPath.c_str());
      return false;
    }
    return true;
  }

  XtcParser* m_parser = nullptr;
  uint32_t m_pageIndex = 0;
  uint16_t m_width = 0;
  uint16_t m_height = 0;
  uint8_t m_bitDepth = 1;
  int m_bwRowBytes = 0;
  int m_grayRowBytes = 0;
  uint8_t* m_bwRow = nullptr;
  uint8_t* m_grayRow = nullptr;
  int m_bwCachedY = -1;
  int m_grayCachedY = -1;
  FsFile m_bwFile;    // XTH ink plane, open for read
  FsFile m_grayFile;  // XTH gray plane, open for read
  std::string m_bwPath;
  std::string m_grayPath;
  bool m_keep = false;
  bool m_ok = false;
};

}  // namespace xtc
