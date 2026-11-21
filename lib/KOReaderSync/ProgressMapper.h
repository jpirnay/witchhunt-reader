#pragma once
#include <Epub.h>

#include <memory>
#include <optional>
#include <string>

#include "CrossPointPosition.h"

/**
 * KOReader position representation.
 */
struct KOReaderPosition {
  std::string xpath;  // XPath-like progress string
  // The book percentage; nullopt when a record's number is no usable one (KOReaderSyncClient.h).
  std::optional<ProgressPpb> percentage = ProgressPpb();
};

/**
 * Maps between CrossPoint and KOReader position formats.
 *
 * CrossPoint tracks position as (spineIndex, pageNumber).
 * KOReader uses XPath-like strings + percentage.
 *
 * Forward mapping (CrossPoint -> KOReader):
 * - Prefer element-level XPath extracted from current spine XHTML.
 * - Fallback to synthetic chapter XPath if extraction fails.
 *
 * Reverse mapping (KOReader -> CrossPoint):
 * - Prefer incoming XPath (DocFragment + element path) when resolvable.
 * - Fallback to percentage-based approximation when XPath is missing/invalid.
 *
 * This keeps behavior stable on low-memory devices while improving round-trip
 * sync precision when KOReader provides detailed paths.
 */
class ProgressMapper {
 public:
  /**
   * Convert CrossPoint position to KOReader format.
   *
   * @param epub The EPUB book
   * @param pos CrossPoint position
   * @return KOReader position
   */
  static KOReaderPosition toKOReader(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos);

  /**
   * Convert KOReader position to CrossPoint format.
   *
   * Uses XPath-first resolution when possible and percentage fallback otherwise.
   * Returned pageNumber can still be approximate because page counts differ
   * across renderer/font/layout settings.
   *
   * @param epub The EPUB book
   * @param koPos KOReader position
   * @param currentSpineIndex Index of the currently open spine item (for density estimation)
   * @param totalPagesInCurrentSpine Total pages in the current spine item (for density estimation)
   * @return CrossPoint position
   */
  static CrossPointPosition toCrossPoint(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                         int currentSpineIndex = -1, int totalPagesInCurrentSpine = 0);

  /**
   * What a record says about its position without opening the book: the spine its DocFragment
   * names and the body-child paragraph its p[N] names, both read from the XPath string. No page;
   * that needs the chapter. Enough for ProgressComparison to decide which side is further before
   * anything is inflated. spineCount <= 0 skips the range check on the spine.
   */
  static CrossPointPosition peekRemote(const KOReaderPosition& koPos, int spineCount);

  /**
   * The book percentage of a page, in the units an upload carries (XHTML bytes). What toKOReader
   * reports, without the XPath and the chapter scan behind it.
   */
  static ProgressPpb percentageFor(const std::shared_ptr<Epub>& epub, int spineIndex, int pageNumber, int totalPages);

 private:
  /**
   * Generate XPath for KOReader compatibility.
   * Fallback format: /body/DocFragment[spineIndex + 1]/body
   */
  static std::string generateXPath(int spineIndex);
};
