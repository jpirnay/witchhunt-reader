// Streaming content.bin (plan v2): ContentBinWriter appends one block at a time and drops it;
// BlockStreamReader reads one logical block at a time (merging 8 KB kContinuation splits). These
// tests prove the streaming path produces a valid v5 file, reconstructs the block stream, rejects a
// corrupt/truncated file, and validates the fingerprint — without ever holding a whole spine.

#include <HalStorage.h>
#include <gtest/gtest.h>
#include <process.h>  // _getpid - per-process temp isolation under parallel ctest

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "Epub/content/BlockStreamReader.h"
#include "Epub/content/CompiledContent.h"
#include "Epub/content/ContentBinWriter.h"
#include "Epub/content/ContentSink.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

using compiled::Block;
using compiled::BlockStreamReader;
using compiled::CompiledContent;
using compiled::ContentBinWriter;
using compiled::ContentSink;

std::string freshDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / ("content_bin_stream_" + std::to_string(_getpid())) / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

std::string corpus(const char* book) { return std::string(CORPUS_DIR) + "/" + book; }

// Build a whole book's CompiledContent (via the real producer) so we have a realistic block stream
// to re-drive through the STREAMING writer.
bool compileWhole(const char* book, const std::string& cacheDir, ContentSink& sink) {
  std::ostringstream log;
  return pipeline_harness::compileContent(corpus(book), cacheDir, pipeline_harness::Profile{}, sink, log);
}

// Drive a ContentBinWriter from an already-built CompiledContent, faithfully replaying the per-block
// side data (footnotes/xpath) that onBlock expects to arrive before the block.
bool streamWrite(const CompiledContent& c, const std::string& path, uint64_t fingerprint) {
  FsFile f;
  if (!f.openForWrite(path)) return false;
  ContentBinWriter w;
  if (!w.begin(f, static_cast<uint32_t>(c.spines.size()), fingerprint)) return false;
  for (const auto& spine : c.spines) {
    w.beginSpine();
    for (const auto& b : spine.blocks) {
      // Replay the block's own footnotes/xpath as the walk would (before onBlock).
      if (b.hasXPath) w.onXPathAdvance(b.xpath.paragraphIndex, b.xpath.listItemIndex, b.xpath.bodyChildByteOffset);
      for (const auto& fn : b.footnotes) w.onFootnote(static_cast<int>(fn.wordIndex), fn.entry);
      Block copy = b;
      copy.footnotes.clear();  // onBlock re-attaches from the pending buffer we just filled
      copy.hasXPath = false;
      const CssStyle& style = (b.styleId < c.stylePool.size()) ? c.stylePool[b.styleId] : CssStyle{};
      w.onBlock(std::move(copy), style);
    }
    for (const auto& a : spine.anchors) w.onAnchor(a.id);
    for (const auto& pl : spine.pageBreakLabels) w.onPageBreakLabel(pl.label);
    w.onSpineEnd();
  }
  const bool ok = w.finish();
  f.close();
  return ok;
}

}  // namespace

// The streaming writer + reader reproduce the LOGICAL block stream (continuation records merged
// back). We compare against the whole-book CompiledContent read via readContentBin, which stores
// the RAW split records — so we re-coalesce the whole-book side the same way for comparison.
TEST(ContentBinStream, WriteThenLogicalReadReconstructsBlocks) {
  const std::string dir = freshDir("roundtrip");
  ContentSink sink;
  ASSERT_TRUE(compileWhole("test_headings.epub", dir, sink));
  const CompiledContent& built = sink.content();

  const std::string bin = dir + "/content.bin";
  ASSERT_TRUE(streamWrite(built, bin, 0x1234'5678'9abc'def0ull));

  FsFile in;
  ASSERT_TRUE(in.openForRead(bin));
  BlockStreamReader r;
  ASSERT_TRUE(r.open(in));
  EXPECT_EQ(r.fingerprint(), 0x1234'5678'9abc'def0ull);
  ASSERT_EQ(r.spineCount(), built.spines.size());

  for (uint32_t si = 0; si < r.spineCount(); ++si) {
    ASSERT_TRUE(r.openSpine(si));
    // Reconstruct the built spine's LOGICAL blocks (merge kContinuation) for comparison.
    std::vector<std::string> builtLogicalText;
    for (const auto& b : built.spines[si].blocks) {
      if ((b.flags & compiled::kContinuation) != 0 && !builtLogicalText.empty()) {
        builtLogicalText.back() += b.text;
      } else {
        builtLogicalText.push_back(b.text);
      }
    }
    std::vector<std::string> streamedText;
    Block b;
    while (r.nextLogicalBlock(b)) streamedText.push_back(b.text);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(streamedText.size(), builtLogicalText.size()) << "spine " << si << " logical block count";
    for (size_t i = 0; i < streamedText.size(); ++i) {
      EXPECT_EQ(streamedText[i], builtLogicalText[i]) << "spine " << si << " block " << i;
    }
    // After the block stream, the aux tables must read back.
    std::vector<compiled::Anchor> anchors;
    std::vector<compiled::PageBreakLabel> labels;
    ASSERT_TRUE(r.readSpineAux(anchors, labels));
    EXPECT_EQ(anchors.size(), built.spines[si].anchors.size());
    EXPECT_EQ(labels.size(), built.spines[si].pageBreakLabels.size());
  }
  in.close();
}

// Footnote-bearing book: footnotes must survive the streaming round-trip on the LOGICAL block.
TEST(ContentBinStream, FootnotesSurviveStreaming) {
  const std::string dir = freshDir("footnotes");
  ContentSink sink;
  ASSERT_TRUE(compileWhole("test_inline_footnotes.epub", dir, sink));
  const CompiledContent& built = sink.content();
  size_t builtFootnotes = 0;
  for (const auto& s : built.spines)
    for (const auto& b : s.blocks) builtFootnotes += b.footnotes.size();
  ASSERT_EQ(builtFootnotes, 3u);

  const std::string bin = dir + "/content.bin";
  ASSERT_TRUE(streamWrite(built, bin, 42));

  FsFile in;
  ASSERT_TRUE(in.openForRead(bin));
  BlockStreamReader r;
  ASSERT_TRUE(r.open(in));
  size_t streamedFootnotes = 0;
  for (uint32_t si = 0; si < r.spineCount(); ++si) {
    ASSERT_TRUE(r.openSpine(si));
    Block b;
    while (r.nextLogicalBlock(b)) streamedFootnotes += b.footnotes.size();
  }
  EXPECT_EQ(streamedFootnotes, 3u) << "the fixture's three footnotes must survive streaming";
  in.close();
}

// v7: the baked per-spine block-offset table gives O(1) seekToBlock. Prove (a) the table has exactly
// one entry per LOGICAL block, (b) seeking to any block N and reading forward yields the SAME blocks
// as a sequential read from N, and (c) the table's charOffset matches the block's own charOffset.
TEST(ContentBinStream, SeekToBlockMatchesSequentialRead) {
  const std::string dir = freshDir("seek");
  ContentSink sink;
  ASSERT_TRUE(compileWhole("test_headings.epub", dir, sink));
  const CompiledContent& built = sink.content();

  const std::string bin = dir + "/content.bin";
  ASSERT_TRUE(streamWrite(built, bin, 0xFEED'FACE'0000'0001ull));

  FsFile in;
  ASSERT_TRUE(in.openForRead(bin));
  BlockStreamReader r;
  ASSERT_TRUE(r.open(in));

  for (uint32_t si = 0; si < r.spineCount(); ++si) {
    ASSERT_TRUE(r.openSpine(si));

    // (a) Sequentially read all LOGICAL blocks (text + first-record index + char offset), which also
    // establishes the ground truth we seek against.
    struct Ref {
      std::string text;
      uint32_t firstRecord;
      uint32_t charOffset;
    };
    std::vector<Ref> seq;
    Block b;
    while (r.nextLogicalBlock(b)) {
      seq.push_back(Ref{b.text, r.currentFirstRecordIndex(), b.charOffset});
    }
    ASSERT_TRUE(r.ok());

    // The baked table has exactly one entry per logical block.
    ASSERT_EQ(r.spineLogicalBlockCount(), seq.size()) << "spine " << si << " logical-block count";
    for (size_t i = 0; i < seq.size(); ++i) {
      compiled::BlockOffset bo;
      ASSERT_TRUE(r.blockOffsetAt(static_cast<uint32_t>(i), &bo)) << "spine " << si << " blockOffsetAt " << i;
      EXPECT_EQ(bo.recordIndex, seq[i].firstRecord) << "spine " << si << " table recordIndex " << i;
      EXPECT_EQ(bo.charOffset, seq[i].charOffset) << "spine " << si << " table charOffset " << i;
    }

    // (b) For every start block N, seekToBlock(N) then a forward read must reproduce seq[N..].
    for (uint32_t start = 0; start < seq.size(); ++start) {
      ASSERT_TRUE(r.seekToBlock(start)) << "spine " << si << " seekToBlock(" << start << ")";
      for (uint32_t j = start; j < seq.size(); ++j) {
        Block bj;
        ASSERT_TRUE(r.nextLogicalBlock(bj)) << "spine " << si << " seek " << start << " read " << j;
        EXPECT_EQ(r.currentFirstRecordIndex(), seq[j].firstRecord)
            << "spine " << si << " seek " << start << " block " << j << " record index";
        EXPECT_EQ(bj.text, seq[j].text) << "spine " << si << " seek " << start << " block " << j << " text";
        EXPECT_EQ(bj.charOffset, seq[j].charOffset) << "spine " << si << " seek " << start << " block " << j << " char";
      }
      // Reading past the end returns false, exactly like a sequential drain.
      Block tail;
      EXPECT_FALSE(r.nextLogicalBlock(tail)) << "spine " << si << " seek " << start << " must end at spine end";
    }

    // Out-of-range seek is rejected, not silently clamped.
    EXPECT_FALSE(r.seekToBlock(static_cast<uint32_t>(seq.size())));
  }
  in.close();
}

// v6: a partially-written file (header + zeroed index, no spines committed) OPENS cleanly — that is
// the frontier model, not corruption — but every spine reports !spineAvailable(). A truncated file
// whose committed offsets point past EOF must still be rejected at open.
TEST(ContentBinStream, PartialFileOpensButSpinesUnavailable) {
  const std::string dir = freshDir("corrupt");
  ContentSink sink;
  ASSERT_TRUE(compileWhole("test_headings.epub", dir, sink));

  // (1) A file whose header + zeroed index were written but no spine committed (interrupted before
  // the first onSpineEnd). v6 accepts it (partial = legal); no spine is available yet.
  const std::string unfinished = dir + "/unfinished.bin";
  const uint32_t spineCount = static_cast<uint32_t>(sink.content().spines.size());
  {
    FsFile f;
    ASSERT_TRUE(f.openForWrite(unfinished));
    ContentBinWriter w;
    ASSERT_TRUE(w.begin(f, spineCount, 7));
    // ... deliberately NO spines committed ...
    f.close();
  }
  {
    FsFile in;
    ASSERT_TRUE(in.openForRead(unfinished));
    BlockStreamReader r;
    EXPECT_TRUE(r.open(in)) << "v6 partial file (zeroed index) must open — it is the frontier, not stale";
    EXPECT_EQ(r.spineCount(), spineCount);
    for (uint32_t si = 0; si < spineCount; ++si)
      EXPECT_FALSE(r.spineAvailable(si)) << "no spine committed yet → spine " << si << " unavailable";
    EXPECT_FALSE(r.openSpine(0)) << "openSpine on an uncommitted slot must fail";
    in.close();
  }

  // (2) A valid file truncated mid-stream. v6 puts the spine-offset index at the FRONT (right after
  // the header), so a mid-file chop leaves the index readable; open() may succeed. Corruption is then
  // caught either at open (a committed offset now past EOF) OR when the affected spine is read
  // (openSpine seek / nextLogicalBlock read fails). Assert the reader never returns garbage: it must
  // fail cleanly at SOME point rather than yielding a full, valid replay of the truncated book.
  const std::string full = dir + "/full.bin";
  ASSERT_TRUE(streamWrite(sink.content(), full, 9));
  const auto fullSize = fs::file_size(full);
  const std::string truncated = dir + "/truncated.bin";
  {
    std::string bytes(fullSize, '\0');
    FILE* fp = fopen(full.c_str(), "rb");
    ASSERT_TRUE(fp);
    ASSERT_EQ(fread(&bytes[0], 1, fullSize, fp), fullSize);
    fclose(fp);
    // Chop just past the header + spine index so spine 0's own data is guaranteed truncated,
    // regardless of book size (a /2 chop could leave a small book's spines fully intact).
    const size_t cut = compiled::kHeaderSize + static_cast<size_t>(sink.content().spines.size()) * sizeof(uint32_t) + 8;
    bytes.resize(std::min<size_t>(bytes.size(), cut));
    FILE* out = fopen(truncated.c_str(), "wb");
    ASSERT_TRUE(out);
    fwrite(bytes.data(), 1, bytes.size(), out);
    fclose(out);
  }
  {
    FsFile in;
    ASSERT_TRUE(in.openForRead(truncated));
    BlockStreamReader r;
    bool cleanlyDetected = false;
    if (!r.open(in)) {
      cleanlyDetected = true;  // offset past EOF caught at open
    } else {
      // open() accepted the front-loaded index; a full drain of every AVAILABLE spine must hit an
      // error before completing (a spine's blocks/aux were chopped off).
      for (uint32_t si = 0; si < r.spineCount() && !cleanlyDetected; ++si) {
        if (!r.spineAvailable(si)) continue;
        if (!r.openSpine(si)) {
          cleanlyDetected = true;
          break;
        }
        Block b;
        while (r.nextLogicalBlock(b)) { /* drain */
        }
        if (!r.ok()) cleanlyDetected = true;
      }
    }
    EXPECT_TRUE(cleanlyDetected)
        << "a truncated file must be detected at open or while reading, not silently replayed whole";
    in.close();
  }
}

// Increment F split write/commit: with setAutoCommit(false), onSpineEnd writes a spine's DATA but does
// NOT publish its index slot — an explicit commitSpine() does. A spine written-but-not-committed stays
// unavailable, so the reader's tee build can withhold a degraded/truncated parse from content.bin.
TEST(ContentBinStream, DeferredCommitGatesAvailability) {
  const std::string dir = freshDir("defercommit");
  ContentSink sink;
  ASSERT_TRUE(compileWhole("test_text_rendering.epub", dir, sink));
  const CompiledContent& built = sink.content();
  ASSERT_GE(built.spines.size(), 2u);

  const std::string bin = dir + "/content.bin";
  {
    FsFile f;
    ASSERT_TRUE(f.openForWrite(bin));
    ContentBinWriter w;
    w.setAutoCommit(false);  // caller publishes spines explicitly
    ASSERT_TRUE(w.begin(f, static_cast<uint32_t>(built.spines.size()), 55));
    for (size_t si = 0; si < built.spines.size(); ++si) {
      const auto& spine = built.spines[si];
      w.beginSpineAt(static_cast<uint32_t>(si));
      for (const auto& b : spine.blocks) {
        if (b.hasXPath) w.onXPathAdvance(b.xpath.paragraphIndex, b.xpath.listItemIndex, b.xpath.bodyChildByteOffset);
        for (const auto& fn : b.footnotes) w.onFootnote(static_cast<int>(fn.wordIndex), fn.entry);
        Block copy = b;
        copy.footnotes.clear();
        copy.hasXPath = false;
        const CssStyle& style = (b.styleId < built.stylePool.size()) ? built.stylePool[b.styleId] : CssStyle{};
        w.onBlock(std::move(copy), style);
      }
      for (const auto& a : spine.anchors) w.onAnchor(a.id);
      for (const auto& pl : spine.pageBreakLabels) w.onPageBreakLabel(pl.label);
      w.onSpineEnd();                 // writes DATA; does NOT commit (autoCommit off)
      if (si == 0) w.commitSpine(0);  // publish ONLY spine 0
      // spine 1+ data is written but deliberately left uncommitted
    }
    ASSERT_TRUE(w.finish());
    f.close();
  }

  FsFile in;
  ASSERT_TRUE(in.openForRead(bin));
  BlockStreamReader r;
  ASSERT_TRUE(r.open(in));
  EXPECT_TRUE(r.spineAvailable(0)) << "explicitly committed spine is available";
  EXPECT_TRUE(r.openSpine(0)) << "committed spine replays";
  for (uint32_t si = 1; si < r.spineCount(); ++si)
    EXPECT_FALSE(r.spineAvailable(si)) << "written-but-uncommitted spine " << si << " must stay unavailable";
  in.close();
}

// Increment F cross-session append: a writer opened with openExisting() on a matching content.bin
// keeps the already-committed spines and appends new ones at EOF, so a second session extends coverage
// instead of truncating. Emulates the reader reopening a book: session 1 commits spine 0, session 2
// (openExisting) commits spine 1; both must be available + replayable afterwards.
TEST(ContentBinStream, OpenExistingAppendsKeepingCommittedSpines) {
  const std::string dir = freshDir("append");
  ContentSink sink;
  ASSERT_TRUE(compileWhole("test_text_rendering.epub", dir, sink));
  const CompiledContent& built = sink.content();
  ASSERT_GE(built.spines.size(), 2u);
  const uint32_t spineCount = static_cast<uint32_t>(built.spines.size());
  const uint64_t fp = 0xABCD'1234'5678'9999ull;
  const std::string bin = dir + "/content.bin";

  const auto writeSpine = [&](ContentBinWriter& w, uint32_t si) {
    w.beginSpineAt(si);
    for (const auto& b : built.spines[si].blocks) {
      if (b.hasXPath) w.onXPathAdvance(b.xpath.paragraphIndex, b.xpath.listItemIndex, b.xpath.bodyChildByteOffset);
      for (const auto& fn : b.footnotes) w.onFootnote(static_cast<int>(fn.wordIndex), fn.entry);
      Block copy = b;
      copy.footnotes.clear();
      copy.hasXPath = false;
      const CssStyle& style = (b.styleId < built.stylePool.size()) ? built.stylePool[b.styleId] : CssStyle{};
      w.onBlock(std::move(copy), style);
    }
    for (const auto& a : built.spines[si].anchors) w.onAnchor(a.id);
    for (const auto& pl : built.spines[si].pageBreakLabels) w.onPageBreakLabel(pl.label);
    w.onSpineEnd();
  };

  // Session 1: fresh begin(), commit ONLY spine 0.
  {
    FsFile f;
    ASSERT_TRUE(f.openForWrite(bin));
    ContentBinWriter w;
    ASSERT_TRUE(w.begin(f, spineCount, fp));
    writeSpine(w, 0);
    ASSERT_TRUE(w.finish());
    f.close();
  }
  // Session 2: openExisting() on the same file, append + commit spine 1. Spine 0 stays committed.
  {
    FsFile f;
    ASSERT_TRUE(Storage.openFileForReadWrite("TST", bin, f));
    ContentBinWriter w;
    ASSERT_TRUE(w.openExisting(f, spineCount, fp)) << "matching-fingerprint file must reopen for append";
    EXPECT_TRUE(w.spineCommitted(0)) << "prior-session spine 0 seen as committed";
    EXPECT_FALSE(w.spineCommitted(1));
    writeSpine(w, 1);
    ASSERT_TRUE(w.finish());
    f.close();
  }
  // Both spines are now available and replay.
  FsFile in;
  ASSERT_TRUE(in.openForRead(bin));
  BlockStreamReader r;
  ASSERT_TRUE(r.open(in));
  EXPECT_TRUE(r.spineAvailable(0)) << "spine 0 survived the append";
  EXPECT_TRUE(r.spineAvailable(1)) << "spine 1 was appended + committed";
  EXPECT_TRUE(r.openSpine(0));
  {
    Block b;
    size_t n = 0;
    while (r.nextRawRecord(b)) ++n;
    EXPECT_GT(n, 0u);
  }
  EXPECT_TRUE(r.openSpine(1));
  {
    Block b;
    size_t n = 0;
    while (r.nextRawRecord(b)) ++n;
    EXPECT_GT(n, 0u);
  }
  EXPECT_TRUE(r.ok());
  in.close();

  // openExisting with a MISMATCHED fingerprint must refuse (caller then truncate+begin()s).
  {
    FsFile f;
    ASSERT_TRUE(Storage.openFileForReadWrite("TST", bin, f));
    ContentBinWriter w;
    EXPECT_FALSE(w.openExisting(f, spineCount, fp ^ 0x1)) << "fingerprint mismatch must be rejected";
    f.close();
  }
}
