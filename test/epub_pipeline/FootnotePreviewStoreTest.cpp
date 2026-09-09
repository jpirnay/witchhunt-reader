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
#include "StoredZipWriter.h"

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

// ---------------------------------------------------------------------------------------------
// The index MERGE, at a scale the fixtures above cannot reach.
//
// The store's index lives on disk and is never held in RAM: a pass parks it in a spill file, its
// blobs overwrite it, and commit() merges the spill with the pass's own rows straight back out.
// Above, that merge runs with four notes and a single pass, which is exactly the size at which a
// wrong merge still looks right. These tests give it several passes over hundreds of notes, with
// key hashes that interleave the two sides arbitrarily, and check the three properties a reader
// depends on: every note is findable, the index is ordered so the binary search can find it, and
// re-resolving does not grow it.
namespace {

// A book of `chapters` chapters, each with `notesPerChapter` callers into one shared notes
// document. Deliberately minimal — no size padding, no front matter; the shapes those cover are
// the subject of FootnoteResolveSliceTest. Note text is a function of the note's id so the test
// can assert the exact string that came back, not merely that something did.
std::string noteTextFor(const int id) { return "Note " + std::to_string(id) + " body text for the merge test."; }

std::string makeManyNotesBook(const std::string& dir, const int chapters, const int notesPerChapter) {
  test_zip::StoredZipWriter zip;
  std::string manifest, spine, notesBodies;
  int noteId = 0;
  for (int c = 0; c < chapters; ++c) {
    const std::string name = "chapter" + std::to_string(c) + ".xhtml";
    std::string body;
    for (int n = 0; n < notesPerChapter; ++n) {
      const std::string id = std::to_string(++noteId);
      body += "<p>Prose before the marker<a href=\"notes.xhtml#ft_" + id + "\" id=\"ft" + id +
              "\"><sup>*</sup></a> and after it.</p>\n";
      notesBodies += "<p id=\"ft_" + id + "\"><a href=\"" + name + "#ft" + id + "\"><sup>*</sup></a>" +
                     noteTextFor(noteId) + "</p>\n";
    }
    zip.add("OEBPS/" + name,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
            "<head><title>C</title></head><body>\n" +
                body + "</body></html>\n");
    manifest +=
        "<item id=\"c" + std::to_string(c) + "\" href=\"" + name + "\" media-type=\"application/xhtml+xml\"/>\n";
    spine += "<itemref idref=\"c" + std::to_string(c) + "\"/>";
  }
  zip.add("OEBPS/notes.xhtml",
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
          "<head><title>N</title></head><body>\n" +
              notesBodies + "</body></html>\n");
  manifest += "<item id=\"n\" href=\"notes.xhtml\" media-type=\"application/xhtml+xml\"/>\n";
  spine += "<itemref idref=\"n\"/>";

  zip.add("OEBPS/content.opf",
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
          "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">\n"
          "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">many-notes"
          "</dc:identifier><dc:title>Many Notes</dc:title><dc:language>en</dc:language></metadata>\n"
          "<manifest>\n" +
              manifest + "</manifest>\n<spine>" + spine + "</spine>\n</package>\n");
  zip.add("mimetype", "application/epub+zip");
  zip.add("META-INF/container.xml",
          "<?xml version=\"1.0\"?>\n"
          "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
          "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
          "media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");

  const std::string path = dir + "/many_notes.epub";
  zip.write(path);
  return path;
}

// The index as it sits on disk: count, then the rows. Read from the bytes rather than through
// Lookup, so the test can say something about the ORDER the search depends on and not only about
// what a search happens to return.
struct OnDiskIndex {
  uint16_t count = 0;
  std::vector<std::pair<uint32_t, uint32_t>> rows;  // keyHash, blobOffset
};

OnDiskIndex readIndex(const Epub& epub) {
  OnDiskIndex out;
  std::ifstream in(epub.getCachePath() + FootnotePreviews::CACHE_FILENAME, std::ios::binary);
  if (!in) return out;
  const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (bytes.size() < kBlobStart) return out;
  uint32_t indexOffset = 0;
  memcpy(&out.count, bytes.data() + 6, sizeof(out.count));
  memcpy(&indexOffset, bytes.data() + 8, sizeof(indexOffset));
  // The invariant Lookup::open and Store::open both validate before trusting the file.
  EXPECT_EQ(bytes.size(), indexOffset + static_cast<size_t>(out.count) * 8) << "store length does not match its header";
  for (uint16_t i = 0; i < out.count; ++i) {
    const size_t at = indexOffset + static_cast<size_t>(i) * 8;
    if (at + 8 > bytes.size()) break;
    uint32_t keyHash = 0, blobOffset = 0;
    memcpy(&keyHash, bytes.data() + at, sizeof(keyHash));
    memcpy(&blobOffset, bytes.data() + at + 4, sizeof(blobOffset));
    out.rows.emplace_back(keyHash, blobOffset);
  }
  return out;
}

}  // namespace

// Five passes, 140 notes each. Every note must come back with its own text — a merge that drops
// one side, or writes the rows out of order, loses notes the binary search then cannot find.
// 700 notes is also past the 512 the store used to cap at, so this fails outright on the old
// budget rather than merely losing the ordering.
TEST(FootnotePreviewStore, MergesHundredsOfNotesAcrossPasses) {
  constexpr int kChapters = 5;
  constexpr int kPerChapter = 140;
  const std::string dir = freshDir("merge_scale");
  const std::string book = makeManyNotesBook(dir, kChapters, kPerChapter);
  auto epub = openBook(book, dir + "/cache");

  for (int c = 0; c < kChapters; ++c) {
    ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, c)) << "chapter " << c << " failed to resolve";
  }

  const OnDiskIndex index = readIndex(*epub);
  EXPECT_EQ(index.count, kChapters * kPerChapter) << "the merge lost or duplicated notes";

  // Ordered, and strictly so: the search assumes both.
  bool sorted = true;
  bool unique = true;
  for (size_t i = 1; i < index.rows.size(); ++i) {
    if (index.rows[i].first < index.rows[i - 1].first) sorted = false;
    if (index.rows[i].first == index.rows[i - 1].first) unique = false;
  }
  EXPECT_TRUE(sorted) << "the merged index is not ordered by key hash";
  EXPECT_TRUE(unique) << "the merged index contains duplicate keys";

  // And every note actually resolves to ITS text, through the same Lookup the layout parse uses.
  FootnotePreviews::Lookup lookup;
  ASSERT_TRUE(lookup.open(epub->getCachePath(), epub.get(), /*currentSpineIndex=*/0));
  int missing = 0;
  int wrong = 0;
  for (int id = 1; id <= kChapters * kPerChapter; ++id) {
    const std::string href = "notes.xhtml#ft_" + std::to_string(id);
    std::string text;
    if (!lookup.find(href.c_str(), text)) {
      ++missing;
    } else if (text != noteTextFor(id)) {
      ++wrong;
    }
  }
  EXPECT_EQ(missing, 0) << missing << " notes are in no index the reader can search";
  EXPECT_EQ(wrong, 0) << wrong << " notes resolved to another note's text";
}

// Re-resolving a chapter whose notes are all stored must leave the index alone, not append a
// second copy of every row. What makes that true is contains() filtering the pass's targets
// against the on-disk index before anything is appended -- the pass then has nothing to add and
// short-circuits, so the merge is never reached at all. (Verified: breaking commit()'s tie rule
// does not fail this test, because no tie ever arises.)
TEST(FootnotePreviewStore, ReResolvingDoesNotGrowTheMergedIndex) {
  const std::string dir = freshDir("merge_idempotent");
  const std::string book = makeManyNotesBook(dir, /*chapters=*/3, /*notesPerChapter=*/60);
  auto epub = openBook(book, dir + "/cache");

  for (int c = 0; c < 3; ++c) ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, c));
  const OnDiskIndex first = readIndex(*epub);
  const uintmax_t sizeAfterFirst = storeSize(*epub);
  ASSERT_EQ(first.count, 180);

  for (int c = 0; c < 3; ++c) ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, c));
  const OnDiskIndex second = readIndex(*epub);
  EXPECT_EQ(second.count, first.count) << "a second pass over the same spines grew the index";
  EXPECT_EQ(second.rows, first.rows) << "a second pass rewrote the index differently";
  EXPECT_EQ(storeSize(*epub), sizeAfterFirst) << "a second pass grew the store on disk";
}

// A notes document's entries open with a link back to the caller they belong to, and those
// back-links are marker-shaped, so Pass A collects them like any other footnote-shaped link.
// Pass B used to follow one into the chapter, land on the caller <a>, see a one-character subtree
// ("*"), take that for the empty inline anchor of the Calibre filepos pattern, and capture the
// text FOLLOWING it. The chapter's next sentence was then stored as if it were a note -- and
// spliced into the notes page when it was laid out. On a real book that put 512 entries of
// chapter prose in the store and injected 329 of them into the endnotes chapter.
TEST(FootnotePreviewStore, ANoteBackLinkIsNotItselfANote) {
  const std::string dir = freshDir("back_links");
  const std::string book = makeManyNotesBook(dir, /*chapters=*/2, /*notesPerChapter=*/8);
  auto epub = openBook(book, dir + "/cache");
  constexpr int kNotesDocSpine = 2;  // after the two chapters

  for (int c = 0; c < 2; ++c) ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, c));
  const OnDiskIndex afterChapters = readIndex(*epub);
  ASSERT_EQ(afterChapters.count, 16) << "both chapters' notes should be stored";

  // Resolving the notes document itself must add nothing: every marker-shaped link in it points
  // back at a caller, and a caller is not a note.
  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, kNotesDocSpine));
  const OnDiskIndex afterNotesDoc = readIndex(*epub);
  EXPECT_EQ(afterNotesDoc.count, afterChapters.count) << "back-links were stored as if they were notes";

  // And specifically: nothing is stored for the caller anchors the back-links point at, so
  // laying the notes document out cannot splice chapter prose into it.
  FootnotePreviews::Lookup lookup;
  ASSERT_TRUE(lookup.open(epub->getCachePath(), epub.get(), kNotesDocSpine));
  std::string text;
  EXPECT_FALSE(lookup.find("chapter0.xhtml#ft1", text))
      << "the caller anchor resolved to a preview: '" << text << "'";
  EXPECT_FALSE(lookup.find("chapter1.xhtml#ft9", text))
      << "the caller anchor resolved to a preview: '" << text << "'";

  // The real notes are untouched by the fix.
  ASSERT_TRUE(lookup.open(epub->getCachePath(), epub.get(), /*currentSpineIndex=*/0));
  ASSERT_TRUE(lookup.find("notes.xhtml#ft_1", text));
  EXPECT_EQ(text, noteTextFor(1));
}
