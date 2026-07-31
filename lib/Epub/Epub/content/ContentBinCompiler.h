#pragma once

// ContentBinCompiler — the fresh reader's incremental, resumable, memory-bounded background compiler
// for <cachePath>/content.bin. It walks the book spine-by-spine, driving the SAME proven sliced
// build machinery the reader's Background-B/-C use (Section::stepSectionBuild with a time budget, the
// borrowed-framebuffer build arena, the 1 KB feed-chunk yield) but in CONTENT-ONLY mode
// (Section::setContentBinContentOnly): each spine's parsed blocks stream straight to content.bin with
// NO section-cache pages and NO per-settings section files (cache hygiene — a thousands-of-spines
// book must not clutter its cache dir).
//
// Resumable: it opens an existing content.bin (fingerprint-matched) and skips already-committed
// spines, continuing from the first uncommitted one to EOF. A committed slot is durable truth
// (two-phase commit), so a crash/exit/sleep mid-compile picks up from the last good spine.
//
// Driven a slice at a time from the reader's idle loop (like Background-C): call step(budgetMs) until
// it returns Done. The caller owns the memory gating (borrow the framebuffer, skip when render is
// busy) exactly as the section-build driver does, and passes the borrowed arena via
// setExternalScratch so the per-spine inflate ring never touches the fragmented heap.
//
// Behind EPUB_STAGE1.

#include <Epub.h>
#include <Epub/Section.h>
#include <Epub/content/BlockStreamReader.h>
#include <Epub/content/ContentBinWriter.h>
#include <Epub/content/PageLayout.h>

#include <memory>

class GfxRenderer;
class BuildArena;

namespace compiled {

class ContentBinCompiler {
 public:
  enum class Step : uint8_t {
    More,    // work remains — call step() again
    Done,    // all spines committed; content.bin is complete
    Failed,  // unrecoverable error; content.bin left partial (committed spines are still valid)
  };

  ContentBinCompiler(std::shared_ptr<Epub> epub, GfxRenderer& renderer, const Section::BuildParams& params)
      : epub_(std::move(epub)), renderer_(renderer), params_(params) {}
  ~ContentBinCompiler();
  ContentBinCompiler(const ContentBinCompiler&) = delete;
  ContentBinCompiler& operator=(const ContentBinCompiler&) = delete;

  // Lend the per-spine build arena (typically a BuildArena over the borrowed secondary framebuffer)
  // so the inflate ring is carved from that contiguous region, not the fragmented heap. Forwarded to
  // each per-spine Section. Set before/between step() calls; the region must outlive the compile.
  void setExternalScratch(BuildArena* scratch) { externalScratch_ = scratch; }

  // Advance the compile by at most ~budgetMs of work (0 = run a whole spine to completion in one
  // call — the blocking/host path). Returns More while spines remain, Done when content.bin is
  // complete, Failed on an unrecoverable error. Opens/resumes content.bin lazily on the first call.
  Step step(uint32_t budgetMs);

  // The lowest not-yet-committed spine index (the compile frontier). Spines [0, committedSpines())
  // are durably in content.bin. Cheap; for the reader's "is the page I want ready?" check.
  uint32_t committedSpines() const { return committedFrontier_; }
  uint32_t spineCount() const { return spineCount_; }
  bool done() const { return state_ == State::Done; }

  // True while a per-spine Section build is in flight (between the first slice that opens a spine and
  // the slice that finishes it). The reader uses this to bound a borrowed-framebuffer arena to a
  // single spine: borrow before a spine's first slice, return at the spine boundary when this goes
  // false, so the reader can reclaim the buffer between spines. False at Init/Done/Failed and at every
  // inter-spine gap.
  bool spineInFlight() const { return spine_ && spine_->hasActiveBuild(); }

  // Render ONE page from content.bin at `cursor`, reading THROUGH the producer's own open handle
  // (SdFat allows only one handle per file; the producer holds content.bin open the whole compile).
  // Safe because reader + producer run cooperatively: this flushes the writer, seeks+reads the
  // committed spine via a BlockStreamReader + compiled::layoutPage, then restores the writer's append
  // cursor (ContentBinWriter::withReadableFile). The cursor's spine MUST be committed
  // (spineAvailable / cursor.spineIndex < committedSpines()) — check before calling. Returns a
  // LaidOutPage with ok=false if the spine isn't readable or a read error occurs. This is the reader's
  // content.bin read primitive (M4): the reader asks the producer for a page, never touching the file.
  compiled::LaidOutPage readPageAt(const compiled::PagePosition& cursor, const compiled::LayoutParams& params,
                                   GfxRenderer& renderer);

 private:
  enum class State : uint8_t { Init, Compiling, Done, Failed };

  bool ensureOpen();          // lazy: open/resume content.bin, set the cursor to the first uncommitted spine
  void advanceToUncommitted();  // move spineCursor_ to the next spine whose slot is not committed
  void deleteSpineTransients(uint32_t spineIndex);  // drop the unzipped-HTML temp once a spine is committed
  void removeBlockOffsetSidecar();  // delete <cachePath>/blockoff.tmp (spliced into content.bin already)

  std::shared_ptr<Epub> epub_;
  GfxRenderer& renderer_;
  Section::BuildParams params_;
  BuildArena* externalScratch_ = nullptr;

  State state_ = State::Init;
  FsFile binFile_;                 // content.bin, kept open across ticks
  ContentBinWriter writer_;        // book-scoped; setAutoCommit(false) — commit is per clean spine Done
  std::unique_ptr<Section> spine_;  // the current spine's in-flight build (owns its BuildState)
  uint32_t spineCount_ = 0;
  uint32_t spineCursor_ = 0;        // spine currently being (or about to be) compiled
  uint32_t committedFrontier_ = 0;  // count of committed spines (== lowest uncommitted index)
  // A spine that finishes DEGRADED (CSS-skipped / truncated → not committed) is RETRIED rather than
  // skipped (CSS completion has precedence — a gap would render wrong). Bounded so a spine that can
  // never build clean doesn't stall the compile: after the cap it's skipped (uncommitted gap).
  uint32_t degradedRetries_ = 0;
  static constexpr uint32_t kMaxDegradedRetriesPerSpine = 5;
};

}  // namespace compiled
