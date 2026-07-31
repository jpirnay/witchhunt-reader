#include "PipelineRunner.h"

#include <GfxRenderer.h>

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <regex>

#include "Epub.h"
#include "Epub/FootnotePreviews.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/content/BlockStreamReader.h"
#include "Epub/content/CompiledContent.h"
#include "Epub/content/ContentBinWriter.h"
#include "Epub/content/ContentSink.h"
#include "Epub/content/LayoutSink.h"

namespace pipeline_harness {
namespace {

// Image paths live under the (run-specific) cache dir; strip that prefix so
// dumps compare across runs and machines. The per-book cache subdir is named
// epub_<hash-of-absolute-path>, which differs by checkout location (e.g. a dev's
// /home/... vs CI's /home/runner/work/...), so canonicalize that hash too —
// otherwise image-bearing goldens are machine-specific and fail in CI.
std::string normalizePath(const std::string& path, const std::string& cacheDir) {
  std::string p = (path.rfind(cacheDir, 0) == 0) ? "<cache>" + path.substr(cacheDir.size()) : path;
  p = std::regex_replace(p, std::regex("epub_[0-9]+"), "epub_<hash>");
  // The image cache basename is img_<spine>_<propertyHash>_<counter>.<ext>. propertyHash is
  // settings-derived; canonicalize it so the dump compares on image IDENTITY (spine, counter,
  // extension) rather than the settings hash — this lets LayoutSink (which does not recompute
  // the section propertyHash) match the fused dump while still asserting the right image.
  return std::regex_replace(p, std::regex("img_([0-9]+)_[0-9a-f]{8}_"), "img_$1_<hash>_");
}

void dumpTextLine(std::ostream& out, const PageLine& line) {
  const auto& block = *line.getBlock();
  const auto& bs = block.getBlockStyle();
  out << "  LINE y=" << line.yPos << " x=" << line.xPos << " align=" << static_cast<int>(bs.alignment)
      << " mult=" << std::fixed << std::setprecision(3) << bs.fontSizeMultiplier << " hfid=" << bs.headingFontId
      << " words=" << block.wordCount() << "\n";
  for (uint16_t w = 0; w < block.wordCount(); ++w) {
    out << "   W x=" << block.wordXpos(w) << " s=" << static_cast<int>(block.wordStyle(w))
        << " z=" << static_cast<int>(block.wordSizePct(w)) << " t=" << block.wordText(w) << "\n";
  }
}

}  // namespace

void dumpOnePage(std::ostream& out, const Page& page, const uint16_t pageIndex, const std::string& cacheDir) {
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
      case TAG_PageTable: {
        const auto& tbl = static_cast<const PageTableFragment&>(*el);
        out << "  TABLE y=" << tbl.yPos << " x=" << tbl.xPos << " h=" << tbl.getTotalHeight() << "\n";
        break;
      }
      case TAG_PageHR:
        out << "  HR y=" << el->yPos << " x=" << el->xPos << "\n";
        break;
    }
  }
  for (const auto& fn : page.footnotes) {
    out << "  FN n=" << fn.number << " href=" << fn.href << "\n";
  }
}

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
    if (!section.createSectionFile(profile.fontId, profile.lineCompression, profile.extraParagraphSpacing,
                                   profile.paragraphAlignment, profile.viewportWidth, profile.viewportHeight,
                                   profile.hyphenationEnabled, profile.embeddedStyle, profile.bionicReadingEnabled,
                                   profile.inlineFootnotePreviews, profile.imageRendering, {}, /*skipEviction=*/true,
                                   {})) {
      out << "SPINE " << i << " ERROR build failed\n";
      return false;
    }
    if (!section.loadSectionFile(profile.fontId, profile.lineCompression, profile.extraParagraphSpacing,
                                 profile.paragraphAlignment, profile.viewportWidth, profile.viewportHeight,
                                 profile.hyphenationEnabled, profile.embeddedStyle, profile.bionicReadingEnabled,
                                 profile.inlineFootnotePreviews, profile.imageRendering)) {
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
      dumpOnePage(out, *page, p, cacheDir);
    }
    if (spineStat) {
      const auto us =
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - spineStart);
      spineStat(i, section.pageCount, us.count());
    }
  }
  return true;
}

bool compileContent(const std::string& epubPath, const std::string& cacheDir, const Profile& profile,
                    compiled::ContentSink& sink, std::ostream& out) {
  GfxRenderer renderer;

  auto epub = std::make_shared<Epub>(epubPath, cacheDir);
  if (!epub->load(true)) {
    out << "ERROR load failed\n";
    return false;
  }
  epub->loadImageManifest();
  // Gather book-level footnote previews (footnotes.bin) so the inline-footnote-preview path is
  // exercised in the content compile too, matching runAndDump/layoutViaSink. Without this the
  // preview lookup is empty and no preview is ever injected.
  FootnotePreviews::gather(*epub);

  for (int i = 0; i < epub->getSpineItemsCount(); ++i) {
    sink.beginSpine();
    Section section(epub, i, renderer);
    section.setStage1Sink(&sink);
    if (!section.createSectionFile(profile.fontId, profile.lineCompression, profile.extraParagraphSpacing,
                                   profile.paragraphAlignment, profile.viewportWidth, profile.viewportHeight,
                                   profile.hyphenationEnabled, profile.embeddedStyle, profile.bionicReadingEnabled,
                                   profile.inlineFootnotePreviews, profile.imageRendering, {}, /*skipEviction=*/true,
                                   {})) {
      out << "SPINE " << i << " ERROR build failed\n";
      return false;
    }
  }
  return true;
}

bool layoutViaSink(const std::string& epubPath, const std::string& cacheDir, const Profile& profile,
                   std::ostream& out) {
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

  compiled::LayoutParams params;
  params.fontId = profile.fontId;
  params.lineCompression = profile.lineCompression;
  params.extraParagraphSpacing = profile.extraParagraphSpacing;
  params.paragraphAlignment = profile.paragraphAlignment;
  params.viewportWidth = profile.viewportWidth;
  params.viewportHeight = profile.viewportHeight;
  params.hyphenationEnabled = profile.hyphenationEnabled;
  params.bionicReadingEnabled = profile.bionicReadingEnabled;
  params.embeddedStyle = profile.embeddedStyle;
  // Empty ladder: matches the {} the PipelineRunner passes to createSectionFile, so
  // resolveBlockFont takes the same scale-only fallback on both sides of the diff.

  params.epubFilePath = epub->getPath();

  for (int i = 0; i < epub->getSpineItemsCount(); ++i) {
    std::vector<std::unique_ptr<Page>> pages;
    // Cache-path prefix in the fused shape img_<spine>_<hash>_; the propertyHash is canonicalized
    // away by the dump's normalizePath, so a placeholder hash is fine — only spine + counter +
    // ext are compared.
    params.imageBasePath = epub->getCachePath() + "/img_" + std::to_string(i) + "_00000000_";
    compiled::LayoutSink sink(renderer, params,
                              [&pages](std::unique_ptr<Page> page) { pages.push_back(std::move(page)); });

    Section section(epub, i, renderer);
    section.setStage1Sink(&sink);
    // The fused build still writes its own section cache to disk; we ignore it and dump the
    // LayoutSink's Page stream instead. The producer drives BOTH in one walk.
    if (!section.createSectionFile(profile.fontId, profile.lineCompression, profile.extraParagraphSpacing,
                                   profile.paragraphAlignment, profile.viewportWidth, profile.viewportHeight,
                                   profile.hyphenationEnabled, profile.embeddedStyle, profile.bionicReadingEnabled,
                                   profile.inlineFootnotePreviews, profile.imageRendering, {}, /*skipEviction=*/true,
                                   {})) {
      out << "SPINE " << i << " ERROR build failed\n";
      return false;
    }
    // Match runAndDump's SPINE line exactly. truncated/cssFallback are section-cache
    // properties with no LayoutSink analogue; emit the same 0/0 the text corpus produces so
    // the dumps line up (any book that actually truncates or falls back is out of the
    // text-corpus subset and handled when those paths are covered).
    out << "SPINE " << i << " href=" << epub->getSpineItem(i).href << " pages=" << pages.size()
        << " truncated=0 cssFallback=0\n";
    // LUT invariant: exactly one paragraph-LUT entry per emitted page (Section enforces this hard
    // check, cpp:1059). Emit a marker ONLY on violation — the fused dump never contains it, so any
    // mismatch fails the equivalence EXPECT_EQ. Proves onXPathAdvance -> emitPage stays in lockstep.
    if (sink.paragraphLutPerPage().size() != pages.size()) {
      out << "  [LUT-INVARIANT-FAIL lut=" << sink.paragraphLutPerPage().size() << " pages=" << pages.size() << "]\n";
    }
    for (uint16_t p = 0; p < pages.size(); ++p) {
      dumpOnePage(out, *pages[p], p, cacheDir);
    }
  }
  return true;
}

bool compileToContentBin(const std::string& epubPath, const std::string& cacheDir, const Profile& profile,
                         std::ostream& out) {
  GfxRenderer renderer;
  auto epub = std::make_shared<Epub>(epubPath, cacheDir);
  if (!epub->load(true)) {
    out << "ERROR load failed\n";
    return false;
  }
  epub->loadImageManifest();
  FootnotePreviews::gather(*epub);

  compiled::ContentSink contentSink;
  for (int i = 0; i < epub->getSpineItemsCount(); ++i) {
    contentSink.beginSpine();
    Section section(epub, i, renderer);
    section.setStage1Sink(&contentSink);
    if (!section.createSectionFile(profile.fontId, profile.lineCompression, profile.extraParagraphSpacing,
                                   profile.paragraphAlignment, profile.viewportWidth, profile.viewportHeight,
                                   profile.hyphenationEnabled, profile.embeddedStyle, profile.bionicReadingEnabled,
                                   profile.inlineFootnotePreviews, profile.imageRendering, {}, /*skipEviction=*/true,
                                   {})) {
      out << "SPINE " << i << " ERROR stage-1 build failed\n";
      return false;
    }
  }
  // Stamp the source book's ZIP content fingerprint so a reader can reject a stale content.bin.
  uint64_t fp = 0;
  if (epub->zipContentFingerprint(&fp)) contentSink.content().sourceFingerprint = fp;

  FsFile w;
  if (!w.openForWrite(cacheDir + "/content.bin") || !compiled::writeContentBin(w, contentSink.content())) {
    out << "ERROR content.bin write failed\n";
    return false;
  }
  w.close();
  return true;
}

namespace {
namespace fs = std::filesystem;

// Byte-compare two files in bounded chunks (never loads either whole into RAM — the section file for
// a 570 KB spine can be large, and this is a test, but there is no reason to balloon). Returns 0 if
// identical, else the 1-based offset of the first differing byte, or a negative code on I/O error
// (-1 open, -2 size mismatch).
long firstDiffOffset(const std::string& a, const std::string& b) {
  FILE* fa = fopen(a.c_str(), "rb");
  FILE* fb = fopen(b.c_str(), "rb");
  if (!fa || !fb) {
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return -1;
  }
  constexpr size_t kChunk = 4096;
  uint8_t ba[kChunk], bb[kChunk];
  long offset = 0;
  long result = 0;
  for (;;) {
    const size_t na = fread(ba, 1, kChunk, fa);
    const size_t nb = fread(bb, 1, kChunk, fb);
    if (na != nb) {
      result = -2;
      break;
    }                    // size mismatch
    if (na == 0) break;  // both EOF, equal so far
    for (size_t i = 0; i < na; ++i) {
      if (ba[i] != bb[i]) {
        result = offset + static_cast<long>(i) + 1;
        goto done;
      }
    }
    offset += static_cast<long>(na);
  }
done:
  fclose(fa);
  fclose(fb);
  return result;
}

long fileSizeOf(const std::string& path) {
  std::error_code ec;
  const auto n = fs::file_size(path, ec);
  return ec ? -1 : static_cast<long>(n);
}

// The book's per-book cache dir (Epub: cacheDir + "/epub_<hash-of-path>"), where content.bin and
// sections/ live — matching what buildSectionFromContentBin reads.
std::string bookCacheDir(const std::string& cacheDir, const std::string& epubPath) {
  return cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(epubPath));
}

// Find the single section file for `spineIndex` in the book's sections/ dir (name embeds the
// settings-derived propertyHash, which the harness does not compute).
std::string findSectionFile(const std::string& bookDir, int spineIndex) {
  const auto dir = fs::path(bookDir) / "sections";
  const std::string prefix = std::to_string(spineIndex) + "_";
  if (fs::exists(dir)) {
    for (const auto& e : fs::directory_iterator(dir)) {
      const std::string name = e.path().filename().string();
      if (name.rfind(prefix, 0) == 0 && e.path().extension() == ".bin" && name.find("html_") == std::string::npos)
        return e.path().string();
    }
  }
  return {};
}
}  // namespace

bool sectionEquivalence(const std::string& epubPath, const std::string& cacheDir, int spineIndex,
                        const Profile& profile, std::ostream& out, bool sliced) {
  const std::string bookDir = bookCacheDir(cacheDir, epubPath);

  // (1) Normal parse build of the spine → capture the section file bytes.
  GfxRenderer renderer;
  auto epub = std::make_shared<Epub>(epubPath, cacheDir);
  if (!epub->load(true)) {
    out << "ERROR load failed\n";
    return false;
  }
  epub->loadImageManifest();
  FootnotePreviews::gather(*epub);
  {
    Section section(epub, spineIndex, renderer);
    if (!section.createSectionFile(profile.fontId, profile.lineCompression, profile.extraParagraphSpacing,
                                   profile.paragraphAlignment, profile.viewportWidth, profile.viewportHeight,
                                   profile.hyphenationEnabled, profile.embeddedStyle, profile.bionicReadingEnabled,
                                   profile.inlineFootnotePreviews, profile.imageRendering, {}, /*skipEviction=*/true,
                                   {})) {
      out << "ERROR parse build failed\n";
      return false;
    }
  }
  const std::string sectionPath = findSectionFile(bookDir, spineIndex);
  if (sectionPath.empty()) {
    out << "ERROR parse section file not found\n";
    return false;
  }
  // Copy the parse output aside — the read-back build overwrites the same path. std::filesystem::copy
  // streams; no whole-file buffering.
  const std::string parseCopy = bookDir + "/parse_section.bin";
  {
    std::error_code ec;
    fs::copy_file(sectionPath, parseCopy, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      out << "ERROR could not copy parse section file: " << ec.message() << "\n";
      return false;
    }
  }

  // (2) Compile content.bin via the DEVICE writer (Section::compileBookToContentBin, streaming
  // ContentBinWriter) into the book cache dir — so this gate exercises the exact device write path.
  Section::BuildParams bp;
  bp.fontId = profile.fontId;
  bp.lineCompression = profile.lineCompression;
  bp.extraParagraphSpacing = profile.extraParagraphSpacing;
  bp.paragraphAlignment = profile.paragraphAlignment;
  bp.viewportWidth = profile.viewportWidth;
  bp.viewportHeight = profile.viewportHeight;
  bp.hyphenationEnabled = profile.hyphenationEnabled;
  bp.embeddedStyle = profile.embeddedStyle;
  bp.bionicReadingEnabled = profile.bionicReadingEnabled;
  bp.inlineFootnotePreviews = profile.inlineFootnotePreviews;
  bp.imageRendering = profile.imageRendering;
  if (!Section::compileBookToContentBin(epub, renderer, bp)) {
    out << "ERROR compileBookToContentBin failed\n";
    return false;
  }

  // (3) Read-back build of the spine (overwrites the same section file path). Reuses `bp` from (2).
  // sliced=false: run-to-completion. sliced=true: pump the resumable stepper with a 1 ms budget so it
  // yields mid-spine many times — the section file must come out identical regardless of slicing.
  {
    Section section(epub, spineIndex, renderer);
    if (sliced) {
      int guard = 0;
      Section::ReadBackStep step = Section::ReadBackStep::More;
      while (guard++ < 1000000) {
        step = section.stepReadBackFromContentBin(bp, /*budgetMs=*/1, /*skipEviction=*/true);
        if (step != Section::ReadBackStep::More) break;
      }
      if (step != Section::ReadBackStep::Done) {
        out << "ERROR sliced read-back build did not reach Done (step=" << static_cast<int>(step) << ")\n";
        return false;
      }
    } else if (!section.buildSectionFromContentBin(bp, /*skipEviction=*/true)) {
      out << "ERROR read-back build failed (buildSectionFromContentBin returned false)\n";
      return false;
    }
  }
  // (4) Chunked byte-diff (neither file loaded whole).
  const long parseSize = fileSizeOf(parseCopy);
  const long readbackSize = fileSizeOf(sectionPath);
  const long diff = firstDiffOffset(parseCopy, sectionPath);
  if (diff == -1) {
    out << "ERROR could not open section files for diff\n";
    return false;
  }
  if (diff == -2 || parseSize != readbackSize) {
    out << "DIFF section file SIZE: parse=" << parseSize << " readback=" << readbackSize << "\n";
    return false;
  }
  if (diff > 0) {
    out << "DIFF section file at byte offset " << (diff - 1) << " (parse size=" << parseSize << ")\n";
    return false;
  }
  return true;
}

bool teeEquivalence(const std::string& epubPath, const std::string& cacheDir, int spineIndex, const Profile& profile,
                    std::ostream& out) {
  const std::string bookDir = bookCacheDir(cacheDir, epubPath);

  GfxRenderer renderer;
  auto epub = std::make_shared<Epub>(epubPath, cacheDir);
  if (!epub->load(true)) {
    out << "ERROR load failed\n";
    return false;
  }
  epub->loadImageManifest();
  FootnotePreviews::gather(*epub);

  Section::BuildParams bp;
  bp.fontId = profile.fontId;
  bp.lineCompression = profile.lineCompression;
  bp.extraParagraphSpacing = profile.extraParagraphSpacing;
  bp.paragraphAlignment = profile.paragraphAlignment;
  bp.viewportWidth = profile.viewportWidth;
  bp.viewportHeight = profile.viewportHeight;
  bp.hyphenationEnabled = profile.hyphenationEnabled;
  bp.embeddedStyle = profile.embeddedStyle;
  bp.bionicReadingEnabled = profile.bionicReadingEnabled;
  bp.inlineFootnotePreviews = profile.inlineFootnotePreviews;
  bp.imageRendering = profile.imageRendering;

  // (1) Plain parse build → capture the section file bytes as the reference.
  {
    Section section(epub, spineIndex, renderer);
    if (!section.createSectionFile(bp.fontId, bp.lineCompression, bp.extraParagraphSpacing, bp.paragraphAlignment,
                                   bp.viewportWidth, bp.viewportHeight, bp.hyphenationEnabled, bp.embeddedStyle,
                                   bp.bionicReadingEnabled, bp.inlineFootnotePreviews, bp.imageRendering, {},
                                   /*skipEviction=*/true, {})) {
      out << "ERROR plain parse build failed\n";
      return false;
    }
  }
  const std::string sectionPath = findSectionFile(bookDir, spineIndex);
  if (sectionPath.empty()) {
    out << "ERROR parse section file not found\n";
    return false;
  }
  const std::string parseCopy = bookDir + "/parse_section.bin";
  {
    std::error_code ec;
    fs::copy_file(sectionPath, parseCopy, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      out << "ERROR copy parse section file: " << ec.message() << "\n";
      return false;
    }
  }

  // (2) TEE build of the SAME spine via the REAL reader path (Section::setContentBinTee): the section
  //     build drives the tee itself — beginSpineAt at build start, commitSpine on a clean Done — so
  //     this gate exercises exactly the code the reader runs on a content.bin miss. Writer in
  //     non-autoCommit mode (the reader owns the publish decision).
  const std::string binPath = bookDir + "/content.bin";
  {
    uint64_t fingerprint = 0;
    epub->zipContentFingerprint(&fingerprint);
    FsFile binFile;
    if (!Storage.openFileForWrite("TEE", binPath, binFile)) {
      out << "ERROR open content.bin\n";
      return false;
    }
    compiled::ContentBinWriter writer;
    writer.setAutoCommit(false);  // Section::setContentBinTee publishes the spine explicitly on clean Done
    if (!writer.begin(binFile, static_cast<uint32_t>(epub->getSpineItemsCount()), fingerprint)) {
      out << "ERROR content.bin begin\n";
      return false;
    }
    Section section(epub, spineIndex, renderer);
    section.setContentBinTee(&writer, static_cast<uint32_t>(spineIndex));  // Section drives the tee lifecycle
    if (!section.createSectionFile(bp.fontId, bp.lineCompression, bp.extraParagraphSpacing, bp.paragraphAlignment,
                                   bp.viewportWidth, bp.viewportHeight, bp.hyphenationEnabled, bp.embeddedStyle,
                                   bp.bionicReadingEnabled, bp.inlineFootnotePreviews, bp.imageRendering, {},
                                   /*skipEviction=*/true, {})) {
      out << "ERROR tee build failed\n";
      return false;
    }
    if (!writer.finish()) {
      out << "ERROR content.bin finish\n";
      return false;
    }
    binFile.close();
    // The clean build must have PUBLISHED the spine (committed slot) via Section::setContentBinTee.
    FsFile check;
    if (!check.openForRead(binPath)) {
      out << "ERROR reopen content.bin\n";
      return false;
    }
    compiled::BlockStreamReader cr;
    if (!cr.open(check) || !cr.spineAvailable(static_cast<uint32_t>(spineIndex))) {
      out << "ERROR tee spine not committed by setContentBinTee (clean build should publish)\n";
      check.close();
      return false;
    }
    check.close();
  }

  // (2a) The tee's section file must be byte-identical to the plain parse (pages unaffected by fan-out).
  {
    const long a = fileSizeOf(parseCopy), b = fileSizeOf(sectionPath);
    const long d = firstDiffOffset(parseCopy, sectionPath);
    if (d == -1) {
      out << "ERROR diff tee section file\n";
      return false;
    }
    if (d == -2 || a != b) {
      out << "DIFF tee section SIZE: parse=" << a << " tee=" << b << "\n";
      return false;
    }
    if (d > 0) {
      out << "DIFF tee section file at byte " << (d - 1) << " (parse size=" << a << ")\n";
      return false;
    }
  }

  // (3) Read-back from the TEE-emitted content.bin → section file must ALSO match the plain parse
  //     (content.bin was emitted correctly).
  {
    Section section(epub, spineIndex, renderer);
    if (!section.buildSectionFromContentBin(bp, /*skipEviction=*/true)) {
      out << "ERROR read-back from tee content.bin failed\n";
      return false;
    }
  }
  {
    const long a = fileSizeOf(parseCopy), b = fileSizeOf(sectionPath);
    const long d = firstDiffOffset(parseCopy, sectionPath);
    if (d == -1) {
      out << "ERROR diff readback section file\n";
      return false;
    }
    if (d == -2 || a != b) {
      out << "DIFF readback(tee-bin) SIZE: parse=" << a << " readback=" << b << "\n";
      return false;
    }
    if (d > 0) {
      out << "DIFF readback(tee-bin) at byte " << (d - 1) << " (parse size=" << a << ")\n";
      return false;
    }
  }
  return true;
}

bool replayFromContentBin(const std::string& epubPath, const std::string& cacheDir, const Profile& profile,
                          std::ostream& out) {
  GfxRenderer renderer;
  // The epub is opened only for image paths / title / spine hrefs — NO createSectionFile (no
  // ZIP/XML/CSS walk). This is the settings-change fast path being measured.
  auto epub = std::make_shared<Epub>(epubPath, cacheDir);
  if (!epub->load(true)) {
    out << "ERROR load failed\n";
    return false;
  }
  epub->loadImageManifest();

  // STREAMING read: open the v5 content.bin with a BlockStreamReader (loads only the small style
  // pool + spine index), and drive LayoutSink one LOGICAL block at a time per spine — never holding
  // a whole spine in RAM. This is the plan-v2 shape (block-streaming). The file is caller-owned.
  FsFile binFile;
  if (!binFile.openForRead(cacheDir + "/content.bin")) {
    out << "ERROR content.bin open failed\n";
    return false;
  }
  compiled::BlockStreamReader reader;
  if (!reader.open(binFile)) {
    out << "ERROR content.bin read failed (bad/stale/corrupt)\n";
    return false;
  }
  // Reject a content.bin that does not match the book on disk (stale cache → recompile). A 0 stored
  // fingerprint means "unset" (anonymous compile) — skip the check then.
  uint64_t bookFp = 0;
  if (reader.fingerprint() != 0 && epub->zipContentFingerprint(&bookFp) && bookFp != reader.fingerprint()) {
    out << "ERROR content.bin fingerprint mismatch (stale cache)\n";
    return false;
  }
  // v6: chapters are per-spine, loaded by replaySpine's openSpine — no book-level read.

  out << "BOOK title=" << epub->getTitle() << " lang=" << epub->getLanguage() << " spine=" << epub->getSpineItemsCount()
      << " toc=" << epub->getTocItemsCount() << " reliableToc=" << (epub->hasReliableToc() ? 1 : 0) << "\n";

  compiled::LayoutParams params;
  params.fontId = profile.fontId;
  params.lineCompression = profile.lineCompression;
  params.extraParagraphSpacing = profile.extraParagraphSpacing;
  params.paragraphAlignment = profile.paragraphAlignment;
  params.viewportWidth = profile.viewportWidth;
  params.viewportHeight = profile.viewportHeight;
  params.hyphenationEnabled = profile.hyphenationEnabled;
  params.bionicReadingEnabled = profile.bionicReadingEnabled;
  params.embeddedStyle = profile.embeddedStyle;
  params.epubFilePath = epub->getPath();

  for (int i = 0; i < epub->getSpineItemsCount(); ++i) {
    std::vector<std::unique_ptr<Page>> pages;
    params.imageBasePath = epub->getCachePath() + "/img_" + std::to_string(i) + "_00000000_";
    compiled::LayoutSink sink(renderer, params,
                              [&pages](std::unique_ptr<Page> page) { pages.push_back(std::move(page)); });

    if (static_cast<uint32_t>(i) < reader.spineCount()) {
      // The shared per-spine replay driver (also used by the device Section read-back build).
      if (!compiled::replaySpine(reader, static_cast<uint32_t>(i), sink)) {
        out << "SPINE " << i << " ERROR block stream replay failed\n";
        return false;
      }
    } else {
      sink.onSpineEnd();  // no records for this spine
    }

    out << "SPINE " << i << " href=" << epub->getSpineItem(i).href << " pages=" << pages.size()
        << " truncated=0 cssFallback=0\n";
    if (sink.paragraphLutPerPage().size() != pages.size()) {
      out << "  [LUT-INVARIANT-FAIL lut=" << sink.paragraphLutPerPage().size() << " pages=" << pages.size() << "]\n";
    }
    for (uint16_t p = 0; p < pages.size(); ++p) {
      dumpOnePage(out, *pages[p], p, cacheDir);
    }
  }
  binFile.close();
  return true;
}

bool layoutViaContentBin(const std::string& epubPath, const std::string& cacheDir, const Profile& profile,
                         std::ostream& out) {
  // Full round-trip: Stage-1 compile+serialize, then Stage-2 read-back+layout. The dump comes
  // from the replay stage (STAGE 1 dumps nothing), so it equals a direct parse+layout.
  std::ostringstream compileLog;
  if (!compileToContentBin(epubPath, cacheDir, profile, compileLog)) {
    out << compileLog.str();
    return false;
  }
  return replayFromContentBin(epubPath, cacheDir, profile, out);
}

}  // namespace pipeline_harness
