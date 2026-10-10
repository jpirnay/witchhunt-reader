#include "BookDetails.h"

#include <Epub.h>
#include <Epub/MetadataSidecar.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <SidecarFiles.h>
#include <Txt.h>
#include <Xtc.h>

#include "activities/reader/ReaderActivity.h"

namespace {

// details.bin sits with the book's other cache files, whatever its format.
std::string detailsPath(const std::string& bookPath) { return ReaderActivity::bookCacheDir(bookPath) + "/details.bin"; }

// The sidecar over what the book said of itself, field by field: an empty sidecar field leaves the
// book's own. False when the sidecar is there but could not be read just now -- for want of memory,
// most likely -- in which case nothing may be recorded: the book would show without its sidecar's
// labels until the sidecar is edited.
bool applySidecar(const std::string& bookPath, BookDetails& out) {
  MetadataSidecarFields sidecar;
  const MetadataSidecar::Result result = MetadataSidecar::read(bookPath, sidecar);
  if (result == MetadataSidecar::Result::Unavailable) return false;
  if (result != MetadataSidecar::Result::Read) return true;
  if (!sidecar.title.empty()) out.title = sidecar.title;
  if (!sidecar.author.empty()) out.author = sidecar.author;
  MetadataSidecar::overlayPrimaryAuthor(sidecar, out.primaryAuthor, out.authorSort);
  if (!sidecar.series.empty()) out.series = sidecar.series;
  if (!sidecar.seriesIndex.empty()) out.seriesIndex = sidecar.seriesIndex;
  if (!sidecar.isbn.empty()) out.isbn = sidecar.isbn;
  if (!sidecar.asin.empty()) out.asin = sidecar.asin;
  return true;
}

}  // namespace

namespace BookDetailsLookup {

// details.bin only, never book.bin. Epub::loadForMetadata() reads book.bin when it is current but
// quietly falls back to the ~300 ms OPF parse when it is from an older cache version -- which after
// a firmware update is every book on the card -- and this runs while a page is being drawn.
bool cached(const std::string& bookPath, const uint32_t bookSize, BookDetails& out) {
  // A TXT or Markdown book says nothing about itself: without a sidecar there is nothing to look up,
  // and the view titles it by its filename. An existence probe, not the sidecar's hash.
  const bool describesItself = FsHelpers::hasEpubExtension(bookPath) || FsHelpers::hasXtcExtension(bookPath);
  if (!describesItself && SidecarFiles::metadataPath(bookPath).empty()) {
    out = {};
    return true;
  }
  const std::string path = detailsPath(bookPath);
  // Most misses are books never seen before: answer those without hashing the sidecar.
  if (!Storage.exists(path.c_str())) return false;
  return BookDetailsCache::read(path, bookSize, SidecarFiles::metadataStamp(bookPath), out);
}

bool parse(const std::string& bookPath, const uint32_t bookSize, BookDetails& out, BuildArena* scratch) {
  out = {};
  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    // Past book.bin, which keeps no primary author: one OPF parse per book, then details.bin has it.
    // The .opf sidecar is applied by the load.
    if (!epub.loadForMetadata(scratch, /*useBookBin=*/false)) return false;
    out.title = epub.getTitle();
    out.author = epub.getAuthor();
    out.primaryAuthor = epub.getPrimaryAuthor();
    out.authorSort = epub.getAuthorSort();
    out.series = epub.getSeries();
    out.seriesIndex = epub.getSeriesIndex();
    out.isbn = epub.getIsbn();
    out.asin = epub.getAsin();
    epub.setupCacheDir();
  } else if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    if (!xtc.load()) return false;
    out.title = xtc.getTitle();
    out.author = xtc.getAuthor();
    out.primaryAuthor = out.author;  // an XTC header names one author
    if (!applySidecar(bookPath, out)) return false;
    xtc.setupCacheDir();
  } else {
    // TXT and Markdown: the sidecar is all there is. Recorded even when it gave nothing (a malformed
    // sidecar), so the book is not read again on every visit; editing the sidecar changes its stamp.
    if (!applySidecar(bookPath, out)) return false;
    Txt(bookPath, "/.crosspoint").setupCacheDir();
  }
  BookDetailsCache::write(detailsPath(bookPath), bookSize, SidecarFiles::metadataStamp(bookPath), out);
  return true;
}

}  // namespace BookDetailsLookup
