/**
 * XtcParser.h
 *
 * XTC file parsing and page data extraction
 * XTC ebook support for CrossPoint Reader
 */

#pragma once

#include <HalStorage.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "XtcTypes.h"

namespace xtc {

/**
 * XTC File Parser
 *
 * Reads XTC files from SD card and extracts page data.
 * Designed for ESP32-C3's limited RAM (~380KB) using streaming.
 *
 * The source file is kept closed between reads to free heap for rendering.
 * It is reopened on-demand for page table lookups and bitmap data reads.
 */
class XtcParser {
 public:
  XtcParser();
  ~XtcParser();

  // File open/close
  XtcError open(const char* filepath);
  void close();
  bool isOpen() const { return m_isOpen; }

  // Header information access
  const XtcHeader& getHeader() const { return m_header; }
  uint16_t getPageCount() const { return m_header.pageCount; }
  uint16_t getWidth() const { return m_defaultWidth; }
  uint16_t getHeight() const { return m_defaultHeight; }
  uint8_t getBitDepth() const { return m_bitDepth; }  // 1 = XTC/XTG, 2 = XTCH/XTH

  // Page information
  bool getPageInfo(uint32_t pageIndex, PageInfo& info);

  /**
   * Load page bitmap (raw 1-bit data, skipping XTG header)
   *
   * @param pageIndex Page index (0-based)
   * @param buffer Output buffer (caller allocated)
   * @param bufferSize Buffer size
   * @return Number of bytes read on success, 0 on failure
   */
  size_t loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize);

  /**
   * Read a contiguous byte range of a page's bitmap data (after the XTG/XTH
   * page header). Lets a caller stream a page in small windows instead of
   * holding the whole ~94KB page in heap.
   *
   * The file is left open across calls so repeated reads stay cheap; call
   * endPageRange() when the streaming session is done to release the handle.
   *
   * @param pageIndex   Page index (0-based)
   * @param byteOffset  Offset into the bitmap data (0 = first bitmap byte)
   * @param buffer      Output buffer (caller allocated, >= length bytes)
   * @param length      Number of bytes to read
   * @return Number of bytes read, or 0 on failure
   */
  size_t loadPageRange(uint32_t pageIndex, size_t byteOffset, uint8_t* buffer, size_t length);

  /** Release the file handle held open by loadPageRange(). */
  void endPageRange();

  /**
   * Streaming page load
   * Memory-efficient method that reads page data in chunks.
   *
   * @param pageIndex Page index
   * @param callback Callback function to receive data chunks
   * @param chunkSize Chunk size (default: 1024 bytes)
   * @return Error code
   */
  XtcError loadPageStreaming(uint32_t pageIndex,
                             std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                             size_t chunkSize = 1024);

  // Get title/author from metadata
  std::string getTitle() const { return m_title; }
  std::string getAuthor() const { return m_author; }

  bool hasChapters() const { return m_hasChapters; }
  const std::vector<ChapterInfo>& getChapters();

  // Validation
  static bool isValidXtcFile(const char* filepath);

  // Error information
  XtcError getLastError() const { return m_lastError; }

 private:
  FsFile m_file;
  std::string m_filepath;
  bool m_isOpen;
  XtcHeader m_header;
  std::vector<ChapterInfo> m_chapters;
  std::string m_title;
  std::string m_author;
  uint16_t m_defaultWidth;
  uint16_t m_defaultHeight;
  uint8_t m_bitDepth;  // 1 = XTC/XTG (1-bit), 2 = XTCH/XTH (2-bit)
  bool m_hasChapters;
  bool m_chaptersLoaded;
  XtcError m_lastError;

  // Streaming range session state (loadPageRange/endPageRange).
  // m_rangePage is the page whose bitmap base offset is cached; -1 = none open.
  int32_t m_rangePage = -1;
  uint64_t m_rangeBitmapOffset = 0;  // file offset of the page's first bitmap byte

  // Internal helper functions
  XtcError readHeader();
  XtcError readFirstPageInfo();
  XtcError readTitle();
  XtcError readAuthor();
  XtcError readChapters();
  bool readPageTableEntry(uint32_t pageIndex, PageInfo& info);

  // File handle management — reopen on demand, close after use
  bool ensureFileOpen();
  void closeFile();
};

}  // namespace xtc
