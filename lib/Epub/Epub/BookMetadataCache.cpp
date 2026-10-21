#include "BookMetadataCache.h"

#include <BuildArena.h>
#include <Logging.h>
#include <Memory.h>
#include <PassTable.h>
#include <Serialization.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CacheVersion.h"
#include "FsHelpers.h"
#include "SpineHrefMatch.h"

namespace {
// v12: a sparse EPUB 3 nav gives way to an NCX that maps more chapters; a v11 cache holds the sparse TOC
// v13: TOC hrefs that match a spine item by filename only are mapped; a v12 cache holds them as -1
// v14: coverItemHref follows an SVG wrapper to its picture; a v13 cache holds the .svg path
// v15: a named cover missing from the archive no longer blocks the ZIP-name scan; a v14 cache holds the dead path
// v16: the cover fallbacks (ranked ZIP names, a cover page's largest picture, meta cover pages, cover ids, nav
//      landmarks); a v15 cache holds a title banner, a logo or no cover for such books
// v17: a spine record carries its item's ZIP stat (BOOK_SPINE_STAT_BYTES); a v16 record ends at tocIndex
constexpr uint8_t BOOK_CACHE_VERSION = 17;
constexpr char bookBinFile[] = "/book.bin";
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";
// Each TOC entry's spine item (int16_t), in TOC order: the TOC records carry -1 until buildBookBin
// writes them into book.bin with these.
constexpr char tmpTocSpineFile[] = "/toc.spine.tmp";

// The table and read window of buildBookBin's spine stat lookup: each spine item's size, and the
// ZIP stat its chapter is opened with, which book.bin keeps beside it.
//
// One walk of the ZIP central directory resolves as many spine items as the table holds, so the
// table decides how many walks a book costs. It used to be all or nothing: King's Avatar (1732
// spine items, ~3300 entries) found 43.5 KB free on the X3, short of the 67 KB the whole table
// asked for, and took a walk per spine item instead -- 427 s of a 431 s first open. Now it is a
// PassTable: a block of the lent region when first-open indexing has one (52 KB on the X3, 2000
// spine items a walk) so the heap is not touched, else the heap's largest block, and at least
// 128 entries: ceil(n / 128) walks wherever 3 KB can be had. Below LARGE_SPINE_THRESHOLD spine
// items, or with no table at all, the lookup goes per item.
//
// The walk reads through a 4 KB window beside the table: unbuffered, it is about a dozen card
// calls per central-directory entry (~0.8 s a walk for King's Avatar), buffered a few dozen reads.
constexpr size_t SIZE_LOOKUP_WINDOW_BYTES = 4096;
constexpr size_t SIZE_LOOKUP_MIN_ENTRIES = 128;

PassTable<ZipFile::StatTarget> sizeLookupTable(BuildArena* scratch, const size_t spineCount) {
  auto table = PassTable<ZipFile::StatTarget>::fromRegion(scratch, spineCount, SIZE_LOOKUP_MIN_ENTRIES,
                                                          SIZE_LOOKUP_WINDOW_BYTES);
  if (table.capacity() == 0) {
    PassTable<ZipFile::StatTarget>::HeapSizing sizing;
    sizing.minEntries = SIZE_LOOKUP_MIN_ENTRIES;
    table = PassTable<ZipFile::StatTarget>::fromHeap(spineCount, sizing, SIZE_LOOKUP_WINDOW_BYTES);
  }
  return table;
}
}  // namespace

/* ============= WRITING / BUILDING FUNCTIONS ================ */

bool BookMetadataCache::beginWrite() {
  buildMode = true;
  spineCount = 0;
  tocCount = 0;
  LOG_DBG("BMC", "Entering write mode");
  return true;
}

bool BookMetadataCache::beginContentOpfPass() {
  LOG_DBG("BMC", "Beginning content opf pass");

  // Open spine file for writing
  if (!Storage.openFileForWrite("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  spineWriter_.emplace(spineFile);
  return true;
}

bool BookMetadataCache::endContentOpfPass() {
  if (spineWriter_.has_value()) {
    spineWriter_->flush();
    spineWriter_.reset();
  }
  spineFile.close();
  return true;
}

bool BookMetadataCache::beginTocPass(BuildArena* scratch) {
  LOG_DBG("BMC", "Beginning toc pass");

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  if (!openTocOutput()) {
    spineFile.close();
    return false;
  }
  tocScratch_ = scratch;
  return true;
}

bool BookMetadataCache::openTocOutput() {
  if (!Storage.openFileForWrite("BMC", cachePath + tmpTocBinFile, tocFile)) return false;
  if (!Storage.openFileForWrite("BMC", cachePath + tmpTocSpineFile, tocSpineFile_)) {
    tocFile.close();
    return false;
  }
  tocWriter_.emplace(tocFile);
  tocSpineWriter_.emplace(tocSpineFile_, TOC_SPINE_WRITER_BYTES);
  tocCount = 0;
  tocMappedCount_ = 0;
  tocResolved_ = 0;
  tocResolvedOffset_ = 0;
  spineResumeIndex_ = 0;
  spineResumeOffset_ = 0;
  // A parse starts the TOC from its first entry; scan from the first spine again.
  tocScanNextIndex_ = 0;
  tocScanNextOffset_ = 0;
  resetTocFilenameFallback();
  return true;
}

void BookMetadataCache::resetTocFilenameFallback() {
  tocFolderShiftKnown_ = false;
  tocFilenameScanBudget_ = TOC_FILENAME_SCAN_BUDGET;
}

int16_t BookMetadataCache::findSpineByScan(const std::string& href) {
  if (!spineScanReader_.has_value()) spineScanReader_.emplace(spineFile);
  SpineEntry entry;
  // From the resume point to the end, then from the first spine up to the resume point.
  const int resumeIndex = tocScanNextIndex_;
  for (int pass = 0; pass < 2; pass++) {
    const int first = pass == 0 ? resumeIndex : 0;
    const int last = pass == 0 ? spineCount : resumeIndex;
    spineScanReader_->seek(pass == 0 ? tocScanNextOffset_ : 0);
    for (int i = first; i < last; i++) {
      readSpineEntry(*spineScanReader_, entry);
      if (entry.href == href) {
        tocScanNextIndex_ = i + 1;
        tocScanNextOffset_ = spineScanReader_->position();
        if (tocScanNextIndex_ >= spineCount) {
          tocScanNextIndex_ = 0;
          tocScanNextOffset_ = 0;
        }
        return static_cast<int16_t>(i);
      }
    }
  }
  return -1;
}

bool BookMetadataCache::resetTocPassOutput() {
  if (!buildMode) {
    LOG_DBG("BMC", "resetTocPassOutput called but not in build mode");
    return false;
  }

  tocWriter_.reset();  // discard buffered records from the abandoned parse
  tocSpineWriter_.reset();
  if (tocFile) tocFile.close();
  if (tocSpineFile_) tocSpineFile_.close();

  if (!openTocOutput()) {
    LOG_ERR("BMC", "Could not reopen TOC temp file for reset");
    return false;
  }
  LOG_DBG("BMC", "Reset TOC temp output for fallback parse");
  return true;
}

int BookMetadataCache::getTocMappedCount() {
  resolvePendingTocEntries();
  return tocMappedCount_;
}

void BookMetadataCache::resolvePendingTocEntries() {
  if (tocResolved_ >= tocCount || !tocWriter_.has_value() || !tocSpineWriter_.has_value()) return;
  const uint16_t pending = static_cast<uint16_t>(tocCount - tocResolved_);
  // The TOC records go on growing at the end of the file afterwards: reads from here on, then the
  // write position put back.
  tocWriter_->flush();
  const uint32_t tocEnd = tocWriter_->position();

  // In the lent region, the TOC records' read buffer goes with the table: the heap already holds
  // the TOC writer's and the spine reader's.
  auto table = PassTable<TocTarget>::fromRegion(tocScratch_, pending, TOC_RESOLVE_MIN_ENTRIES,
                                                serialization::BufferedFileReader::DEFAULT_BUFFER_BYTES);
  if (table.capacity() == 0) table = PassTable<TocTarget>::fromHeap(pending, PassTable<TocTarget>::HeapSizing{});
  TocTarget inlineTarget = {};
  TocTarget* const targets = table.capacity() > 0 ? table.entries() : &inlineTarget;
  const size_t perPass = table.capacity() > 0 ? table.capacity() : 1;
  if (!spineScanReader_.has_value()) spineScanReader_.emplace(spineFile);
  std::optional<serialization::BufferedFileReader> tocReader;
  if (table.extraBytes() > 0) {
    tocReader.emplace(tocFile, table.extra(), table.extraBytes());
  } else {
    tocReader.emplace(tocFile);
  }
  serialization::BufferedFileReader& tocIn = *tocReader;
  const auto byKey = [](const TocTarget& a, const TocTarget& b) {
    return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
  };
  TocEntry entry;
  SpineEntry spine;
  int passes = 0;
  while (tocResolved_ < tocCount) {
    const size_t count = std::min<size_t>(perPass, tocCount - tocResolved_);
    tocIn.seek(tocResolvedOffset_);
    for (size_t j = 0; j < count; j++) {
      readTocEntry(tocIn, entry);
      targets[j] = TocTarget{HashUtils::fnvHash64(entry.href), static_cast<uint16_t>(entry.href.size()),
                             static_cast<uint16_t>(j), -1};
    }
    const uint32_t chunkEnd = tocIn.position();
    std::sort(targets, targets + count, byKey);

    // One pass over the spine file, from just past the previous table-full's last match and round
    // to it again, ending once every entry has its spine item: a TOC in reading order reads the
    // spine file about once in all, one in any other order a pass per table-full.
    size_t matched = 0;
    const int resumeIndex = spineResumeIndex_ < spineCount ? spineResumeIndex_ : 0;
    for (int half = 0; half < 2 && matched < count; half++) {
      const int first = half == 0 ? resumeIndex : 0;
      const int last = half == 0 ? spineCount : resumeIndex;
      spineScanReader_->seek(half == 0 && resumeIndex > 0 ? spineResumeOffset_ : 0);
      for (int i = first; i < last && matched < count; i++) {
        readSpineEntry(*spineScanReader_, spine);
        if (i > MAX_ADDRESSABLE_SPINES) break;  // past what an int16_t spine index can name
        const TocTarget key = {HashUtils::fnvHash64(spine.href), static_cast<uint16_t>(spine.href.size()), 0, 0};
        for (TocTarget* t = std::lower_bound(targets, targets + count, key, byKey);
             t != targets + count && t->hrefHash == key.hrefHash && t->hrefLen == key.hrefLen; ++t) {
          if (t->spineIndex >= 0) continue;  // a spine item listed twice: its first wins
          t->spineIndex = static_cast<int16_t>(i);
          matched++;
          spineResumeIndex_ = i + 1;
          spineResumeOffset_ = spineScanReader_->position();
        }
      }
    }
    passes++;
    std::sort(targets, targets + count, [](const TocTarget& a, const TocTarget& b) { return a.slot < b.slot; });

    // An href no spine href matches exactly may still name one by its filename: read back, in TOC
    // order, and tried that way (budgeted; see findSpineByFilename).
    if (matched < count) {
      tocIn.seek(tocResolvedOffset_);
      for (size_t j = 0; j < count; j++) {
        readTocEntry(tocIn, entry);
        if (targets[j].spineIndex >= 0) continue;
        targets[j].spineIndex = findSpineByFilename(entry.href);
        if (targets[j].spineIndex == -1) {
          LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", entry.href.c_str());
        }
      }
    }
    for (size_t j = 0; j < count; j++) {
      tocSpineWriter_->writePod(targets[j].spineIndex);
      if (targets[j].spineIndex >= 0) tocMappedCount_++;
    }
    tocResolved_ = static_cast<uint16_t>(tocResolved_ + count);
    tocResolvedOffset_ = chunkEnd;
  }
  tocFile.seek(tocEnd);
  LOG_DBG("BMC", "TOC: %u entries mapped to spine items in %d passes, %u a pass (%s)", static_cast<unsigned>(pending),
          passes, static_cast<unsigned>(perPass), table.inRegion() ? "lent region" : "heap");
  (void)passes;  // only read by LOG_DBG, which compiles out at lower log levels
}

bool BookMetadataCache::endTocPass() {
  resolvePendingTocEntries();
  if (tocWriter_.has_value()) {
    tocWriter_->flush();
    tocWriter_.reset();
  }
  if (tocSpineWriter_.has_value()) {
    tocSpineWriter_->flush();
    tocSpineWriter_.reset();
  }
  spineScanReader_.reset();  // views spineFile, so dropped before it closes
  tocFile.close();
  if (tocSpineFile_) tocSpineFile_.close();
  spineFile.close();
  tocScratch_ = nullptr;
  return true;
}

bool BookMetadataCache::endWrite() {
  if (!buildMode) {
    LOG_DBG("BMC", "endWrite called but not in build mode");
    return false;
  }

  buildMode = false;
  LOG_DBG("BMC", "Wrote %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata, BuildArena* scratch) {
  LOG_DBG("BMC", "buildBookBin start: free=%lu contig=%lu", static_cast<unsigned long>(esp_get_free_heap_size()),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  // Open all three files, writing to meta, reading from spine and toc
  if (!Storage.openFileForWrite("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    bookFile.close();
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
    bookFile.close();
    spineFile.close();
    return false;
  }

  constexpr uint32_t headerASize = sizeof(BOOK_CACHE_VERSION) + /* LUT Offset */ sizeof(uint32_t) + sizeof(spineCount) +
                                   sizeof(tocCount) + sizeof(uint8_t) /* tocReliable */;
  const uint32_t metadataSize = metadata.title.size() + metadata.author.size() + metadata.language.size() +
                                metadata.coverItemHref.size() + metadata.textReferenceHref.size() +
                                metadata.series.size() + metadata.seriesIndex.size() + metadata.description.size() +
                                sizeof(uint32_t) * 8;
  const uint32_t lutSize = sizeof(uint32_t) * spineCount + sizeof(uint32_t) * tocCount;
  const uint32_t lutOffset = headerASize + metadataSize;

  // Buffered views for the whole compile: the loops below issue tens of thousands of tiny
  // field reads/writes (measured 22.6 s of pure FsFile call overhead for a 1732-spine book);
  // batching them into 4 KB transfers is the entire fix. 3 x 4 KB heap for the duration of
  // the build only — inside the first-open released-framebuffer window. Each wrapper
  // degrades to pass-through if its buffer cannot be allocated.
  serialization::BufferedFileWriter bookWriter(bookFile);
  serialization::BufferedFileReader spineReader(spineFile);
  serialization::BufferedFileReader tocReader(tocFile);
  // The TOC entries' spine items (resolvePendingTocEntries): the records themselves carry -1. A
  // book whose TOC pass wrote none (no TOC parsed) leaves every entry unmapped, as before.
  FsFile tocSpineIn;
  const bool haveTocSpines = Storage.openFileForRead("BMC", cachePath + tmpTocSpineFile, tocSpineIn);
  std::optional<serialization::BufferedFileReader> tocSpineReader;
  if (haveTocSpines) tocSpineReader.emplace(tocSpineIn, TOC_SPINE_WRITER_BYTES);
  const auto readTocWithSpine = [&](TocEntry& entry) {
    readTocEntry(tocReader, entry);
    int16_t spineIndex = -1;
    if (tocSpineReader.has_value() && tocSpineReader->readPod(spineIndex)) entry.spineIndex = spineIndex;
  };
  const auto rewindTocSpines = [&] {
    if (tocSpineReader.has_value()) tocSpineReader->seek(0);
  };

  // Header A. tocReliable is patched at the end once the TOC scan below has computed it.
  const uint32_t tocReliableHeaderPos =
      sizeof(BOOK_CACHE_VERSION) + sizeof(uint32_t) /* lutOffset */ + sizeof(spineCount) + sizeof(tocCount);
  bookWriter.writePod(BOOK_CACHE_VERSION);
  bookWriter.writePod(lutOffset);
  bookWriter.writePod(spineCount);
  bookWriter.writePod(tocCount);
  bookWriter.writePod(static_cast<uint8_t>(0));  // placeholder for tocReliable
  // Metadata
  bookWriter.writeString(metadata.title);
  bookWriter.writeString(metadata.author);
  bookWriter.writeString(metadata.language);
  bookWriter.writeString(metadata.coverItemHref);
  bookWriter.writeString(metadata.textReferenceHref);
  bookWriter.writeString(metadata.series);
  bookWriter.writeString(metadata.seriesIndex);
  bookWriter.writeString(metadata.description);

  // Scratch entries reused across all read loops below. Their internal
  // std::strings keep the capacity of the longest href/title encountered, so
  // each subsequent readSpineEntry / readTocEntry call reuses that allocation
  // instead of churning hundreds of small heap blocks during the build.
  SpineEntry spineScratch;
  TocEntry tocScratch;

  // Loop through spine entries, writing LUT positions. A book.bin spine record is its temp record
  // and the ZIP stat after it.
  spineReader.seek(0);
  for (int i = 0; i < spineCount; i++) {
    uint32_t pos = spineReader.position();
    readSpineEntry(spineReader, spineScratch);
    bookWriter.writePod(pos + lutOffset + lutSize + static_cast<uint32_t>(i) * BOOK_SPINE_STAT_BYTES);
  }
  // Total bytes of spine records — the offset the TOC records start at in book.bin.
  const uint32_t spineBytesTotal = spineReader.position() + static_cast<uint32_t>(spineCount) * BOOK_SPINE_STAT_BYTES;

  // Loop through toc entries, writing LUT positions
  tocReader.seek(0);
  for (int i = 0; i < tocCount; i++) {
    uint32_t pos = tocReader.position();
    readTocEntry(tocReader, tocScratch);
    bookWriter.writePod(pos + lutOffset + lutSize + spineBytesTotal);
  }

  // LUTs complete
  // Loop through spines from spine file matching up TOC indexes, calculating cumulative size and writing to book.bin

  // Build spineIndex->tocIndex mapping in one pass (O(n) instead of O(n*m)).
  // Also count distinct spines referenced by the TOC so tocReliable can be persisted in the
  // header below — without this, every first-page load on a large book pays an O(tocCount)
  // seek-heavy scan in Epub::hasReliableToc().
  //
  // Deque, not vector: only ~3.5 KB at 1732 spines, but a vector demands it as ONE contiguous
  // block, and bare operator new abort()s under -fno-exceptions on a fragmented heap — the
  // failure class that once aborted the size lookup's tables below.
  std::deque<int16_t> spineToTocIndex(spineCount, -1);
  int distinctSpinesReferenced = 0;
  tocReader.seek(0);
  rewindTocSpines();
  for (int j = 0; j < tocCount; j++) {
    readTocWithSpine(tocScratch);
    if (tocScratch.spineIndex >= 0 && tocScratch.spineIndex < spineCount) {
      if (spineToTocIndex[tocScratch.spineIndex] == -1) {
        spineToTocIndex[tocScratch.spineIndex] = static_cast<int16_t>(j);
        distinctSpinesReferenced++;
      }
    }
  }

  // Mirrors the heuristic in Epub::hasReliableToc(): require >=25% distinct spine coverage,
  // with short-circuits for edge cases (no entries, or large book with a single TOC entry).
  if (spineCount > 0 && tocCount > 0 && !(spineCount >= 8 && tocCount <= 1)) {
    tocReliable = (distinctSpinesReferenced * 4 >= spineCount);
  } else {
    tocReliable = false;
  }

  ZipFile zip(epubPath);
  // Opened once for every size below. Never loadAllFileStatSlims(): holding every central-directory
  // entry of a 2000-chapter EPUB in RAM crashed the ESP32-C3
  // (https://github.com/crosspoint-reader/crosspoint-reader/issues/134).
  if (!zip.open()) {
    LOG_ERR("BMC", "Could not open EPUB zip for size calculations");
    bookFile.close();
    spineFile.close();
    tocFile.close();
    return false;
  }

  // Each spine item's ZIP stat, a table-full of spine items per central-directory walk (see
  // sizeLookupTable). A table-full's names are read from the spine file ahead of the loop below,
  // which reads the same records again and finds each one's stat by name to write them out.
  const bool batched = spineCount >= LARGE_SPINE_THRESHOLD;
  const PassTable<ZipFile::StatTarget> lookup = sizeLookupTable(batched ? scratch : nullptr, batched ? spineCount : 0);
  ZipFile::StatTarget* const table = lookup.entries();
  const size_t perWalk = lookup.capacity();
  if (batched && perWalk == 0) {
    LOG_INF("BMC", "No room for a size table (free %u); per-item size lookup for %d spine items",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)), spineCount);
  }
  std::string pathScratch;
  int walks = 0;
  // Fills table[0, count) with the stats of spine items [first, first + count), sorted by name for
  // StatTarget::find, and leaves the spine reader where it found it.
  const auto resolveStats = [&](const int count) {
    const uint32_t resumeAt = spineReader.position();
    for (int j = 0; j < count; j++) {
      readSpineEntry(spineReader, spineScratch);
      // Cached hrefs are already decoded (see ContentOpfParser); decoding again would corrupt
      // filenames that legitimately contain '%' sequences.
      FsHelpers::normalisePath(spineScratch.href, pathScratch);
      table[j] = ZipFile::StatTarget::named(pathScratch);
    }
    std::sort(table, table + count, ZipFile::StatTarget::before);
    zip.fillFileStats(table, count, lookup.extra(), lookup.extraBytes());
    walks++;
    spineReader.seek(resumeAt);
  };

  uint32_t cumSize = 0;
  spineReader.seek(0);
  int lastSpineTocIndex = -1;
  // Spine items [tableFirst, tableEnd) have their stats in the table.
  int tableFirst = 0;
  int tableEnd = 0;
  for (int i = 0; i < spineCount; i++) {
    if (perWalk > 0 && i == tableEnd) {
      tableFirst = i;
      tableEnd = i + static_cast<int>(std::min<size_t>(perWalk, spineCount - i));
      resolveStats(tableEnd - tableFirst);
    }
    readSpineEntry(spineReader, spineScratch);

    spineScratch.tocIndex = spineToTocIndex[i];

    // Not a huge deal if we don't fine a TOC entry for the spine entry, this is expected behaviour for EPUBs
    // Logging here is for debugging
    if (spineScratch.tocIndex == -1) {
      LOG_DBG("BMC", "Warning: Could not find TOC entry for spine item %d: %s, using title from last section", i,
              spineScratch.href.c_str());
      spineScratch.tocIndex = lastSpineTocIndex;
    }
    lastSpineTocIndex = spineScratch.tocIndex;

    FsHelpers::normalisePath(spineScratch.href, pathScratch);
    const ZipFile::StatTarget* const found =
        perWalk > 0 ? ZipFile::StatTarget::find(table, table + (tableEnd - tableFirst), pathScratch) : nullptr;
    ZipFile::FileStatSlim stat = {};
    if (found != nullptr && found->found()) {
      stat = found->stat();
    } else if (!zip.loadFileStatSlim(pathScratch.c_str(), &stat)) {
      // Not matched by the walk, which compares names byte for byte (an entry stored in another
      // case or with backslashes): looked up on its own, and still unknown if that fails too.
      LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", pathScratch.c_str());
      stat = {};
    }

    cumSize += stat.uncompressedSize;
    spineScratch.cumulativeSize = cumSize;
    spineScratch.zipMethod = stat.method;
    spineScratch.zipCompressedSize = stat.compressedSize;
    spineScratch.zipUncompressedSize = stat.uncompressedSize;
    spineScratch.zipLocalHeaderOffset = stat.localHeaderOffset;

    // Write out spine data to book.bin
    writeBookSpineEntry(bookWriter, spineScratch);
  }
  if (perWalk > 0) {
    LOG_DBG("BMC", "Spine sizes: %d items in %d central-directory walks, table of %u in the %s", spineCount, walks,
            static_cast<unsigned>(perWalk), lookup.inRegion() ? "lent region" : "heap");
  } else {
    LOG_DBG("BMC", "Spine sizes: %d items looked up one by one", spineCount);
  }
  (void)walks;  // only read by LOG_DBG, which compiles out at lower log levels
  // Close opened zip file
  zip.close();

  // Loop through toc entries from toc file writing to book.bin
  tocReader.seek(0);
  rewindTocSpines();
  for (int i = 0; i < tocCount; i++) {
    readTocWithSpine(tocScratch);
    writeTocEntry(bookWriter, tocScratch);
  }

  // Patch tocReliable placeholder in header A. Everything buffered must be on disk before the
  // out-of-band seek writes through the raw file handle.
  bookWriter.flush();
  bookFile.seek(tocReliableHeaderPos);
  serialization::writePod(bookFile, static_cast<uint8_t>(tocReliable ? 1 : 0));

  bookFile.close();
  spineFile.close();
  tocFile.close();
  tocSpineReader.reset();
  if (haveTocSpines) tocSpineIn.close();

  LOG_DBG("BMC", "Successfully built book.bin");
  LOG_DBG("BMC", "buildBookBin end: free=%lu contig=%lu", static_cast<unsigned long>(esp_get_free_heap_size()),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  return true;
}

bool BookMetadataCache::cleanupTmpFiles() const {
  const auto spineBinFile = cachePath + tmpSpineBinFile;
  if (Storage.exists(spineBinFile.c_str())) {
    Storage.remove(spineBinFile.c_str());
  }
  const auto tocBinFile = cachePath + tmpTocBinFile;
  if (Storage.exists(tocBinFile.c_str())) {
    Storage.remove(tocBinFile.c_str());
  }
  const auto tocSpineFile = cachePath + tmpTocSpineFile;
  if (Storage.exists(tocSpineFile.c_str())) {
    Storage.remove(tocSpineFile.c_str());
  }
  return true;
}

uint32_t BookMetadataCache::writeTocEntry(FsFile& file, const TocEntry& entry) const {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.title);
  serialization::writeString(file, entry.href);
  serialization::writeString(file, entry.anchor);
  serialization::writePod(file, entry.level);
  serialization::writePod(file, entry.spineIndex);
  return pos;
}

uint32_t BookMetadataCache::writeSpineEntry(serialization::BufferedFileWriter& out, const SpineEntry& entry) const {
  const uint32_t pos = out.position();
  out.writeString(entry.href);
  out.writePod(entry.cumulativeSize);
  out.writePod(entry.tocIndex);
  return pos;
}

uint32_t BookMetadataCache::writeBookSpineEntry(serialization::BufferedFileWriter& out, const SpineEntry& entry) const {
  const uint32_t pos = writeSpineEntry(out, entry);
  out.writePod(entry.zipMethod);
  out.writePod(entry.zipCompressedSize);
  out.writePod(entry.zipUncompressedSize);
  out.writePod(entry.zipLocalHeaderOffset);
  return pos;
}

uint32_t BookMetadataCache::writeTocEntry(serialization::BufferedFileWriter& out, const TocEntry& entry) const {
  const uint32_t pos = out.position();
  out.writeString(entry.title);
  out.writeString(entry.href);
  out.writeString(entry.anchor);
  out.writePod(entry.level);
  out.writePod(entry.spineIndex);
  return pos;
}

void BookMetadataCache::readSpineEntry(serialization::BufferedFileReader& in, SpineEntry& out) const {
  in.readString(out.href);
  in.readPod(out.cumulativeSize);
  in.readPod(out.tocIndex);
}

void BookMetadataCache::readTocEntry(serialization::BufferedFileReader& in, TocEntry& out) const {
  in.readString(out.title);
  in.readString(out.href);
  in.readString(out.anchor);
  in.readPod(out.level);
  in.readPod(out.spineIndex);
}

// Note: for the LUT to be accurate, this **MUST** be called for all spine items before `addTocEntry` is ever called
// this is because in this function we're marking positions of the items
void BookMetadataCache::createSpineEntry(const std::string& href) {
  if (!buildMode || !spineFile || !spineWriter_.has_value()) {
    LOG_DBG("BMC", "createSpineEntry called but not in build mode");
    return;
  }

  const SpineEntry entry(href, 0, -1);
  writeSpineEntry(*spineWriter_, entry);
  spineCount++;
}

// A TOC href that no spine href matches exactly can still name a spine item by its filename, as
// Epub::resolveHrefToSpineIndex accepts for in-book links. Books ship nav documents whose links are
// relative to a folder the nav does not sit in (nav.xhtml in OEBPS/ linking "c1.xhtml" for
// OEBPS/text/c1.xhtml): every entry missed, and the contents list showed titles that could not be
// opened. A filename that several spine items share in different folders is not guessed at; that
// entry stays unmapped.
//
// Such a document is off by the same folder throughout, so the folder the last match was found in
// is tried first for the next miss from the same folder, as an exact lookup: a TOC whose every
// entry misses reads the spine file once, not once per entry, and no per-spine table is built. The
// full reads that remain are budgeted per parse, so hrefs that match nothing at all cannot cost a
// read of the whole spine each on a book with thousands of spine items.
//
// Based on crosspoint-reader PR #2987 (Hoang Manh Linh, @jetaudio), reworked to scan the spine
// temp file instead of building a RAM index of every spine filename.
int16_t BookMetadataCache::findSpineByFilename(const std::string& href) {
  const std::string_view name = spine_href::filenameOf(href);
  if (name.empty()) return -1;
  const std::string_view dir = std::string_view(href).substr(0, href.size() - name.size());

  if (tocFolderShiftKnown_ && dir == tocMissDir_) {
    tocShiftedHref_.assign(tocFoundDir_).append(name);
    const int16_t shifted = findSpineByScan(tocShiftedHref_);
    if (shifted >= 0) return shifted;
  }

  if (tocFilenameScanBudget_ < spineCount) {
    if (tocFilenameScanBudget_ > 0) {
      LOG_INF("BMC", "TOC filename fallback spent its spine reads (%d spines); later misses stay unmapped", spineCount);
      tocFilenameScanBudget_ = 0;
    }
    return -1;
  }
  tocFilenameScanBudget_ -= spineCount;

  // Every scan seeks its own position before reading, so the view can be shared.
  if (!spineScanReader_.has_value()) spineScanReader_.emplace(spineFile);
  spineScanReader_->seek(0);
  SpineEntry entry;
  int16_t found = -1;
  int sameName = 0;
  std::string foundDir;
  for (int i = 0; i < spineCount; i++) {
    readSpineEntry(*spineScanReader_, entry);
    if (spine_href::match(href, entry.href) == SpineHrefMatch::None) continue;
    if (sameName++ == 0) {
      found = static_cast<int16_t>(i);
      foundDir.assign(entry.href, 0, entry.href.size() - name.size());
    }
  }
  if (sameName != 1) {
    if (sameName > 1) {
      LOG_DBG("BMC", "TOC href %s: %d spine items share its filename, left unmapped", href.c_str(), sameName);
    }
    return -1;
  }

  tocMissDir_.assign(dir);
  tocFoundDir_ = std::move(foundDir);
  tocFolderShiftKnown_ = true;
  return found;
}

void BookMetadataCache::createTocEntry(const std::string& title, const std::string& href, const std::string& anchor,
                                       const uint8_t level) {
  if (!buildMode || !tocFile || !spineFile || !tocWriter_.has_value()) {
    LOG_DBG("BMC", "createTocEntry called but not in build mode");
    return;
  }

  // Its spine item is found later, a table-full of entries at a time (resolvePendingTocEntries);
  // the record goes out now. Written field by field, not through a TocEntry: its three string
  // copies were heap blocks per TOC entry, thousands on a web novel's TOC.
  tocWriter_->writeString(title);
  tocWriter_->writeString(href);
  tocWriter_->writeString(anchor);
  tocWriter_->writePod(level);
  tocWriter_->writePod(static_cast<int16_t>(-1));
  tocCount++;
}

/* ============= READING / LOADING FUNCTIONS ================ */

bool BookMetadataCache::cacheExists(const std::string& cachePath) {
  return Storage.exists((cachePath + bookBinFile).c_str());
}

bool BookMetadataCache::cacheIsCurrent(const std::string& cachePath) {
  return cache_version::isCurrent(cachePath + bookBinFile, BOOK_CACHE_VERSION);
}

bool BookMetadataCache::load() {
  if (!Storage.openFileForRead("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(bookFile, version);
  if (version != BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Cache version mismatch: expected %d, got %d", BOOK_CACHE_VERSION, version);
    bookFile.close();
    return false;
  }

  serialization::readPod(bookFile, lutOffset);
  serialization::readPod(bookFile, spineCount);
  serialization::readPod(bookFile, tocCount);
  uint8_t tocReliableByte;
  serialization::readPod(bookFile, tocReliableByte);
  tocReliable = (tocReliableByte != 0);

  serialization::readString(bookFile, coreMetadata.title);
  serialization::readString(bookFile, coreMetadata.author);
  serialization::readString(bookFile, coreMetadata.language);
  serialization::readString(bookFile, coreMetadata.coverItemHref);
  serialization::readString(bookFile, coreMetadata.textReferenceHref);
  serialization::readString(bookFile, coreMetadata.series);
  serialization::readString(bookFile, coreMetadata.seriesIndex);
  serialization::readString(bookFile, coreMetadata.description);

  loaded = true;
  LOG_DBG("BMC", "Loaded cache data: %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

BookMetadataCache::SpineEntry BookMetadataCache::getSpineEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getSpineEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    LOG_ERR("BMC", "getSpineEntry index %d out of range", index);
    return {};
  }

  // Seek to spine LUT item, read from LUT and get out data
  bookFile.seek(lutOffset + sizeof(uint32_t) * index);
  uint32_t spineEntryPos;
  serialization::readPod(bookFile, spineEntryPos);
  bookFile.seek(spineEntryPos);
  return readSpineEntry(bookFile);
}

int BookMetadataCache::findSpineIndexByHref(const std::string_view target, const bool byFilename) {
  if (!loaded || spineCount == 0 || target.empty()) return -1;
  // The records follow each other in spine order from the first one's LUT entry.
  bookFile.seek(lutOffset);
  uint32_t firstRecord = 0;
  if (bookFile.read(&firstRecord, sizeof(firstRecord)) != static_cast<int>(sizeof(firstRecord))) return -1;
  bookFile.seek(firstRecord);
  serialization::BufferedFileReader in(bookFile, SPINE_SCAN_WINDOW_BYTES);
  std::string href;
  for (int i = 0; i < static_cast<int>(spineCount); i++) {
    if (!in.readString(href)) return -1;
    in.seek(in.position() + BOOK_SPINE_FIXED_BYTES);
    const bool found = byFilename ? spine_href::match(target, href) != SpineHrefMatch::None : href == target;
    if (found) return i;
  }
  return -1;
}

BookMetadataCache::TocEntry BookMetadataCache::getTocEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getTocEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(tocCount)) {
    LOG_ERR("BMC", "getTocEntry index %d out of range", index);
    return {};
  }

  // Seek to TOC LUT item, read from LUT and get out data
  bookFile.seek(lutOffset + sizeof(uint32_t) * spineCount + sizeof(uint32_t) * index);
  uint32_t tocEntryPos;
  serialization::readPod(bookFile, tocEntryPos);
  bookFile.seek(tocEntryPos);
  return readTocEntry(bookFile);
}

void BookMetadataCache::readSpineEntry(FsFile& file, SpineEntry& out) const {
  serialization::readString(file, out.href);
  // The fixed fields in one call, not one each.
  uint8_t fixed[BOOK_SPINE_FIXED_BYTES];
  if (file.read(fixed, sizeof(fixed)) != static_cast<int>(sizeof(fixed))) {
    const std::string href = std::move(out.href);
    out = SpineEntry();
    out.href = href;
    return;
  }
  size_t at = 0;
  const auto get = [&fixed, &at](auto& field) {
    memcpy(&field, fixed + at, sizeof(field));
    at += sizeof(field);
  };
  get(out.cumulativeSize);
  get(out.tocIndex);
  get(out.zipMethod);
  get(out.zipCompressedSize);
  get(out.zipUncompressedSize);
  get(out.zipLocalHeaderOffset);
  static_assert(sizeof(out.cumulativeSize) + sizeof(out.tocIndex) + sizeof(out.zipMethod) +
                        sizeof(out.zipCompressedSize) + sizeof(out.zipUncompressedSize) +
                        sizeof(out.zipLocalHeaderOffset) ==
                    BOOK_SPINE_FIXED_BYTES,
                "book.bin spine record changed: update BOOK_SPINE_FIXED_BYTES and bump BOOK_CACHE_VERSION");
}

void BookMetadataCache::readTocEntry(FsFile& file, TocEntry& out) const {
  serialization::readString(file, out.title);
  serialization::readString(file, out.href);
  serialization::readString(file, out.anchor);
  serialization::readPod(file, out.level);
  serialization::readPod(file, out.spineIndex);
}

BookMetadataCache::SpineEntry BookMetadataCache::readSpineEntry(FsFile& file) const {
  SpineEntry entry;
  readSpineEntry(file, entry);
  return entry;
}

BookMetadataCache::TocEntry BookMetadataCache::readTocEntry(FsFile& file) const {
  TocEntry entry;
  readTocEntry(file, entry);
  return entry;
}
