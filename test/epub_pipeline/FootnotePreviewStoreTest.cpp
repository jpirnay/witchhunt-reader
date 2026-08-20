// Cover for the incremental footnote preview store.
//
// Inline expansion makes note text a LAYOUT input: it changes line breaking, so it must be in
// place before a spine is laid out, or that spine's page cache is written under the previews-on
// key while showing bare markers — damage that survives every later visit. The store therefore
// grows from inside the build, one spine at a time, in the gap between the extract and the
// layout parse. These tests pin the three properties that makes it safe to rely on:
//
//   1. it accumulates across spines and is idempotent per spine,
//   2. a spine resolved earlier costs no archive access at all,
//   3. note documents get banked on the way past, so the next chapter pointing into one reads
//      it from SD instead of inflating it again.
#include <Arduino.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/FootnotePreviews.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

// chapter1 carries the callers; notes.xhtml holds every note body.
constexpr int kChapterSpine = 0;
constexpr int kNotesSpine = 1;

std::string freshDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "footnote_store_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

const std::string kCorpusEpub = std::string(CORPUS_DIR) + "/test_inline_footnotes.epub";

uintmax_t storeSize(const Epub& epub) {
  const std::string path = epub.getCachePath() + FootnotePreviews::CACHE_FILENAME;
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  return ec ? 0 : size;
}

// Layout of the store's fixed part: a 12-byte header, then the resolved-spine bitmap, then the
// blob. Spelled out here rather than shared with the implementation so that a format change has
// to be made deliberately in two places instead of silently agreeing with itself.
constexpr size_t kHeaderBytes = 12;
constexpr size_t kResolvedBitmapBytes = 64;
constexpr size_t kBlobStart = kHeaderBytes + kResolvedBitmapBytes;

// Every note text the store holds, in blob order. Reading the format here rather than through
// Lookup keeps the test honest about what actually landed on disk.
std::vector<std::string> storeTexts(const Epub& epub) {
  std::vector<std::string> texts;
  std::ifstream in(epub.getCachePath() + FootnotePreviews::CACHE_FILENAME, std::ios::binary);
  if (!in) return texts;
  const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (bytes.size() < kBlobStart) return texts;
  uint16_t count = 0;
  uint32_t indexOffset = 0;
  memcpy(&count, bytes.data() + 6, sizeof(count));
  memcpy(&indexOffset, bytes.data() + 8, sizeof(indexOffset));
  size_t off = kBlobStart;
  for (uint16_t i = 0; i < count && off + 2 <= indexOffset; ++i) {
    uint16_t len = 0;
    memcpy(&len, bytes.data() + off, sizeof(len));
    off += sizeof(len);
    if (off + len > bytes.size()) break;
    texts.emplace_back(bytes, off, len);
    off += len;
  }
  return texts;
}

std::shared_ptr<Epub> openBook(const std::string& path, const std::string& cacheDir) {
  auto epub = std::make_shared<Epub>(path, cacheDir);
  EXPECT_TRUE(epub->load(true));
  return epub;
}

}  // namespace

TEST(FootnotePreviewStore, AccumulatesPerSpineAndIsIdempotent) {
  const std::string cacheDir = freshDir("accumulates");
  auto epub = openBook(kCorpusEpub, cacheDir);

  // Nothing is resolved until a spine asks for it — a book whose notes the reader never reaches
  // must never pay for them.
  EXPECT_FALSE(FootnotePreviews::cacheExists(epub->getCachePath()));

  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, kChapterSpine));
  const std::vector<std::string> afterFirst = storeTexts(*epub);
  EXPECT_FALSE(afterFirst.empty()) << "chapter1's callers should have resolved to note text";

  // Second pass over the same spine: every target is already known, so it appends nothing and
  // leaves the file byte-for-byte alone.
  const uintmax_t sizeAfterFirst = storeSize(*epub);
  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, kChapterSpine));
  EXPECT_EQ(storeSize(*epub), sizeAfterFirst);
  EXPECT_EQ(storeTexts(*epub), afterFirst);

  // The notes spine itself carries no callers, so resolving it is a no-op rather than an error.
  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, kNotesSpine));
  EXPECT_EQ(storeTexts(*epub), afterFirst);
}

TEST(FootnotePreviewStore, ResolvedSpineNeedsNoArchive) {
  const std::string cacheDir = freshDir("no_archive");
  const std::string epubCopy = cacheDir + "/book.epub";  // work from a copy, never the corpus
  fs::copy_file(kCorpusEpub, epubCopy);

  auto epub = openBook(epubCopy, cacheDir);
  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, kChapterSpine));
  const std::vector<std::string> resolved = storeTexts(*epub);
  ASSERT_FALSE(resolved.empty());

  // Pass A reads the spine's banked XHTML and Pass B has nothing left to fetch, so a re-resolve
  // must not touch the archive. Deleting it is the only way to prove that from outside.
  GfxRenderer renderer;
  Section section(epub, kChapterSpine, renderer);
  Section::BuildParams params;
  params.viewportWidth = 480;
  params.viewportHeight = 800;
  params.lineCompression = 1.0f;
  ASSERT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));  // banks chapter1
  fs::remove(epubCopy);

  EXPECT_TRUE(FootnotePreviews::resolveSpine(*epub, kChapterSpine));
  EXPECT_EQ(storeTexts(*epub), resolved);
}

TEST(FootnotePreviewStore, BanksTheNoteDocumentItStreams) {
  const std::string cacheDir = freshDir("banks_notes");
  auto epub = openBook(kCorpusEpub, cacheDir);

  // notes.xhtml has never been built as a chapter, so nothing has banked it yet.
  const std::string notesHtml = Section::sectionHtmlCachePath(epub->getCachePath(), kNotesSpine);
  ASSERT_FALSE(fs::exists(notesHtml));

  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, kChapterSpine));

  // Banked at full length, which is exactly the staleness test the section builder applies
  // before reusing it — so the next reader of that spine skips inflation too.
  ASSERT_TRUE(fs::exists(notesHtml));
  size_t inflatedSize = 0;
  ASSERT_TRUE(epub->getSpineItemInflatedSize(kNotesSpine, &inflatedSize));
  EXPECT_EQ(fs::file_size(notesHtml), inflatedSize);
}

TEST(FootnotePreviewStore, BuildResolvesItsOwnSpine) {
  const std::string cacheDir = freshDir("build_resolves");
  auto epub = openBook(kCorpusEpub, cacheDir);

  GfxRenderer renderer;
  Section section(epub, kChapterSpine, renderer);
  Section::BuildParams params;
  params.viewportWidth = 480;
  params.viewportHeight = 800;
  params.lineCompression = 1.0f;
  params.inlineFootnotePreviews = true;

  // The build must produce the note text itself. Before this change the reader had to gather the
  // whole book up front and then throw the chapter away and rebuild it.
  ASSERT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
  EXPECT_FALSE(storeTexts(*epub).empty());
}

// The other real-world shape: one note per spine document, so a single chapter's callers point
// into several different files (Feet of Clay puts 14 notes in 14 documents, Small Gods 10 in 10).
// Nothing else in the corpus exercises multiple note documents in one pass, and nothing else
// exercises banking a document that is a spine entry no reader ever opens as a chapter.
TEST(FootnotePreviewStore, ResolvesNotesSplitAcrossManyDocuments) {
  const std::string cacheDir = freshDir("split_notes");
  auto epub = openBook(std::string(CORPUS_DIR) + "/test_split_footnotes.epub", cacheDir);

  // chapter1 -> note1..note3 (spines 2..4), chapter2 -> note4 plus note1 again.
  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, 0));
  const std::vector<std::string> afterChapter1 = storeTexts(*epub);
  EXPECT_EQ(afterChapter1.size(), 3u);
  for (int noteSpine = 2; noteSpine <= 4; ++noteSpine) {
    EXPECT_TRUE(fs::exists(Section::sectionHtmlCachePath(epub->getCachePath(), noteSpine)))
        << "note document at spine " << noteSpine << " should have been banked on the way past";
  }

  // Chapter two adds exactly one note: its other caller points at a note already resolved, which
  // must not be re-streamed or re-stored.
  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, 1));
  const std::vector<std::string> afterChapter2 = storeTexts(*epub);
  EXPECT_EQ(afterChapter2.size(), 4u);

  // Chrome that real converters emit around a note body — the number as a heading, and the link
  // back to the caller — must not eat the preview's width.
  for (const std::string& text : afterChapter2) {
    EXPECT_EQ(text.find("note_"), std::string::npos) << "backlink text leaked into: " << text;
    EXPECT_FALSE(text.empty());
    EXPECT_FALSE(isdigit(static_cast<unsigned char>(text[0]))) << "heading number leaked into: " << text;
  }
}

// A sliced build re-enters runBuildParse once per slice, from the top, so the between-phases step
// — resolve the notes, then initialise the layout parser — is guarded to run exactly once. That
// guard is not observable from outside: resolving twice appends nothing the second time, so its
// only symptom is wasted work, not a wrong answer. What this test can pin is the other half —
// that the sliced entry point produces the same result as the blocking one with previews on,
// which is the shape every background build takes and which nothing else covers. On the host a
// corpus fixture usually completes in a single slice; the equivalence is the point either way.
TEST(FootnotePreviewStore, SlicedBuildMatchesTheBlockingBuild) {
  const std::string epubPath = std::string(CORPUS_DIR) + "/test_split_footnotes.epub";

  Section::BuildParams params;
  params.viewportWidth = 480;
  params.viewportHeight = 800;
  params.lineCompression = 1.0f;
  params.inlineFootnotePreviews = true;

  GfxRenderer renderer;
  const std::string blockingDir = freshDir("sliced_reference");
  auto blockingEpub = openBook(epubPath, blockingDir);
  Section blocking(blockingEpub, 0, renderer);
  ASSERT_TRUE(blocking.createSectionFile(params, {}, /*skipEviction=*/true));
  ASSERT_TRUE(blocking.loadSectionFile(params));

  const std::string slicedDir = freshDir("sliced");
  auto slicedEpub = openBook(epubPath, slicedDir);
  Section sliced(slicedEpub, 0, renderer);
  int slices = 0;
  Section::BuildStep step = Section::BuildStep::More;
  while (step != Section::BuildStep::Done && step != Section::BuildStep::Failed && slices < 20000) {
    step = sliced.stepSectionBuild(params, /*budgetMs=*/1);
    ++slices;
  }
  ASSERT_EQ(step, Section::BuildStep::Done);
  ASSERT_TRUE(sliced.loadSectionFile(params));

  // Same notes, same pagination: the expansion text is part of the layout, so a page-count match
  // is a strong statement that previews were present for the sliced build too.
  EXPECT_EQ(storeTexts(*slicedEpub), storeTexts(*blockingEpub));
  EXPECT_EQ(sliced.pageCount, blocking.pageCount);
  EXPECT_GT(storeTexts(*slicedEpub).size(), 0u);
}

// The resolved bit is what lets a build skip the resolver entirely, and what Background-B reads
// to decide a spine is safe to pre-build without doing resolver work on the loop task. Both rest
// on it meaning "scanned, nothing outstanding" — so a chapter with NO notes has to answer true as
// well, or a book without footnotes would look permanently unresolved and never get look-ahead.
TEST(FootnotePreviewStore, MarksSpinesResolvedIncludingOnesWithoutNotes) {
  const std::string cacheDir = freshDir("resolved_bits");
  auto epub = openBook(kCorpusEpub, cacheDir);

  EXPECT_FALSE(FootnotePreviews::spineResolved(epub->getCachePath(), kChapterSpine));
  EXPECT_FALSE(FootnotePreviews::spineResolved(epub->getCachePath(), kNotesSpine));

  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, kChapterSpine));
  EXPECT_TRUE(FootnotePreviews::spineResolved(epub->getCachePath(), kChapterSpine));
  EXPECT_FALSE(FootnotePreviews::spineResolved(epub->getCachePath(), kNotesSpine));

  // notes.xhtml carries note bodies and no callers of its own. Nothing to store, bit set anyway.
  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, kNotesSpine));
  EXPECT_TRUE(FootnotePreviews::spineResolved(epub->getCachePath(), kNotesSpine));
}

// With the bit set the resolver must not read the document at all — not the banked XHTML, not the
// archive. Removing both is the only way to assert that from outside, and it is the property the
// per-build cost rests on: before the bit existed, every rebuild re-scanned the whole spine with a
// 9.2 KB parser just to discover nothing was missing.
TEST(FootnotePreviewStore, ResolvedSpineIsNotScannedAgain) {
  const std::string cacheDir = freshDir("no_rescan");
  const std::string epubCopy = cacheDir + "/book.epub";
  fs::copy_file(kCorpusEpub, epubCopy);
  auto epub = openBook(epubCopy, cacheDir);

  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, kChapterSpine));
  const std::vector<std::string> resolved = storeTexts(*epub);
  ASSERT_FALSE(resolved.empty());

  fs::remove(epubCopy);
  fs::remove(Section::sectionHtmlCachePath(epub->getCachePath(), kChapterSpine));  // if it was banked

  EXPECT_TRUE(FootnotePreviews::resolveSpine(*epub, kChapterSpine));
  EXPECT_EQ(storeTexts(*epub), resolved);
}
