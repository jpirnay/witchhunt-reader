#include "CompiledContent.h"

#include <HalStorage.h>

#include "BlockSerialization.h"
#include "BlockStreamReader.h"
#include "Serialization.h"

namespace compiled {
namespace {

using serialization::readPod;
using serialization::writePod;

}  // namespace

// Whole-book v6 writer. Produces the SAME on-disk layout as the streaming ContentBinWriter (header →
// pre-allocated spine-offset index → per-spine SELF-CONTAINED sections: block stream then an aux
// region of anchors + labels + this spine's style table + this spine's chapters), from an
// already-materialized CompiledContent (test/tooling convenience). Blocks are written AS-IS (their
// existing styleId + already-split kContinuation records). The caller's block styleId values must
// index a PER-SPINE pool (v6); this writer re-derives each spine's local pool from the referenced
// styles so a CompiledContent carrying a single book-global stylePool still round-trips. The
// book-level content.chapters are partitioned back to their spines by Chapter::spineIndex.
bool writeContentBin(FsFile& out, const CompiledContent& content) {
  if (!out) return false;
  const uint32_t spineCount = static_cast<uint32_t>(content.spines.size());
  out.write(reinterpret_cast<const uint8_t*>(kMagic), 4);
  writePod(out, kVersion);
  writePod(out, content.sourceFingerprint);
  writePod(out, spineCount);
  const uint32_t indexPos = static_cast<uint32_t>(out.position());  // == kHeaderSize
  for (uint32_t i = 0; i < spineCount; ++i) writePod(out, static_cast<uint32_t>(0));  // zeroed index

  for (uint32_t si = 0; si < spineCount; ++si) {
    const SpineContent& spine = content.spines[si];
    const uint32_t spineStart = static_cast<uint32_t>(out.position());
    writePod(out, spine.firstCharOffset);
    writePod(out, static_cast<uint32_t>(spine.blocks.size()));
    writePod(out, static_cast<uint32_t>(0));  // auxOffset placeholder

    // Re-map each block's styleId into a spine-local pool (v6 self-contained styles). The source
    // CompiledContent may carry a book-global stylePool; we intern only the styles this spine uses.
    // v7: capture one BlockOffset per LOGICAL block (a base record, not a kContinuation) — file
    // position + charOffset + record index — for the reader's O(1) seekToBlock. Mirrors
    // ContentBinWriter's per-onBlock capture so both writers produce the same table.
    std::vector<CssStyle> spineStyles;
    std::vector<BlockOffset> blockOffsets;
    uint32_t recordIndex = 0;
    for (Block b : spine.blocks) {
      const CssStyle& s =
          (b.styleId < content.stylePool.size()) ? content.stylePool[b.styleId] : CssStyle{};
      b.styleId = internStyle(spineStyles, s);
      const bool isContinuation = (b.flags & kContinuation) != 0 && b.type == BlockType::Text;
      if (!isContinuation) {
        blockOffsets.push_back(BlockOffset{static_cast<uint32_t>(out.position()), b.charOffset, recordIndex});
      }
      writeBlock(out, b);
      ++recordIndex;
    }
    const uint32_t auxOffset = static_cast<uint32_t>(out.position());
    writeAnchors(out, spine.anchors);
    writeLabels(out, spine.pageBreakLabels);
    writeStylePool(out, spineStyles);
    // This spine's chapters, in book order, re-based to this spine.
    std::vector<Chapter> spineChapters;
    for (const Chapter& ch : content.chapters)
      if (ch.spineIndex == static_cast<uint16_t>(si)) spineChapters.push_back(ch);
    writeChapters(out, spineChapters);
    writeBlockOffsets(out, blockOffsets);  // v7: after chapters, matching BlockStreamReader::openSpine

    const uint32_t afterAux = static_cast<uint32_t>(out.position());
    if (!out.seekSet(spineStart + 2 * sizeof(uint32_t))) return false;  // skip firstCharOffset+blockCount
    writePod(out, auxOffset);
    if (!out.seekSet(indexPos + si * sizeof(uint32_t))) return false;   // commit index slot si
    writePod(out, spineStart);
    out.seekSet(afterAux);
  }
  return static_cast<bool>(out);
}

bool readContentBin(FsFile& in, CompiledContent& content) {
  content.stylePool.clear();
  content.spines.clear();
  content.chapters.clear();
  content.sourceFingerprint = 0;

  BlockStreamReader r;
  if (!r.open(in)) return false;
  content.sourceFingerprint = r.fingerprint();

  const uint32_t spineCount = r.spineCount();
  content.spines.resize(spineCount);
  for (uint32_t si = 0; si < spineCount; ++si) {
    SpineContent& spine = content.spines[si];
    if (!r.openSpine(si)) return false;
    spine.firstCharOffset = r.spineFirstCharOffset();
    spine.anchors = r.spineAnchors();  // openSpine pre-loaded these
    spine.pageBreakLabels = r.spineLabels();
    // v6: styles + chapters are per-spine. Flatten them back into the book-global CompiledContent
    // shape: re-base each block's styleId into content.stylePool (deduped across spines) and collect
    // the spine's chapters into content.chapters.
    const auto& spineStyles = r.spineStylePool();
    std::vector<uint16_t> remap(spineStyles.size(), 0);
    for (size_t k = 0; k < spineStyles.size(); ++k) remap[k] = internStyle(content.stylePool, spineStyles[k]);
    // Whole-book read keeps the RAW on-disk records (kContinuation splits stored as-is), so read
    // records directly rather than the merged logical blocks.
    Block b;
    while (r.nextRawRecord(b)) {
      if (b.styleId < remap.size()) b.styleId = remap[b.styleId];
      spine.blocks.push_back(std::move(b));
    }
    if (!r.ok()) return false;
    for (const Chapter& ch : r.spineChapters()) content.chapters.push_back(ch);
  }
  return true;
}

bool styleEquals(const CssStyle& a, const CssStyle& b) {
  const auto lenEq = [](const CssLength& x, const CssLength& y) { return x.value == y.value && x.unit == y.unit; };
  const auto definedEq = [](const CssPropertyFlags& x, const CssPropertyFlags& y) {
    return packDefined(x) == packDefined(y);
  };
  return a.textAlign == b.textAlign && a.fontStyle == b.fontStyle && a.fontWeight == b.fontWeight &&
         a.textDecoration == b.textDecoration && a.display == b.display && a.verticalAlign == b.verticalAlign &&
         a.listStyleNone == b.listStyleNone && a.pageBreakBefore == b.pageBreakBefore &&
         a.pageBreakAfter == b.pageBreakAfter && a.cssFloat == b.cssFloat && a.smallCaps == b.smallCaps &&
         a.lineHeightMultiplier == b.lineHeightMultiplier && a.fontSizeMultiplier == b.fontSizeMultiplier &&
         lenEq(a.textIndent, b.textIndent) && lenEq(a.marginTop, b.marginTop) &&
         lenEq(a.marginBottom, b.marginBottom) && lenEq(a.marginLeft, b.marginLeft) &&
         lenEq(a.marginRight, b.marginRight) && lenEq(a.paddingTop, b.paddingTop) &&
         lenEq(a.paddingBottom, b.paddingBottom) && lenEq(a.paddingLeft, b.paddingLeft) &&
         lenEq(a.paddingRight, b.paddingRight) && lenEq(a.imageHeight, b.imageHeight) &&
         lenEq(a.imageWidth, b.imageWidth) && definedEq(a.defined, b.defined);
}

uint16_t internStyle(std::vector<CssStyle>& pool, const CssStyle& style) {
  for (size_t i = 0; i < pool.size(); ++i) {
    if (styleEquals(pool[i], style)) return static_cast<uint16_t>(i);
  }
  pool.push_back(style);
  return static_cast<uint16_t>(pool.size() - 1);
}

uint16_t internStyle(CompiledContent& content, const CssStyle& style) {
  return internStyle(content.stylePool, style);
}

}  // namespace compiled
