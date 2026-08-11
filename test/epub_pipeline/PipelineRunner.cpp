#include "PipelineRunner.h"

#include <GfxRenderer.h>

#include <chrono>
#include <iomanip>
#include <memory>
#include <regex>

#include "Epub.h"
#include "Epub/FootnotePreviews.h"
#include "Epub/Page.h"
#include "Epub/Section.h"

namespace pipeline_harness {
namespace {

// Image paths live under the (run-specific) cache dir; strip that prefix so
// dumps compare across runs and machines. The per-book cache subdir is named
// epub_<hash-of-absolute-path>, which differs by checkout location (e.g. a dev's
// /home/... vs CI's /home/runner/work/...), so canonicalize that hash too —
// otherwise image-bearing goldens are machine-specific and fail in CI.
std::string normalizePath(const std::string& path, const std::string& cacheDir) {
  std::string p = (path.rfind(cacheDir, 0) == 0) ? "<cache>" + path.substr(cacheDir.size()) : path;
  return std::regex_replace(p, std::regex("epub_[0-9]+"), "epub_<hash>");
}

// One `W` record per word, at `indent`. Kept to a single shape so the word-stream extraction used
// to verify "no content was dropped" (`\bW x=\S+ s=\S+ z=\S+ t=(.*)`) sees table cells and ordinary
// paragraphs alike.
void dumpWords(std::ostream& out, const TextBlock& block, const char* indent) {
  for (uint16_t w = 0; w < block.wordCount(); ++w) {
    out << indent << "W x=" << block.wordXpos(w) << " s=" << static_cast<int>(block.wordStyle(w))
        << " z=" << static_cast<int>(block.wordSizePct(w)) << " t=" << block.wordText(w) << "\n";
  }
}

void dumpTextLine(std::ostream& out, const PageLine& line) {
  const auto& block = *line.getBlock();
  const auto& bs = block.getRenderStyle();
  out << "  LINE y=" << line.yPos << " x=" << line.xPos << " align=" << static_cast<int>(bs.alignment)
      << " mult=" << std::fixed << std::setprecision(3) << bs.fontSizeMultiplier << " hfid=" << bs.headingFontId
      << " words=" << block.wordCount() << "\n";
  dumpWords(out, block, "   ");
}

// Table fragments used to dump as a single `TABLE y= x= h=` line, so every golden was blind to the
// text inside a grid — the reason a cell truncation went unnoticed across the whole corpus. Walk
// rows, cells and lines so cell content is covered by the same golden and word-stream checks as
// body text.
void dumpTableFragment(std::ostream& out, const PageTableFragment& tbl, const std::string& cacheDir) {
  out << "  TABLE y=" << tbl.yPos << " x=" << tbl.xPos << " h=" << tbl.getTotalHeight()
      << " cols=" << static_cast<int>(tbl.getColumnCount()) << " border=" << (tbl.getHasBorder() ? 1 : 0)
      << " rows=" << tbl.getRows().size() << "\n";
  for (const auto& row : tbl.getRows()) {
    out << "   ROW h=" << row.height << " hdr=" << (row.isHeaderRow ? 1 : 0) << " cells=" << row.cells.size() << "\n";
    for (size_t c = 0; c < row.cells.size(); ++c) {
      const auto& cell = row.cells[c];
      out << "    CELL " << c << " hdr=" << (cell.isHeader ? 1 : 0) << " lines=" << cell.lines.size() << "\n";
      for (const auto& line : cell.lines) {
        out << "     LN words=" << line->wordCount() << "\n";
        dumpWords(out, *line, "      ");
      }
      if (cell.image) {
        out << "     CELLIMG w=" << cell.image->getWidth() << " h=" << cell.image->getRenderedHeight()
            << " src=" << normalizePath(cell.image->getImagePath(), cacheDir) << "\n";
      }
    }
  }
}

void dumpPage(std::ostream& out, const Page& page, const uint16_t pageIndex, const std::string& cacheDir) {
  out << " PAGE " << pageIndex << " elements=" << page.elements.size() << " footnotes=" << page.footnotes.size()
      << "\n";
  for (const auto& el : page.elements) {
    switch (el->getTag()) {
      case TAG_PageLine:
        dumpTextLine(out, static_cast<const PageLine&>(*el));
        break;
      case TAG_PageImage: {
        const auto& img = static_cast<const PageImage&>(*el);
        const auto& ib = img.getImageBlock();
        out << "  IMG y=" << img.yPos << " x=" << img.xPos << " w=" << ib.getWidth() << " h=" << ib.getRenderedHeight()
            << " src=" << normalizePath(ib.getImagePath(), cacheDir) << "\n";
        break;
      }
      case TAG_PageTable:
        dumpTableFragment(out, static_cast<const PageTableFragment&>(*el), cacheDir);
        break;
      case TAG_PageHR:
        out << "  HR y=" << el->yPos << " x=" << el->xPos << "\n";
        break;
    }
  }
  for (const auto& fn : page.footnotes) {
    out << "  FN n=" << fn.number << " href=" << fn.href << "\n";
  }
}

}  // namespace

bool runAndDump(const std::string& epubPath, const std::string& cacheDir, const Profile& profile, std::ostream& out,
                const SpineStatFn& spineStat) {
  GfxRenderer renderer;

  auto epub = std::make_shared<Epub>(epubPath, cacheDir);
  if (!epub->load(true)) {
    out << "ERROR load failed\n";
    return false;
  }
  epub->loadImageManifest();
  FootnotePreviews::gather(*epub);

  out << "BOOK title=" << epub->getTitle() << " lang=" << epub->getLanguage() << " spine=" << epub->getSpineItemsCount()
      << " toc=" << epub->getTocItemsCount() << " reliableToc=" << (epub->hasReliableToc() ? 1 : 0) << "\n";

  for (int i = 0; i < epub->getSpineItemsCount(); ++i) {
    const auto spineStart = std::chrono::steady_clock::now();
    Section section(epub, i, renderer);
    Section::BuildParams p;
    p.fontId = profile.fontId;
    p.lineCompression = profile.lineCompression;
    p.extraParagraphSpacing = profile.extraParagraphSpacing;
    p.paragraphAlignment = profile.paragraphAlignment;
    p.viewportWidth = profile.viewportWidth;
    p.viewportHeight = profile.viewportHeight;
    p.hyphenationEnabled = profile.hyphenationEnabled;
    p.fontSizeNormalization = profile.fontSizeNormalization;
    p.embeddedStyle = profile.embeddedStyle;
    p.bionicReadingEnabled = profile.bionicReadingEnabled;
    p.inlineFootnotePreviews = profile.inlineFootnotePreviews;
    p.imageRendering = profile.imageRendering;
    if (!section.createSectionFile(p, {}, /*skipEviction=*/true)) {
      out << "SPINE " << i << " ERROR build failed\n";
      return false;
    }
    if (!section.loadSectionFile(p)) {
      out << "SPINE " << i << " ERROR load failed\n";
      return false;
    }
    out << "SPINE " << i << " href=" << epub->getSpineItem(i).href << " pages=" << section.pageCount
        << " truncated=" << (section.isTruncatedCache() ? 1 : 0)
        << " cssFallback=" << (section.isEmbeddedStyleFallback() ? 1 : 0) << "\n";
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      if (!page) {
        out << " PAGE " << p << " ERROR load failed\n";
        return false;
      }
      dumpPage(out, *page, p, cacheDir);
    }
    if (spineStat) {
      const auto us =
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - spineStart);
      spineStat(i, section.pageCount, us.count());
    }
  }
  return true;
}

}  // namespace pipeline_harness
