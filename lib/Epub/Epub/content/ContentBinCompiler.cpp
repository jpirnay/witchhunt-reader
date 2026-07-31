#include "ContentBinCompiler.h"

#include <Epub/Page.h>  // full Page type for LaidOutPage's unique_ptr<Page> (returned by value)
#include <HalStorage.h>
#include <Logging.h>
#include <esp_system.h>

#if EPUB_STAGE1

namespace compiled {

namespace {
constexpr char kTag[] = "CBC";
}

ContentBinCompiler::~ContentBinCompiler() {
  // The sidecar is transient scratch — never leave it behind (it was spliced into content.bin at each
  // spine end). On an incomplete/dropped compile the next run re-opens it truncating anyway.
  removeBlockOffsetSidecar();
}

void ContentBinCompiler::removeBlockOffsetSidecar() {
  const std::string p = epub_->getCachePath() + "/blockoff.tmp";
  if (Storage.exists(p.c_str())) Storage.remove(p.c_str());
}

bool ContentBinCompiler::ensureOpen() {
  const std::string binPath = epub_->getCachePath() + "/content.bin";
  { const std::string dir = epub_->getCachePath(); Storage.mkdir(dir.c_str()); }
  spineCount_ = static_cast<uint32_t>(epub_->getSpineItemsCount());
  if (spineCount_ == 0) return false;

  uint64_t fingerprint = 0;
  epub_->zipContentFingerprint(&fingerprint);

  // RESUME: reopen a fingerprint-matching content.bin and continue from the first uncommitted spine.
  // A committed slot's data is durable (two-phase commit), so skipping committed spines is safe.
  bool resumed = false;
  if (Storage.exists(binPath.c_str()) && Storage.openFileForReadWrite("CBC", binPath, binFile_)) {
    if (writer_.openExisting(binFile_, spineCount_, fingerprint)) {
      resumed = true;
    } else {
      binFile_.close();  // stale/foreign/corrupt — fall through to a fresh begin()
    }
  }
  if (!resumed) {
    if (!Storage.openFileForWrite("CBC", binPath, binFile_)) {
      LOG_ERR(kTag, "cannot open %s for write", binPath.c_str());
      return false;
    }
    if (!writer_.begin(binFile_, spineCount_, fingerprint)) {
      binFile_.close();
      Storage.remove(binPath.c_str());
      return false;
    }
  }
  // The reader publishes each spine explicitly on a clean Done (the content-only tee lifecycle calls
  // commitSpine only when the parse was not degraded/truncated) — so a bad parse never publishes.
  writer_.setAutoCommit(false);
  // R1: stream the per-block offset table to a sidecar (O(1) resident) so a giant single spine never
  // accumulates ~48 KB of offsets in RAM — the reader-context OOM this prevents.
  writer_.setBlockOffsetTempPath(epub_->getCachePath() + "/blockoff.tmp");

  spineCursor_ = 0;
  advanceToUncommitted();
  LOG_INF(kTag, "content.bin %s: %u spines, resuming at spine %u (free=%lu)", resumed ? "resume" : "fresh", spineCount_,
          spineCursor_, static_cast<unsigned long>(esp_get_free_heap_size()));
  return true;
}

void ContentBinCompiler::advanceToUncommitted() {
  while (spineCursor_ < spineCount_ && writer_.spineCommitted(spineCursor_)) ++spineCursor_;
  // committedFrontier_ = the lowest uncommitted index. Committed spines are always a prefix here
  // (we compile in order), so the cursor is the frontier.
  committedFrontier_ = spineCursor_;
}

void ContentBinCompiler::deleteSpineTransients(uint32_t spineIndex) {
  // The book-keyed unzipped-HTML temp is content.bin's transient input; once the spine is committed it
  // is no longer needed and would clutter the cache (thousands for a big book). Best-effort delete.
  Section probe(epub_, static_cast<int>(spineIndex), renderer_);
  probe.removeHtmlCache();
}

ContentBinCompiler::Step ContentBinCompiler::step(uint32_t budgetMs) {
  if (state_ == State::Done) return Step::Done;
  if (state_ == State::Failed) return Step::Failed;

  if (state_ == State::Init) {
    if (!ensureOpen()) {
      state_ = State::Failed;
      return Step::Failed;
    }
    state_ = State::Compiling;
  }

  // Whole book already covered?
  if (spineCursor_ >= spineCount_) {
    writer_.finish();
    binFile_.close();
    removeBlockOffsetSidecar();
    state_ = State::Done;
    LOG_INF(kTag, "content.bin complete (%u spines)", spineCount_);
    return Step::Done;
  }

  // Start a spine build if none is in flight for the current cursor.
  if (!spine_ || !spine_->hasActiveBuild()) {
    spine_ = std::make_unique<Section>(epub_, static_cast<int>(spineCursor_), renderer_);
    if (!spine_) {
      state_ = State::Failed;
      return Step::Failed;
    }
    // Content-ONLY: stream this spine's blocks to content.bin, no section pages / no section file.
    // Commit is published on a clean Done by the tee lifecycle (setAutoCommit(false) above).
    spine_->setContentBinContentOnly(&writer_, spineCursor_);
    spine_->setExternalBuildScratch(externalScratch_);
  }

  // Advance the in-flight spine by one budgeted slice (skipEviction: we don't touch the variant cache).
  const Section::BuildStep bs = spine_->stepSectionBuild(params_, budgetMs, {}, /*skipEviction=*/true);
  switch (bs) {
    case Section::BuildStep::More:
    case Section::BuildStep::Setup:
    case Section::BuildStep::Parse:
    case Section::BuildStep::Finalize:
      return Step::More;  // this spine still has work

    case Section::BuildStep::Done: {
      const uint32_t justDone = spineCursor_;
      // CSS COMPLETION HAS PRECEDENCE: a spine that finished DEGRADED (CSS lookups skipped mid-build
      // because heap dipped) or TRUNCATED has blocks missing their resolved styles — it must NOT become
      // a permanent content.bin gap. Read the Section's own degraded flags (before reset()); a clean
      // build has neither set and commits normally (byte-identical to the whole-book compile — the host
      // contract). A degraded build: DON'T advance the cursor — retry the SAME spine on a later slice,
      // when the reader's borrowed-arena + CSS heap-floor gate lets it build clean. Bound the retries so
      // a spine that can NEVER build clean (malformed, not a heap transient) doesn't stall forever —
      // after the cap, skip it (uncommitted gap) and move on.
      const bool degraded = spine_ && (spine_->isCssLowHeapDegraded() || spine_->isTruncatedCache());
      spine_.reset();
      if (degraded) {
        if (++degradedRetries_ < kMaxDegradedRetriesPerSpine) {
          LOG_INF(kTag, "spine %u degraded; retry %u/%u (free=%lu)", justDone, degradedRetries_,
                  kMaxDegradedRetriesPerSpine, static_cast<unsigned long>(esp_get_free_heap_size()));
          return Step::More;  // same spineCursor_ — the next step() rebuilds this spine
        }
        LOG_ERR(kTag, "spine %u still degraded after %u retries; skipping (content.bin gap)", justDone,
                kMaxDegradedRetriesPerSpine);
      }
      degradedRetries_ = 0;  // reset for the next spine (clean, or skipped after the cap)
      if (writer_.spineCommitted(justDone)) deleteSpineTransients(justDone);
      spineCursor_ = justDone + 1;
      advanceToUncommitted();
      if (spineCursor_ >= spineCount_) {
        writer_.finish();
        binFile_.close();
        removeBlockOffsetSidecar();
        state_ = State::Done;
        LOG_INF(kTag, "content.bin complete (%u spines, free=%lu)", spineCount_,
                static_cast<unsigned long>(esp_get_free_heap_size()));
        return Step::Done;
      }
      return Step::More;
    }

    case Section::BuildStep::Failed: {
      // A single spine's build failed (transient: heap/arena state at that moment — the reader's own
      // rebuild of the same spine often succeeds right after). Do NOT terminate the whole compile:
      // RETRY the spine a bounded number of times, then SKIP it (uncommitted gap) and continue to the
      // next — the same discipline as a degraded Done. Terminating on one failure stranded content.bin
      // at a tiny committed prefix (device-observed: spine 4 failed → CBC gave up on a 1732-spine book).
      const uint32_t justFailed = spineCursor_;
      spine_.reset();
      if (++degradedRetries_ < kMaxDegradedRetriesPerSpine) {
        LOG_INF(kTag, "spine %u compile failed; retry %u/%u (free=%lu)", justFailed, degradedRetries_,
                kMaxDegradedRetriesPerSpine, static_cast<unsigned long>(esp_get_free_heap_size()));
        return Step::More;  // same spineCursor_ — rebuild this spine next slice
      }
      LOG_ERR(kTag, "spine %u failed after %u retries; skipping (content.bin gap)", justFailed,
              kMaxDegradedRetriesPerSpine);
      degradedRetries_ = 0;
      spineCursor_ = justFailed + 1;
      advanceToUncommitted();
      if (spineCursor_ >= spineCount_) {
        writer_.finish();
        binFile_.close();
        removeBlockOffsetSidecar();
        state_ = State::Done;
        LOG_INF(kTag, "content.bin complete (%u spines, free=%lu)", spineCount_,
                static_cast<unsigned long>(esp_get_free_heap_size()));
        return Step::Done;
      }
      return Step::More;
    }
  }
  return Step::More;
}

LaidOutPage ContentBinCompiler::readPageAt(const PagePosition& cursor, const LayoutParams& params,
                                           GfxRenderer& renderer) {
  LaidOutPage result;  // ok=false by default
  // The producer must have content.bin open (it does from the first step through Done). Read the page
  // THROUGH that handle under withReadableFile, which flushes the writer, lets us seek/read, then
  // restores the append cursor — so this never disturbs the in-progress compile.
  if (!binFile_) return result;
  writer_.withReadableFile([&](FsFile& f) {
    BlockStreamReader reader;
    if (!reader.open(f)) return;
    if (cursor.spineIndex >= reader.spineCount() || !reader.spineAvailable(cursor.spineIndex)) return;
    result = layoutPage(reader, renderer, params, cursor);
  });
  return result;
}

}  // namespace compiled

#endif  // EPUB_STAGE1
