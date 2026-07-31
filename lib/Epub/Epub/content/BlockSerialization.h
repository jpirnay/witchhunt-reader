#pragma once
// Per-block (and per-spine-aux) serialization primitives for the WBC1 content.bin format, shared by
// the whole-book writer/reader (CompiledContent.cpp) and the STREAMING writer/reader
// (ContentBinWriter / BlockStreamReader). Factoring these out keeps the on-disk block encoding in
// ONE place so the streaming path and the whole-book path can never drift.
//
// A "block record" here is exactly what writeContentBin emits per Block: type + styleId + flags +
// charOffset + the type-specific body + the shared footnote/xpath tail. No spine framing — the
// caller owns spine headers, anchor/label sections, style pool, index, and chapters.

#include <BufferedFileIO.h>  // serialization::BufferedFileWriter (buffered compile-path sink)
#include <HalStorage.h>      // FsFile

#include <functional>

#include "CompiledContent.h"

namespace compiled {

// A TEXT block whose serialized body would exceed the 8 KB record cap (or the MAX_STRING_LENGTH text
// cap) is split into continuation records at WRITE time — a read-time memory bound (microreader
// MrbConverter rationale). `emit` is called for each resulting record in order: the first keeps the
// block's flags + inline image + xpath; the rest are flagged kContinuation. Footnotes are
// distributed to the run containing their anchor word (wordIndex rebased per run). A block with ≤1
// word or already within the cap is emitted whole (one `emit` call). Shared by the whole-book
// ContentSink and the streaming ContentBinWriter so the on-disk split is identical.
void splitTextBlock(Block&& block, const std::function<void(Block&&)>& emit);

// Serialize one Block record (all fields; type-dispatched body + shared tail). Returns false on I/O
// error. Mirrors readBlock exactly. Templated on the sink so the compile path can pass a
// serialization::BufferedFileWriter (batches the thousands of tiny per-word writes into few SD
// writes — the giant-spine parse's dominant cost) while the whole-book path passes an FsFile. Only
// FsFile and BufferedFileWriter are instantiated (see the explicit instantiations in the .cpp).
template <typename Sink>
bool writeBlock(Sink& out, const Block& b);

// Read one Block record written by writeBlock. Returns false on I/O error or an unknown block type
// (which would desync the stream). Mirrors writeBlock exactly.
bool readBlock(FsFile& in, Block& b);

// Serialize a spine's anchor table (count + entries). Mirrors readAnchors.
template <typename Sink>
bool writeAnchors(Sink& out, const std::vector<Anchor>& anchors);
bool readAnchors(FsFile& in, std::vector<Anchor>& anchors);

// Serialize a spine's page-break-label table (count + entries). Mirrors readLabels.
template <typename Sink>
bool writeLabels(Sink& out, const std::vector<PageBreakLabel>& labels);
bool readLabels(FsFile& in, std::vector<PageBreakLabel>& labels);

// Serialize the deduped block-style pool (count + entries). Book-level; written once.
template <typename Sink>
bool writeStylePool(Sink& out, const std::vector<CssStyle>& pool);
bool readStylePool(FsFile& in, std::vector<CssStyle>& pool);

// Serialize the book-level chapter table (count + entries).
template <typename Sink>
bool writeChapters(Sink& out, const std::vector<Chapter>& chapters);
bool readChapters(FsFile& in, std::vector<Chapter>& chapters);

// Serialize a spine's per-logical-block offset table (v7: count + entries). Baked into the aux region
// so a reader can seek to any block in O(1). Mirrors readBlockOffsets.
template <typename Sink>
bool writeBlockOffsets(Sink& out, const std::vector<BlockOffset>& offsets);
bool readBlockOffsets(FsFile& in, std::vector<BlockOffset>& offsets);

// Pack the 24 explicit-set CSS flags into one u32 (used for style equality/dedup, and by the
// style serializer). Exposed so styleEquals stays in lockstep with the serialized form.
uint32_t packDefined(const CssPropertyFlags& d);

}  // namespace compiled
