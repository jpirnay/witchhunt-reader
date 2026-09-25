// Image dimensions behind a large JPEG metadata block (witchhunt-reader #249).
//
// The image manifest probes the first 4 KB of an entry. A Photoshop-style export carries
// EXIF + IPTC + XMP + ICC in front of the frame header — ~29 KB in the reporter's book — so
// the probe never reaches SOF. Finding it then means a streaming walk of the DEFLATED entry
// through an inflate ring sized to the entry (up to 32 KB), which is exactly the block the
// C3 cannot promise mid-parse: the reporter's image sits on page 54 of a 58-page chapter,
// where the heap is at its most fragmented. When that ring failed, the image was cached as
// alt text for good, indistinguishable from a corrupt file.
//
// What has to hold:
//   1. A JPEG whose SOF lies beyond the probe window is DEFERRED — it is neither resolved nor
//      written off as unreadable.
//   2. A deferred image can be resolved later (resolvePending), once, and persists like any
//      other entry, so the walk is paid once per image per book.
//   3. Mid-parse, the walk runs only when contiguous heap actually covers its ring; a refusal
//      is latched as provisional (the build can be discarded and redone), never cached.
//   4. Resolving the pending images at build end makes the next build of the same section
//      clean — the mechanism the reader uses to show the image after one rebuild.
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "BuildArena.h"
#include "Epub.h"
#include "Epub/EpubImageManifest.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

const char* kBook = CORPUS_DIR "/test_jpeg_metadata_heavy.epub";
const char* kEntry = "OEBPS/images/photo.jpg";
constexpr int16_t kPhotoWidth = 64;
constexpr int16_t kPhotoHeight = 48;
// SOF 12 KB into a 53 KB entry: inside the walk's first 16 KB stage, in an entry whose full-size
// ring would be 32 KB.
const char* kBigEntry = "OEBPS/images/photo_big.jpg";
constexpr int16_t kBigWidth = 96;
constexpr int16_t kBigHeight = 64;

// A header that IS within the probe window, for the inline control case.
const char* kPngBook = CORPUS_DIR "/test_png_images.epub";
const char* kPngEntry = "OEBPS/images/scaling_test.png";

struct ImageManifestFixture : testing::Test {
  fs::path work;
  std::string cacheDir;
  uint32_t savedFree = 0;
  uint32_t savedContig = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_imgmanifest_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    cacheDir = (work / "cache").string();
    fs::create_directories(cacheDir);
    savedFree = ESP.getFreeHeap();
    savedContig = ESP.getMaxAllocHeap();
  }
  void TearDown() override {
    ESP.setFreeHeap(savedFree);
    ESP.setMaxAllocHeap(savedContig);
    fs::remove_all(work);
  }
};

// --- 1 + 2: the manifest's own contract --------------------------------------------------

TEST_F(ImageManifestFixture, SofBeyondTheProbeWindowIsDeferredNotUnreadable) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));

  const ImageManifestEntry* entry = nullptr;
  EXPECT_EQ(manifest.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Deferred)
      << "a valid JPEG whose SOF sits past the probe window is not unreadable; it needs the streaming walk";
  EXPECT_EQ(entry, nullptr);
  EXPECT_TRUE(manifest.hasPending());
  EXPECT_EQ(manifest.find(kEntry), nullptr) << "nothing may be recorded until the dimensions are known";
}

TEST_F(ImageManifestFixture, ResolvePendingWalksTheEntryOnceAndPersists) {
  {
    EpubImageManifest manifest;
    ASSERT_TRUE(manifest.load(cacheDir));
    const ImageManifestEntry* entry = nullptr;
    ASSERT_EQ(manifest.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Deferred);

    EXPECT_EQ(manifest.resolvePending(), 1u);
    EXPECT_FALSE(manifest.hasPending());
    const ImageManifestEntry* found = manifest.find(kEntry);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->width, kPhotoWidth);
    EXPECT_EQ(found->height, kPhotoHeight);
    manifest.persistIfDirty();
  }
  // A fresh load sees the persisted entry: the walk is paid once per image per book.
  EpubImageManifest reloaded;
  ASSERT_TRUE(reloaded.load(cacheDir));
  const ImageManifestEntry* entry = nullptr;
  EXPECT_EQ(reloaded.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Resolved);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->width, kPhotoWidth);
  EXPECT_EQ(entry->height, kPhotoHeight);
  EXPECT_FALSE(reloaded.hasPending());
}

TEST_F(ImageManifestFixture, ResolvePendingWalksFromAnArenaWhenTheHeapCannot) {
  // Device-measured (X3, #249): reader-time contig tops out at ~31.7 KB, so a heap ring of
  // 512 + 32 KB never fits at any moment of a session. The one region that size which IS idle
  // at a build's end is the borrowed secondary framebuffer, so the walk must be able to carve
  // its ring from there and leave the arena exactly as it found it.
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  const ImageManifestEntry* entry = nullptr;
  ASSERT_EQ(manifest.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Deferred);

  ESP.setMaxAllocHeap(16 * 1024);  // the heap path would refuse
  BuildArena arena(40 * 1024);
  ASSERT_TRUE(arena.valid());
  EXPECT_EQ(manifest.resolvePending(&arena), 1u);
  EXPECT_EQ(arena.used(), 0u) << "the walk's ring is a scoped block; nothing may linger in the arena";
  const ImageManifestEntry* found = manifest.find(kEntry);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->width, kPhotoWidth);
}

// --- the staged walk ---------------------------------------------------------------------------
//
// Every image in "Strange Pictures" (78 JPEGs, SOF 9.7-18.4 KB in) was unresolvable on the X3
// because the walk sized its ring to the ENTRY -- 32 KB, which that heap never holds mid-parse
// and, after a fragmented build, not even at the build's end (28660 contig vs 33280 needed).
// The header sits in the first 16 KB of output, and a 16 KB ring reads those bytes exactly.

TEST_F(ImageManifestFixture, AHeaderInsideTheFirstStageResolvesWithAStageSizedRing) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  const ImageManifestEntry* entry = nullptr;
  ASSERT_EQ(manifest.resolve(kBook, kBigEntry, entry), EpubImageManifest::Resolve::Deferred);

  // 20 KB: hosts a 16 KB stage ring plus its chunk, never a 32 KB one. The heap is closed off,
  // so the only way to the dimensions is the first stage from this arena.
  ESP.setMaxAllocHeap(4 * 1024);
  BuildArena arena(20 * 1024);
  ASSERT_TRUE(arena.valid());
  EXPECT_EQ(manifest.resolvePending(&arena), 1u);
  EXPECT_EQ(arena.used(), 0u);
  EXPECT_LE(arena.highWater(), 17u * 1024u) << "the first stage must not take an entry-sized ring";
  const ImageManifestEntry* found = manifest.find(kBigEntry);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->width, kBigWidth);
  EXPECT_EQ(found->height, kBigHeight);
}

TEST_F(ImageManifestFixture, TheHeapBudgetAdmitsExactlyTheStagesThatFit) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  const ImageManifestEntry* entry = nullptr;
  ASSERT_EQ(manifest.resolve(kBook, kBigEntry, entry), EpubImageManifest::Resolve::Deferred);
  ASSERT_EQ(manifest.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Deferred);

  // 18 KB of contiguous heap: the 16 KB first stage fits, so photo_big (SOF at 12 KB) resolves.
  // photo.jpg's SOF sits at 20.5 KB: its first stage ends short with the header still running,
  // and the second stage -- the whole 20.9 KB entry -- does not fit. It must be left for later,
  // not written off.
  ESP.setMaxAllocHeap(18 * 1024);
  EXPECT_EQ(manifest.resolvePending(), 1u);
  EXPECT_NE(manifest.find(kBigEntry), nullptr);
  EXPECT_EQ(manifest.find(kEntry), nullptr);

  // Re-queued by the next miss, and resolved once the heap can host its second stage.
  ASSERT_EQ(manifest.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Deferred);
  ESP.setMaxAllocHeap(100 * 1024);
  EXPECT_EQ(manifest.resolvePending(), 1u);
  const ImageManifestEntry* found = manifest.find(kEntry);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->width, kPhotoWidth);
}

TEST_F(ImageManifestFixture, ResolveDeferredNowReportsWhichStageItLacked) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  const ImageManifestEntry* entry = nullptr;
  ASSERT_EQ(manifest.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Deferred);

  // The parser's mid-parse path, with what the heap can spare passed in explicitly.
  EXPECT_EQ(manifest.resolveDeferredNow(kBook, kEntry, entry, 8 * 1024), EpubImageManifest::Walk::NeedsHeap)
      << "not even the first stage fits";
  EXPECT_EQ(manifest.resolveDeferredNow(kBook, kEntry, entry, 18 * 1024), EpubImageManifest::Walk::NeedsHeap)
      << "the first stage ran and ended with the header still open; the second does not fit";
  EXPECT_TRUE(manifest.hasPending()) << "a walk short of heap leaves the image queued";
  EXPECT_EQ(manifest.resolveDeferredNow(kBook, kEntry, entry, 24 * 1024), EpubImageManifest::Walk::Resolved);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->width, kPhotoWidth);
  EXPECT_FALSE(manifest.hasPending());
}

TEST_F(ImageManifestFixture, ResolvePendingFallsBackToTheHeapWhenTheArenaIsTooSmall) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  const ImageManifestEntry* entry = nullptr;
  ASSERT_EQ(manifest.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Deferred);

  ESP.setMaxAllocHeap(100 * 1024);
  BuildArena arena(8 * 1024);  // cannot host the ~21 KB ring
  ASSERT_TRUE(arena.valid());
  EXPECT_EQ(manifest.resolvePending(&arena), 1u);
  EXPECT_EQ(arena.used(), 0u);
  EXPECT_NE(manifest.find(kEntry), nullptr);
}

TEST_F(ImageManifestFixture, AMissingEntryIsUnreadableNotDeferred) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  const ImageManifestEntry* entry = nullptr;
  EXPECT_EQ(manifest.resolve(kBook, "OEBPS/images/absent.jpg", entry), EpubImageManifest::Resolve::Unreadable)
      << "an image that cannot exist must not be queued for a walk that can never succeed";
  EXPECT_EQ(entry, nullptr);
  EXPECT_FALSE(manifest.hasPending());
}

TEST_F(ImageManifestFixture, AJpegShorterThanTheWindowWithNoSofIsUnreadable) {
  // SOI, a complete APP0, then an APP1 whose declared length runs past the end of the file: the
  // marker walk runs out of bytes, but the whole entry was in the window, so no walk can ever
  // find a SOF. Deferring it would queue a walk that fails at every build's end.
  std::string jpeg = "\xFF\xD8\xFF\xE0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00";
  jpeg += "\xFF\xE1\x40\x00Exif\x00\x00";
  jpeg += std::string(200, '\x5A');
  test_zip::StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");
  zip.add("OEBPS/images/cut.jpg", jpeg);
  const std::string book = (work / "cut.epub").string();
  zip.write(book);

  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  const ImageManifestEntry* entry = nullptr;
  EXPECT_EQ(manifest.resolve(book, "OEBPS/images/cut.jpg", entry), EpubImageManifest::Resolve::Unreadable);
  EXPECT_EQ(entry, nullptr);
  EXPECT_FALSE(manifest.hasPending());
}

TEST_F(ImageManifestFixture, AHeaderWithinTheWindowResolvesInline) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  const ImageManifestEntry* entry = nullptr;
  EXPECT_EQ(manifest.resolve(kPngBook, kPngEntry, entry), EpubImageManifest::Resolve::Resolved);
  ASSERT_NE(entry, nullptr);
  EXPECT_GT(entry->width, 0);
  EXPECT_FALSE(manifest.hasPending());
}

// --- 3 + 4: the parser and the build ------------------------------------------------------

struct DeferredImageBuildFixture : ImageManifestFixture {
  std::shared_ptr<Epub> epub;
  GfxRenderer renderer;

  void TearDown() override {
    epub.reset();  // release book.bin and the manifest's resolve handle before the dir goes
    ImageManifestFixture::TearDown();
  }

  Section::BuildParams params() const {
    Section::BuildParams p;
    p.viewportWidth = 480;
    p.viewportHeight = 800;
    p.lineCompression = 1.0f;
    return p;
  }

  void openBook() {
    epub = std::make_shared<Epub>(kBook, cacheDir);
    ASSERT_TRUE(epub->load(true));
    epub->loadImageManifest();  // as the reader does before its first section build
    ASSERT_NE(epub->getImageManifest(), nullptr);
  }

  bool buildAndReportDegraded() {
    Section section(epub, 0, renderer);
    section.clearCache();
    EXPECT_TRUE(section.createSectionFile(params(), {}, /*skipEviction=*/true));
    EXPECT_GT(section.pageCount, 0);
    return section.isImageHeaderDegraded();
  }
};

TEST_F(DeferredImageBuildFixture, AmpleContiguousHeapResolvesInlineAndIsNotFlagged) {
  openBook();
  ESP.setMaxAllocHeap(100 * 1024);
  EXPECT_FALSE(buildAndReportDegraded());
  EXPECT_NE(epub->getImageManifest()->find(kEntry), nullptr) << "an inline walk records its result like any other";
}

TEST_F(DeferredImageBuildFixture, RingRefusedMidParseIsLatchedAsProvisional) {
  openBook();
  // 16 KB clears the 8 KB header-read gate but cannot host this entry's ~21 KB inflate ring.
  // Before the fix the walk was attempted regardless (host malloc never fails), so this
  // reported clean — and on the device the failed ring was cached as permanent alt text.
  ESP.setMaxAllocHeap(16 * 1024);
  EXPECT_TRUE(buildAndReportDegraded()) << "a walk the heap cannot host must be latched, never cached as final";
  EXPECT_EQ(epub->getImageManifest()->find(kEntry), nullptr);
  EXPECT_TRUE(epub->getImageManifest()->hasPending());
}

TEST_F(DeferredImageBuildFixture, ResolvingPendingAtBuildEndMakesTheNextBuildClean) {
  openBook();
  ESP.setMaxAllocHeap(16 * 1024);
  ASSERT_TRUE(buildAndReportDegraded());

  // Build end: the parser and its arena are gone, the heap is at its best. The reader calls
  // this after every completed build; it must report that a deferred image was resolved so
  // the caller knows a rebuild will now come out clean.
  ESP.setMaxAllocHeap(100 * 1024);
  EXPECT_TRUE(epub->persistImageManifest());
  EXPECT_FALSE(epub->persistImageManifest()) << "nothing left to resolve: the second call must not claim progress";

  ESP.setMaxAllocHeap(16 * 1024);  // the rebuild needs no ring at all: the manifest answers
  EXPECT_FALSE(buildAndReportDegraded());
}

}  // namespace
