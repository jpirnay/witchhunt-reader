#include "BlockSerialization.h"

#include "ContentSink.h"  // kMaxSerializedBody (the 8 KB record cap)
#include "Epub/FootnoteEntry.h"
#include "Serialization.h"

namespace compiled {
namespace {

using serialization::readPod;
using serialization::readString;
using serialization::writePod;
using serialization::writeString;

// Serialized size of one word record (textOff u32 + styleSpan u8 + sizePct u8 + bidiLevel u8) and
// the fixed per-TEXT-record overhead — used to bound a run against the 8 KB record cap.
constexpr size_t kWordRecordBytes = sizeof(uint32_t) + 3 * sizeof(uint8_t);
constexpr size_t kTextRecordOverhead =
    sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) +
    sizeof(uint8_t);

// Codepoints (non-continuation bytes) in a NUL-terminated word — for continuation-record charOffset.
uint32_t countCodepoints(const char* s) {
  uint32_t n = 0;
  for (const char* p = s; *p != '\0'; ++p) {
    if ((static_cast<uint8_t>(*p) & 0xC0) != 0x80) ++n;
  }
  return n;
}

template <typename Sink>
void writeLength(Sink& f, const CssLength& len) {
  writePod(f, len.value);
  writePod(f, static_cast<uint8_t>(len.unit));
}

void readLength(FsFile& f, CssLength& len) {
  readPod(f, len.value);
  uint8_t unit = 0;
  readPod(f, unit);
  len.unit = static_cast<CssUnit>(unit);
}

// Word run + its backing text — shared by text blocks and table cells.
template <typename Sink>
void writeWords(Sink& f, const std::vector<Word>& words, const std::string& text) {
  writePod(f, static_cast<uint32_t>(words.size()));
  for (const Word& w : words) {
    writePod(f, w.textOff);
    writePod(f, w.styleSpan);
    writePod(f, w.sizePct);
    writePod(f, w.bidiLevel);
  }
  writeString(f, text);
}

bool readWords(FsFile& f, std::vector<Word>& words, std::string& text) {
  uint32_t count = 0;
  readPod(f, count);
  words.resize(count);
  for (Word& w : words) {
    readPod(f, w.textOff);
    readPod(f, w.styleSpan);
    readPod(f, w.sizePct);
    readPod(f, w.bidiLevel);
  }
  return readString(f, text);
}

void unpackDefined(uint32_t b, CssPropertyFlags& d) {
  int i = 0;
  auto get = [&]() { return (b >> i++) & 1u; };
  d.textAlign = get();
  d.fontStyle = get();
  d.fontWeight = get();
  d.textDecoration = get();
  d.textIndent = get();
  d.marginTop = get();
  d.marginBottom = get();
  d.marginLeft = get();
  d.marginRight = get();
  d.paddingTop = get();
  d.paddingBottom = get();
  d.paddingLeft = get();
  d.paddingRight = get();
  d.imageHeight = get();
  d.imageWidth = get();
  d.display = get();
  d.verticalAlign = get();
  d.listStyleNone = get();
  d.pageBreakBefore = get();
  d.pageBreakAfter = get();
  d.lineHeight = get();
  d.fontSizeMultiplier = get();
  d.cssFloat = get();
  d.smallCaps = get();
}

template <typename Sink>
void writeStyle(Sink& f, const CssStyle& s) {
  writePod(f, static_cast<uint8_t>(s.textAlign));
  writePod(f, static_cast<uint8_t>(s.fontStyle));
  writePod(f, static_cast<uint8_t>(s.fontWeight));
  writePod(f, static_cast<uint8_t>(s.textDecoration));
  writeLength(f, s.textIndent);
  writeLength(f, s.marginTop);
  writeLength(f, s.marginBottom);
  writeLength(f, s.marginLeft);
  writeLength(f, s.marginRight);
  writeLength(f, s.paddingTop);
  writeLength(f, s.paddingBottom);
  writeLength(f, s.paddingLeft);
  writeLength(f, s.paddingRight);
  writeLength(f, s.imageHeight);
  writeLength(f, s.imageWidth);
  writePod(f, static_cast<uint8_t>(s.display));
  writePod(f, static_cast<uint8_t>(s.verticalAlign));
  writePod(f, static_cast<uint8_t>(s.listStyleNone));
  writePod(f, static_cast<uint8_t>(s.pageBreakBefore));
  writePod(f, static_cast<uint8_t>(s.pageBreakAfter));
  writePod(f, s.lineHeightMultiplier);
  writePod(f, s.fontSizeMultiplier);
  writePod(f, static_cast<uint8_t>(s.cssFloat));
  writePod(f, static_cast<uint8_t>(s.smallCaps));
  writePod(f, packDefined(s.defined));
}

void readStyle(FsFile& f, CssStyle& s) {
  uint8_t e = 0;
  readPod(f, e);
  s.textAlign = static_cast<CssTextAlign>(e);
  readPod(f, e);
  s.fontStyle = static_cast<CssFontStyle>(e);
  readPod(f, e);
  s.fontWeight = static_cast<CssFontWeight>(e);
  readPod(f, e);
  s.textDecoration = static_cast<CssTextDecoration>(e);
  readLength(f, s.textIndent);
  readLength(f, s.marginTop);
  readLength(f, s.marginBottom);
  readLength(f, s.marginLeft);
  readLength(f, s.marginRight);
  readLength(f, s.paddingTop);
  readLength(f, s.paddingBottom);
  readLength(f, s.paddingLeft);
  readLength(f, s.paddingRight);
  readLength(f, s.imageHeight);
  readLength(f, s.imageWidth);
  readPod(f, e);
  s.display = static_cast<CssDisplay>(e);
  readPod(f, e);
  s.verticalAlign = static_cast<CssVerticalAlign>(e);
  readPod(f, e);
  s.listStyleNone = e != 0;
  readPod(f, e);
  s.pageBreakBefore = e != 0;
  readPod(f, e);
  s.pageBreakAfter = e != 0;
  readPod(f, s.lineHeightMultiplier);
  readPod(f, s.fontSizeMultiplier);
  readPod(f, e);
  s.cssFloat = static_cast<CssFloat>(e);
  readPod(f, e);
  s.smallCaps = e != 0;
  uint32_t defined = 0;
  readPod(f, defined);
  unpackDefined(defined, s.defined);
}

}  // namespace

// The 24 explicit-set flags packed into one u32, LSB-first in this fixed order. Public (declared in
// BlockSerialization.h) so styleEquals dedups on exactly the serialized bit layout.
uint32_t packDefined(const CssPropertyFlags& d) {
  uint32_t b = 0;
  int i = 0;
  auto set = [&](bool v) {
    if (v) b |= (1u << i);
    ++i;
  };
  set(d.textAlign);
  set(d.fontStyle);
  set(d.fontWeight);
  set(d.textDecoration);
  set(d.textIndent);
  set(d.marginTop);
  set(d.marginBottom);
  set(d.marginLeft);
  set(d.marginRight);
  set(d.paddingTop);
  set(d.paddingBottom);
  set(d.paddingLeft);
  set(d.paddingRight);
  set(d.imageHeight);
  set(d.imageWidth);
  set(d.display);
  set(d.verticalAlign);
  set(d.listStyleNone);
  set(d.pageBreakBefore);
  set(d.pageBreakAfter);
  set(d.lineHeight);
  set(d.fontSizeMultiplier);
  set(d.cssFloat);
  set(d.smallCaps);
  return b;
}

void splitTextBlock(Block&& block, const std::function<void(Block&&)>& emit) {
  // Whole-block serialized size of the word run + text (the part the cap governs).
  const size_t bodyBytes = kTextRecordOverhead + block.words.size() * kWordRecordBytes + block.text.size();
  if (block.words.size() <= 1 || bodyBytes <= kMaxSerializedBody) {
    emit(std::move(block));
    return;
  }
  // Footnotes/xpath ride with the split: take them off the source block so per-run records own their
  // share (footnotes distributed by word index, xpath on the first run).
  std::vector<FootnoteRef> srcFootnotes = std::move(block.footnotes);
  block.footnotes.clear();
  const bool srcHasXPath = block.hasXPath;
  const XPathCounters srcXPath = block.xpath;
  const std::string& srcText = block.text;
  const std::vector<Word>& srcWords = block.words;

  size_t wordStart = 0;
  uint32_t runCharOffset = block.charOffset;  // book-absolute; advances per emitted run
  bool first = true;
  while (wordStart < srcWords.size()) {
    size_t used = kTextRecordOverhead;
    size_t runTextBytes = 0;  // must fit serialization::MAX_STRING_LENGTH (readString caps it)
    size_t wordEnd = wordStart;
    while (wordEnd < srcWords.size()) {
      const size_t textOff = srcWords[wordEnd].textOff;
      const size_t textEnd = srcText.find('\0', textOff);
      const size_t wordBytes = (textEnd == std::string::npos ? srcText.size() : textEnd + 1) - textOff;
      const size_t add = kWordRecordBytes + wordBytes;
      // Two independent caps, each with an always-take-one-word floor so an oversized word still
      // progresses: the serialized-body cap (read-time alloc bound) AND the text-bytes cap.
      if (wordEnd != wordStart &&
          (used + add > kMaxSerializedBody || runTextBytes + wordBytes > serialization::MAX_STRING_LENGTH)) {
        break;
      }
      used += add;
      runTextBytes += wordBytes;
      ++wordEnd;
    }

    Block rec;
    rec.type = BlockType::Text;
    rec.styleId = block.styleId;
    rec.charOffset = runCharOffset;
    rec.flags = first ? block.flags : static_cast<uint8_t>(block.flags | kContinuation);
    if (first) {
      rec.inlineImageEntryPath = block.inlineImageEntryPath;
      rec.inlineImageWidth = block.inlineImageWidth;
      rec.inlineImageHeight = block.inlineImageHeight;
      rec.inlineImageSide = block.inlineImageSide;
      rec.inlineImageAlt = block.inlineImageAlt;
    }
    uint32_t runCodepoints = 0;
    for (size_t i = wordStart; i < wordEnd; ++i) {
      const char* wordPtr = &srcText[srcWords[i].textOff];
      Word w = srcWords[i];
      w.textOff = static_cast<uint32_t>(rec.text.size());
      rec.words.push_back(w);
      rec.text.append(wordPtr);
      rec.text.push_back('\0');
      runCodepoints += countCodepoints(wordPtr);
    }
    if (first && srcHasXPath) {
      rec.hasXPath = true;
      rec.xpath = srcXPath;
    }
    for (const FootnoteRef& fn : srcFootnotes) {
      if (fn.wordIndex >= wordStart && fn.wordIndex < wordEnd) {
        rec.footnotes.push_back({static_cast<uint32_t>(fn.wordIndex - wordStart), fn.entry});
      }
    }
    emit(std::move(rec));
    runCharOffset += runCodepoints;
    wordStart = wordEnd;
    first = false;
  }
}

template <typename Sink>
bool writeBlock(Sink& out, const Block& b) {
  if (!out) return false;
  writePod(out, static_cast<uint8_t>(b.type));
  writePod(out, b.styleId);
  writePod(out, b.flags);
  writePod(out, b.charOffset);
  if (b.type == BlockType::Text) {
    writeWords(out, b.words, b.text);
    writePod(out, static_cast<uint32_t>(b.footnotePreviews.size()));
    for (const PreviewRun& pr : b.footnotePreviews) {
      writePod(out, pr.startWord);
      writePod(out, pr.count);
    }
    const bool hasInline = !b.inlineImageEntryPath.empty();
    writePod(out, static_cast<uint8_t>(hasInline));
    if (hasInline) {
      writeString(out, b.inlineImageEntryPath);
      writePod(out, b.inlineImageWidth);
      writePod(out, b.inlineImageHeight);
      writePod(out, b.inlineImageSide);
      writeString(out, b.inlineImageAlt);
    }
  } else if (b.type == BlockType::Table) {
    writePod(out, static_cast<uint8_t>(b.hasBorder));
    writePod(out, static_cast<uint32_t>(b.rows.size()));
    for (const TableRow& r : b.rows) {
      writePod(out, static_cast<uint8_t>(r.isHeaderRow));
      writePod(out, static_cast<uint32_t>(r.cells.size()));
      for (const TableCell& c : r.cells) {
        writePod(out, static_cast<uint8_t>(c.isHeader));
        writePod(out, c.colSpan);
        writeWords(out, c.words, c.text);
        writeString(out, c.imageEntryPath);
        writePod(out, c.imageWidth);
        writePod(out, c.imageHeight);
        writeString(out, c.imageAlt);
      }
    }
  } else if (b.type == BlockType::Hr) {
    // A horizontal rule carries no body: type + charOffset are enough.
  } else {  // BlockType::Image
    writeString(out, b.entryPath);
    writePod(out, b.width);
    writePod(out, b.height);
    writePod(out, b.floatSide);
    writeString(out, b.alt);
  }
  // Shared per-block tail (any type): footnote anchors + optional XPath counters.
  writePod(out, static_cast<uint32_t>(b.footnotes.size()));
  for (const FootnoteRef& fn : b.footnotes) {
    writePod(out, fn.wordIndex);
    out.write(reinterpret_cast<const uint8_t*>(fn.entry.number), FOOTNOTE_NUMBER_LEN);
    out.write(reinterpret_cast<const uint8_t*>(fn.entry.href), FOOTNOTE_HREF_LEN);
  }
  writePod(out, static_cast<uint8_t>(b.hasXPath));
  if (b.hasXPath) {
    writePod(out, b.xpath.paragraphIndex);
    writePod(out, b.xpath.listItemIndex);
    writePod(out, b.xpath.bodyChildByteOffset);
  }
  return static_cast<bool>(out);
}

bool readBlock(FsFile& in, Block& b) {
  if (!in) return false;
  uint8_t type = 0;
  readPod(in, type);
  b.type = static_cast<BlockType>(type);
  readPod(in, b.styleId);
  readPod(in, b.flags);
  readPod(in, b.charOffset);
  if (b.type == BlockType::Text) {
    if (!readWords(in, b.words, b.text)) return false;
    uint32_t previewCount = 0;
    readPod(in, previewCount);
    b.footnotePreviews.resize(previewCount);
    for (PreviewRun& pr : b.footnotePreviews) {
      readPod(in, pr.startWord);
      readPod(in, pr.count);
    }
    uint8_t hasInline = 0;
    readPod(in, hasInline);
    if (hasInline) {
      if (!readString(in, b.inlineImageEntryPath)) return false;
      readPod(in, b.inlineImageWidth);
      readPod(in, b.inlineImageHeight);
      readPod(in, b.inlineImageSide);
      if (!readString(in, b.inlineImageAlt)) return false;
    }
  } else if (b.type == BlockType::Table) {
    uint8_t hasBorder = 1;
    readPod(in, hasBorder);
    b.hasBorder = hasBorder != 0;
    uint32_t rowCount = 0;
    readPod(in, rowCount);
    b.rows.resize(rowCount);
    for (uint32_t ri = 0; ri < rowCount; ++ri) {
      TableRow& r = b.rows[ri];
      uint8_t isHeaderRow = 0;
      readPod(in, isHeaderRow);
      r.isHeaderRow = isHeaderRow != 0;
      uint32_t cellCount = 0;
      readPod(in, cellCount);
      r.cells.resize(cellCount);
      for (uint32_t ci = 0; ci < cellCount; ++ci) {
        TableCell& c = r.cells[ci];
        uint8_t isHeader = 0;
        readPod(in, isHeader);
        c.isHeader = isHeader != 0;
        readPod(in, c.colSpan);
        if (!readWords(in, c.words, c.text)) return false;
        if (!readString(in, c.imageEntryPath)) return false;
        readPod(in, c.imageWidth);
        readPod(in, c.imageHeight);
        if (!readString(in, c.imageAlt)) return false;
      }
    }
  } else if (b.type == BlockType::Hr) {
    // No body.
  } else if (b.type == BlockType::Image) {
    if (!readString(in, b.entryPath)) return false;
    readPod(in, b.width);
    readPod(in, b.height);
    readPod(in, b.floatSide);
    if (!readString(in, b.alt)) return false;
  } else {
    // Unknown block type: continuing would misalign the stream. Fail cleanly.
    return false;
  }
  // Shared per-block tail: footnote anchors + optional XPath counters.
  uint32_t footnoteCount = 0;
  readPod(in, footnoteCount);
  b.footnotes.resize(footnoteCount);
  for (FootnoteRef& fn : b.footnotes) {
    readPod(in, fn.wordIndex);
    if (in.read(reinterpret_cast<uint8_t*>(fn.entry.number), FOOTNOTE_NUMBER_LEN) != FOOTNOTE_NUMBER_LEN) return false;
    if (in.read(reinterpret_cast<uint8_t*>(fn.entry.href), FOOTNOTE_HREF_LEN) != FOOTNOTE_HREF_LEN) return false;
  }
  uint8_t hasXPath = 0;
  readPod(in, hasXPath);
  b.hasXPath = hasXPath != 0;
  if (b.hasXPath) {
    readPod(in, b.xpath.paragraphIndex);
    readPod(in, b.xpath.listItemIndex);
    readPod(in, b.xpath.bodyChildByteOffset);
  }
  return static_cast<bool>(in);
}

template <typename Sink>
bool writeAnchors(Sink& out, const std::vector<Anchor>& anchors) {
  writePod(out, static_cast<uint32_t>(anchors.size()));
  for (const Anchor& a : anchors) {
    writeString(out, a.id);
    writePod(out, a.blockIndex);
    writePod(out, a.charOffsetInBlock);
  }
  return static_cast<bool>(out);
}

bool readAnchors(FsFile& in, std::vector<Anchor>& anchors) {
  uint32_t count = 0;
  readPod(in, count);
  anchors.resize(count);
  for (Anchor& a : anchors) {
    if (!readString(in, a.id)) return false;
    readPod(in, a.blockIndex);
    readPod(in, a.charOffsetInBlock);
  }
  return static_cast<bool>(in);
}

template <typename Sink>
bool writeLabels(Sink& out, const std::vector<PageBreakLabel>& labels) {
  writePod(out, static_cast<uint32_t>(labels.size()));
  for (const PageBreakLabel& pl : labels) {
    writeString(out, pl.label);
    writePod(out, pl.blockIndex);
  }
  return static_cast<bool>(out);
}

bool readLabels(FsFile& in, std::vector<PageBreakLabel>& labels) {
  uint32_t count = 0;
  readPod(in, count);
  labels.resize(count);
  for (PageBreakLabel& pl : labels) {
    if (!readString(in, pl.label)) return false;
    readPod(in, pl.blockIndex);
  }
  return static_cast<bool>(in);
}

template <typename Sink>
bool writeStylePool(Sink& out, const std::vector<CssStyle>& pool) {
  writePod(out, static_cast<uint32_t>(pool.size()));
  for (const CssStyle& s : pool) writeStyle(out, s);
  return static_cast<bool>(out);
}

bool readStylePool(FsFile& in, std::vector<CssStyle>& pool) {
  uint32_t count = 0;
  readPod(in, count);
  pool.resize(count);
  for (CssStyle& s : pool) readStyle(in, s);
  return static_cast<bool>(in);
}

template <typename Sink>
bool writeChapters(Sink& out, const std::vector<Chapter>& chapters) {
  writePod(out, static_cast<uint32_t>(chapters.size()));
  for (const Chapter& c : chapters) {
    writePod(out, c.spineIndex);
    writePod(out, c.blockIndex);
    writePod(out, c.level);
    writeString(out, c.title);
  }
  return static_cast<bool>(out);
}

bool readChapters(FsFile& in, std::vector<Chapter>& chapters) {
  uint32_t count = 0;
  readPod(in, count);
  chapters.resize(count);
  for (Chapter& c : chapters) {
    readPod(in, c.spineIndex);
    readPod(in, c.blockIndex);
    readPod(in, c.level);
    if (!readString(in, c.title)) return false;
  }
  return static_cast<bool>(in);
}

template <typename Sink>
bool writeBlockOffsets(Sink& out, const std::vector<BlockOffset>& offsets) {
  writePod(out, static_cast<uint32_t>(offsets.size()));
  for (const BlockOffset& bo : offsets) {
    writePod(out, bo.fileOffset);
    writePod(out, bo.charOffset);
    writePod(out, bo.recordIndex);
  }
  return static_cast<bool>(out);
}

bool readBlockOffsets(FsFile& in, std::vector<BlockOffset>& offsets) {
  uint32_t count = 0;
  readPod(in, count);
  offsets.resize(count);
  for (BlockOffset& bo : offsets) {
    readPod(in, bo.fileOffset);
    readPod(in, bo.charOffset);
    readPod(in, bo.recordIndex);
  }
  return static_cast<bool>(in);
}

// Explicit instantiations of the write serializers: FsFile (whole-book writeContentBin path) and
// serialization::BufferedFileWriter (the streaming ContentBinWriter compile path, which batches the
// thousands of tiny per-word writes). Both must stay in lockstep — same code, one instantiation each.
template bool writeBlock<FsFile>(FsFile&, const Block&);
template bool writeBlock<serialization::BufferedFileWriter>(serialization::BufferedFileWriter&, const Block&);
template bool writeAnchors<FsFile>(FsFile&, const std::vector<Anchor>&);
template bool writeAnchors<serialization::BufferedFileWriter>(serialization::BufferedFileWriter&,
                                                              const std::vector<Anchor>&);
template bool writeLabels<FsFile>(FsFile&, const std::vector<PageBreakLabel>&);
template bool writeLabels<serialization::BufferedFileWriter>(serialization::BufferedFileWriter&,
                                                             const std::vector<PageBreakLabel>&);
template bool writeStylePool<FsFile>(FsFile&, const std::vector<CssStyle>&);
template bool writeStylePool<serialization::BufferedFileWriter>(serialization::BufferedFileWriter&,
                                                                const std::vector<CssStyle>&);
template bool writeChapters<FsFile>(FsFile&, const std::vector<Chapter>&);
template bool writeChapters<serialization::BufferedFileWriter>(serialization::BufferedFileWriter&,
                                                              const std::vector<Chapter>&);
template bool writeBlockOffsets<FsFile>(FsFile&, const std::vector<BlockOffset>&);
template bool writeBlockOffsets<serialization::BufferedFileWriter>(serialization::BufferedFileWriter&,
                                                                   const std::vector<BlockOffset>&);

}  // namespace compiled
