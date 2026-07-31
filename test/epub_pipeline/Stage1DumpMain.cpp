// CLI companion to the ContentSink gtest: compile one EPUB through the Stage-1
// producer into a whole-book content.bin and print a canonical text dump of the
// serialized model (Phase 3 step 4 of docs/compiled-book-pipeline-plan.md).
//
// Usage: content_stage1_dump <book.epub> [content.bin] [cacheDir]
// Writes content.bin (default: <tmp>/content_stage1_dump/content.bin), reads it back,
// and dumps it to stdout. Two runs over the same book must produce a byte-identical
// content.bin (the determinism gate) and an identical dump.
#include <HalStorage.h>

#include <cstdio>
#include <filesystem>
#include <iostream>

#include "Epub/content/CompiledContent.h"
#include "Epub/content/ContentSink.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

const char* typeName(compiled::BlockType t) {
  switch (t) {
    case compiled::BlockType::Text:
      return "TEXT";
    case compiled::BlockType::Image:
      return "IMAGE";
    case compiled::BlockType::Table:
      return "TABLE";
  }
  return "?";
}

// Word text within a block/cell (words are NUL-terminated back-to-back).
std::string wordAt(const std::string& text, uint32_t off) { return std::string(&text[off]); }

void dumpBlock(std::ostream& out, const compiled::Block& b) {
  out << "  BLOCK type=" << typeName(b.type) << " style=" << b.styleId << " flags=" << static_cast<int>(b.flags)
      << " char=" << b.charOffset;
  if (b.type == compiled::BlockType::Text) {
    out << " words=" << b.words.size();
    if (!b.inlineImageEntryPath.empty())
      out << " inlineImg=" << b.inlineImageEntryPath << " side=" << static_cast<int>(b.inlineImageSide);
    out << "\n";
    for (const auto& w : b.words) {
      out << "   W s=" << static_cast<int>(w.styleSpan) << " z=" << static_cast<int>(w.sizePct)
          << " t=" << wordAt(b.text, w.textOff) << "\n";
    }
  } else if (b.type == compiled::BlockType::Image) {
    out << " src=" << b.entryPath << " w=" << b.width << " h=" << b.height << " float=" << static_cast<int>(b.floatSide)
        << "\n";
  } else {  // Table
    out << " rows=" << b.rows.size() << " border=" << (b.hasBorder ? 1 : 0) << "\n";
    for (size_t ri = 0; ri < b.rows.size(); ++ri) {
      const auto& r = b.rows[ri];
      out << "   ROW " << ri << (r.isHeaderRow ? " header" : "") << " cells=" << r.cells.size() << "\n";
      for (const auto& c : r.cells) {
        out << "    CELL span=" << static_cast<int>(c.colSpan) << (c.isHeader ? " th" : "")
            << " words=" << c.words.size();
        for (const auto& w : c.words) out << " " << wordAt(c.text, w.textOff);
        if (!c.imageEntryPath.empty()) out << " img=" << c.imageEntryPath;
        out << "\n";
      }
    }
  }
}

void dumpContent(std::ostream& out, const compiled::CompiledContent& content) {
  out << "CONTENT styles=" << content.stylePool.size() << " spines=" << content.spines.size()
      << " chapters=" << content.chapters.size() << "\n";
  for (size_t si = 0; si < content.spines.size(); ++si) {
    const auto& spine = content.spines[si];
    out << "SPINE " << si << " firstChar=" << spine.firstCharOffset << " blocks=" << spine.blocks.size()
        << " anchors=" << spine.anchors.size() << "\n";
    for (const auto& b : spine.blocks) dumpBlock(out, b);
    for (const auto& a : spine.anchors)
      out << "  ANCHOR id=" << a.id << " block=" << a.blockIndex << " char=" << a.charOffsetInBlock << "\n";
  }
  for (const auto& c : content.chapters)
    out << "CHAPTER spine=" << c.spineIndex << " block=" << c.blockIndex << " level=" << static_cast<int>(c.level)
        << " title=" << c.title << "\n";
}

}  // namespace

int main(const int argc, char** argv) {
  std::string epubPath, binPath, cacheDir;
  for (int i = 1; i < argc; ++i) {
    if (epubPath.empty()) {
      epubPath = argv[i];
    } else if (binPath.empty()) {
      binPath = argv[i];
    } else {
      cacheDir = argv[i];
    }
  }
  if (epubPath.empty()) {
    std::fprintf(stderr, "usage: %s <book.epub> [content.bin] [cacheDir]\n", argv[0]);
    return 2;
  }
  const auto base = fs::temp_directory_path() / "content_stage1_dump";
  if (binPath.empty()) binPath = (base / "content.bin").string();
  if (cacheDir.empty()) {
    const auto dir = base / "cache";
    fs::remove_all(dir);
    cacheDir = dir.string();
  }
  fs::create_directories(cacheDir);

  compiled::ContentSink sink;
  if (!pipeline_harness::compileContent(epubPath, cacheDir, pipeline_harness::Profile{}, sink, std::cerr)) {
    std::fprintf(stderr, "content compile FAILED\n");
    return 1;
  }

  FsFile out;
  if (!out.openForWrite(binPath) || !compiled::writeContentBin(out, sink.content())) {
    std::fprintf(stderr, "write content.bin FAILED: %s\n", binPath.c_str());
    return 1;
  }
  out.close();

  FsFile in;
  compiled::CompiledContent readback;
  if (!in.openForRead(binPath) || !compiled::readContentBin(in, readback)) {
    std::fprintf(stderr, "read content.bin FAILED: %s\n", binPath.c_str());
    return 1;
  }
  in.close();

  dumpContent(std::cout, readback);
  std::fprintf(stderr, "content.bin bytes=%ju labels=%zu\n", static_cast<uintmax_t>(fs::file_size(binPath)),
               sink.labels().size());
  return 0;
}
