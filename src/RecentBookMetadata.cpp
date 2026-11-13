#include "RecentBookMetadata.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <SidecarFiles.h>

#include "RecentBooksStore.h"
#include "activities/home/BookDetails.h"

namespace RecentBookMetadata {
namespace {

// "Series #3", or just "Series" without an index.
std::string seriesLabel(const std::string& series, const std::string& index) {
  return !series.empty() && !index.empty() ? series + " #" + index : series;
}

}  // namespace

uint32_t openStamp(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)
             ? SidecarFiles::metadataStamp(path)
             : 0;
}

bool refreshFromSidecar(RecentBook& book) {
  const uint32_t stamp = SidecarFiles::metadataStamp(book.path);
  if (stamp == book.metadataStamp) return false;
  if (FsHelpers::hasEpubExtension(book.path)) {
    // The same load the reader's metadata comes from, so a removed sidecar restores the embedded values rather than
    // leaving the old override.
    Epub epub(book.path, "/.crosspoint");
    if (!epub.loadForMetadata()) {
      LOG_DBG("RBS", "Sidecar metadata changed but book did not load, keeping entry: %s", book.path.c_str());
      return false;
    }
    book.title = epub.getTitle();
    book.author = epub.getAuthor();
    book.series = seriesLabel(epub.getSeries(), epub.getSeriesIndex());
  } else {
    // XTC, TXT and Markdown: the lookup the Library's Details view uses, the sidecar over the book's own header (XTC)
    // or file name (TXT, Markdown). Edits made in the web interface's Details dialog reached Home for EPUBs only.
    BookDetails details;
    if (!BookDetailsLookup::cached(book.path, 0, details) && !BookDetailsLookup::parse(book.path, 0, details)) {
      return false;
    }
    // No title (a TXT or Markdown book whose sidecar was removed) means the file name, as the reader lists it
    // (LineReaderActivity); keeping the old one would show the removed title.
    book.title = details.title.empty() ? book.path.substr(book.path.rfind('/') + 1) : details.title;
    book.author = details.author;
    book.series = seriesLabel(details.series, details.seriesIndex);
  }
  LOG_DBG("RBS", "Sidecar metadata changed, refreshing entry: %s", book.path.c_str());
  book.metadataStamp = stamp;
  return true;
}

std::string overridesCacheDir(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) ? Epub::cachePathFor(path, "/.crosspoint") : std::string();
}

}  // namespace RecentBookMetadata
