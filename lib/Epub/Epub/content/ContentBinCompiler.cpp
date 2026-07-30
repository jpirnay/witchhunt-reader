#include "ContentBinCompiler.h"

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
      // The spine committed (or not, if degraded — but content-only + clean parse commits). Drop the
      // per-spine Section (frees its build state), clean its transient input, advance the cursor.
      const uint32_t justDone = spineCursor_;
      spine_.reset();
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

    case Section::BuildStep::Failed:
      LOG_ERR(kTag, "spine %u compile failed", spineCursor_);
      spine_.reset();
      state_ = State::Failed;
      return Step::Failed;
  }
  return Step::More;
}

}  // namespace compiled

#endif  // EPUB_STAGE1
