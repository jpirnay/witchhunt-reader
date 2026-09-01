// Cover for the SLICING of the footnote-preview resolve.
//
// FootnotePreviewStoreTest pins what the resolve produces. This file pins what it costs, which is
// a separate property and the one that regressed: the pass runs on the loop task from inside a
// section build, so no single step may do an amount of work that depends on the book. A 200 KB
// chapter and a chapter with a hundred callers into one fat rearnotes document have to slice the
// same way a 1 KB chapter with one note does — otherwise Background-B stalls input for as long as
// the document takes, which is what made an earlier fix refuse to pre-build such spines at all
// (issue #211).
//
// The books are generated here rather than checked in, because the point is the MATRIX — spine
// size against note count — and a fixture per cell would be four binaries that no one can diff.
// They are written as STORED (uncompressed) zip entries, which ZipFile and EntryReader both read;
// the deflated path is covered by the corpus book at the bottom.
#include <Arduino.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
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

// The resolver's read granularity. Spelled out rather than shared with the implementation so a
// change to it has to be made deliberately in two places (same rule as the store layout in
// FootnotePreviewStoreTest).
constexpr size_t kStreamChunkBytes = 1024;

constexpr int kChapterSpine = 0;
constexpr int kNotesSpine = 1;

std::string freshDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "footnote_slice_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

// --- minimal STORED-only zip writer -------------------------------------------------------

class StoredZipWriter {
 public:
  void add(const std::string& name, const std::string& data) { entries_.push_back({name, data, 0}); }

  void write(const std::string& path) {
    std::string out;
    for (Entry& e : entries_) {
      e.localOffset = static_cast<uint32_t>(out.size());
      appendLocalHeader(out, e);
      out += e.data;
    }
    const uint32_t centralStart = static_cast<uint32_t>(out.size());
    for (const Entry& e : entries_) {
      appendCentralHeader(out, e);
    }
    const uint32_t centralSize = static_cast<uint32_t>(out.size()) - centralStart;
    appendEocd(out, centralStart, centralSize, static_cast<uint16_t>(entries_.size()));
    std::ofstream f(path, std::ios::binary);
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
  }

 private:
  struct Entry {
    std::string name;
    std::string data;
    uint32_t localOffset;
  };

  static void put16(std::string& out, const uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
  }
  static void put32(std::string& out, const uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  // CRC-32 is checked by nothing on the read path here, but a zero would be a lie in a file we
  // hand to a real reader, so compute it properly.
  static uint32_t crc32(const std::string& data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char c : data) {
      crc ^= c;
      for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
    return ~crc;
  }
  static void appendLocalHeader(std::string& out, const Entry& e) {
    put32(out, 0x04034B50);
    put16(out, 20);  // version needed
    put16(out, 0);   // flags
    put16(out, 0);   // method: stored
    put16(out, 0);   // time
    put16(out, 0);   // date
    put32(out, crc32(e.data));
    put32(out, static_cast<uint32_t>(e.data.size()));
    put32(out, static_cast<uint32_t>(e.data.size()));
    put16(out, static_cast<uint16_t>(e.name.size()));
    put16(out, 0);
    out += e.name;
  }
  static void appendCentralHeader(std::string& out, const Entry& e) {
    put32(out, 0x02014B50);
    put16(out, 20);  // version made by
    put16(out, 20);  // version needed
    put16(out, 0);
    put16(out, 0);  // method: stored
    put16(out, 0);
    put16(out, 0);
    put32(out, crc32(e.data));
    put32(out, static_cast<uint32_t>(e.data.size()));
    put32(out, static_cast<uint32_t>(e.data.size()));
    put16(out, static_cast<uint16_t>(e.name.size()));
    put16(out, 0);  // extra
    put16(out, 0);  // comment
    put16(out, 0);  // disk
    put16(out, 0);  // internal attrs
    put32(out, 0);  // external attrs
    put32(out, e.localOffset);
    out += e.name;
  }
  static void appendEocd(std::string& out, const uint32_t centralStart, const uint32_t centralSize,
                         const uint16_t entryCount) {
    put32(out, 0x06054B50);
    put16(out, 0);  // disk number
    put16(out, 0);  // disk with central dir
    put16(out, entryCount);
    put16(out, entryCount);
    put32(out, centralSize);
    put32(out, centralStart);
    put16(out, 0);  // comment length
  }

  std::vector<Entry> entries_;
};

// --- fixture generation --------------------------------------------------------------------

// One chapter with `noteCount` footnote callers, padded to at least `chapterBytes`, plus one
// document holding every note body. Two knobs, four interesting corners.
std::string makeBook(const std::string& dir, const int noteCount, const size_t chapterBytes) {
  std::string callers;
  for (int i = 1; i <= noteCount; ++i) {
    callers += "<p>Body text before the marker <a href=\"notes.xhtml#note_" + std::to_string(i) + "\">" +
               std::to_string(i) + "</a> and after it.</p>\n";
  }
  // Pad with ordinary prose so the scanner has to walk a big document, not a big attribute.
  std::string padding;
  while (callers.size() + padding.size() < chapterBytes) {
    padding +=
        "<p>The Patrician had a broad and comprehensive view of the world, which is to say a "
        "short one, and he had it at some length.</p>\n";
  }
  const std::string chapter =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">\n<head><title>C"
      "</title></head>\n<body>\n" +
      callers + padding + "</body>\n</html>\n";

  std::string notes;
  for (int i = 1; i <= noteCount; ++i) {
    notes += "<div id=\"note_" + std::to_string(i) + "\"><p>Note number " + std::to_string(i) +
             ", which says something worth previewing and then stops.</p></div>\n";
  }
  const std::string notesDoc =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">\n<head><title>N"
      "</title></head>\n<body>\n" +
      notes + "</body>\n</html>\n";

  const std::string opf =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">\n"
      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">slice-test</dc:identifier>"
      "<dc:title>Slice Test</dc:title><dc:language>en</dc:language></metadata>\n"
      "<manifest>\n"
      "<item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "<item id=\"n\" href=\"notes.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "</manifest>\n"
      "<spine><itemref idref=\"c\"/><itemref idref=\"n\"/></spine>\n"
      "</package>\n";

  const std::string container =
      "<?xml version=\"1.0\"?>\n"
      "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
      "<rootfiles><rootfile full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n"
      "</container>\n";

  StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");
  zip.add("META-INF/container.xml", container);
  zip.add("content.opf", opf);
  zip.add("chapter.xhtml", chapter);
  zip.add("notes.xhtml", notesDoc);
  const std::string path = dir + "/book.epub";
  zip.write(path);
  return path;
}

// A second shape, modelled on a real book (Men at Arms): several LARGE chapters whose callers all
// point into ONE shared notes document, with tiny front matter in front of them. It is the shape
// that costs the most — a 275 KB pass A, then a note document that has to come out of the archive
// the first time — and the one the device trace measured at 1125 ms of resolve for a single spine.
// That whole 1125 ms used to be one loop-task slice.
//
// The markup is the real thing, not a simplification. Callers are `<a href="..."><sup>*</sup></a>`
// (a marker with no digits, so the shape heuristic has to accept `*`), and each note body opens
// with a back-link carrying that same marker — chrome the capture must skip, or every preview
// would start with a stray asterisk.
struct Chapter {
  size_t bytes;
  int notes;
};

std::string makeSharedNotesBook(const std::string& dir, const std::vector<Chapter>& chapters) {
  const std::string notesDocName = "notes.xhtml";
  StoredZipWriter zip;
  std::string manifest;
  std::string spine;
  std::string notesBodies;
  int noteId = 0;

  // Front matter: two one-page documents with no notes at all, so the fixture also covers the
  // "spine that owes nothing still gets its resolved bit" path in the same book.
  for (int i = 0; i < 2; ++i) {
    const std::string name = "front" + std::to_string(i) + ".xhtml";
    zip.add("OEBPS/" + name,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
            "<head><title>F</title></head><body><p>Front matter.</p></body></html>\n");
    manifest +=
        "<item id=\"f" + std::to_string(i) + "\" href=\"" + name + "\" media-type=\"application/xhtml+xml\"/>\n";
    spine += "<itemref idref=\"f" + std::to_string(i) + "\"/>";
  }

  for (size_t c = 0; c < chapters.size(); ++c) {
    const std::string name = "chapter" + std::to_string(c) + ".xhtml";
    std::string body;
    for (int n = 0; n < chapters[c].notes; ++n) {
      const std::string id = std::to_string(++noteId);
      body += "<p>Prose leading up to the marker<a href=\"" + notesDocName + "#ft_" + id + "\" id=\"ft" + id +
              "\"><sup>*</sup></a> and prose after it.</p>\n";
      notesBodies += "<p class=\"footnote\" id=\"ft_" + id + "\"><span class=\"footnotePara\"><a href=\"" + name +
                     "#ft" + id + "\"><sup>*</sup></a>Note " + id +
                     ": the text a preview is supposed to show, and nothing else.</span></p>\n";
    }
    while (body.size() < chapters[c].bytes) {
      body +=
          "<p>The Patrician had a broad and comprehensive view of the world, which is to say a "
          "short one, and he had it at considerable length.</p>\n";
    }
    zip.add("OEBPS/" + name,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
            "<head><title>C</title></head><body>\n" +
                body + "</body></html>\n");
    manifest +=
        "<item id=\"c" + std::to_string(c) + "\" href=\"" + name + "\" media-type=\"application/xhtml+xml\"/>\n";
    spine += "<itemref idref=\"c" + std::to_string(c) + "\"/>";
  }

  zip.add("OEBPS/" + notesDocName,
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
          "<head><title>N</title></head><body>\n" +
              notesBodies + "</body></html>\n");
  manifest += "<item id=\"n\" href=\"" + notesDocName + "\" media-type=\"application/xhtml+xml\"/>\n";
  spine += "<itemref idref=\"n\"/>";

  const std::string opf =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">\n"
      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">shared-notes</dc:identifier>"
      "<dc:title>Shared Notes</dc:title><dc:language>en</dc:language></metadata>\n"
      "<manifest>\n" +
      manifest + "</manifest>\n<spine>" + spine + "</spine>\n</package>\n";
  zip.add("OEBPS/content.opf", opf);
  zip.add("mimetype", "application/epub+zip");
  zip.add("META-INF/container.xml",
          "<?xml version=\"1.0\"?>\n"
          "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
          "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
          "media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");

  const std::string path = dir + "/shared_notes.epub";
  zip.write(path);
  return path;
}

std::shared_ptr<Epub> openBook(const std::string& path, const std::string& cacheDir) {
  auto epub = std::make_shared<Epub>(path, cacheDir);
  EXPECT_TRUE(epub->load(true));
  return epub;
}

std::string storeBytes(const Epub& epub) {
  std::ifstream in(epub.getCachePath() + FootnotePreviews::CACHE_FILENAME, std::ios::binary);
  if (!in) return {};
  return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The note texts the store actually SERVES, read the way Lookup reads them: through the sorted
// index, not in blob order. The difference matters here and nowhere else — an abandoned append
// leaves orphaned blobs behind (see AbandonedResolveLeavesTheStoreUsable), and blob-order reading
// would report those dead bytes as if they were entries. Sorted, so index order is not part of
// the assertion. Layout constants are spelled out rather than shared with the implementation, for
// the same reason FootnotePreviewStoreTest spells them out: a format change should have to be
// made deliberately in two places.
std::vector<std::string> storeTexts(const Epub& epub) {
  constexpr size_t kBlobStart = 12 + 64;  // header + resolved-spine bitmap
  constexpr size_t kIndexRowBytes = 8;    // u32 keyHash + u32 blobOffset
  std::vector<std::string> texts;
  const std::string bytes = storeBytes(epub);
  if (bytes.size() < kBlobStart) return texts;
  uint16_t count = 0;
  uint32_t indexOffset = 0;
  memcpy(&count, bytes.data() + 6, sizeof(count));
  memcpy(&indexOffset, bytes.data() + 8, sizeof(indexOffset));
  for (uint16_t i = 0; i < count; ++i) {
    const size_t row = indexOffset + static_cast<size_t>(i) * kIndexRowBytes;
    if (row + kIndexRowBytes > bytes.size()) break;
    uint32_t blobOffset = 0;
    memcpy(&blobOffset, bytes.data() + row + 4, sizeof(blobOffset));
    if (blobOffset + 2 > bytes.size()) break;
    uint16_t len = 0;
    memcpy(&len, bytes.data() + blobOffset, sizeof(len));
    if (blobOffset + 2 + len > bytes.size()) break;
    texts.emplace_back(bytes, blobOffset + 2, len);
  }
  std::sort(texts.begin(), texts.end());
  return texts;
}

// Runs the resolver one step at a time. Returns the step count, or -1 on failure.
int resolveStepByStep(Epub& epub, const int spineIndex) {
  FootnotePreviews::Resolver resolver;
  if (!resolver.begin(epub, spineIndex)) return -1;
  int steps = 0;
  while (true) {
    const auto step = resolver.step();
    ++steps;
    if (step == FootnotePreviews::Resolver::Step::Done) return steps;
    if (step == FootnotePreviews::Resolver::Step::Failed) return -1;
    if (steps > 200000) {
      ADD_FAILURE() << "resolver never terminated";
      return -1;
    }
  }
}

struct Shape {
  const char* name;
  int noteCount;
  size_t chapterBytes;
};

// The four corners the fix has to hold at. Small/large spine against few/many notes: each pairing
// stresses a different phase — a large spine is a long pass A, many notes is a long pass B.
const Shape kShapes[] = {
    {"small_one_note", 1, 512},
    {"small_many_notes", 100, 512},
    {"large_one_note", 1, 200 * 1024},
    {"large_many_notes", 100, 200 * 1024},
};

}  // namespace

// Stepping the resolver to completion must produce exactly the file the one-shot call produces,
// for every shape. This is the property that lets Section slice the pass without anyone auditing
// the store afterwards.
TEST(FootnoteResolveSlice, SlicedResolveMatchesOneShotForEveryShape) {
  for (const Shape& shape : kShapes) {
    const std::string oneShotDir = freshDir(std::string(shape.name) + "_oneshot");
    const std::string oneShotBook = makeBook(oneShotDir, shape.noteCount, shape.chapterBytes);
    auto oneShotEpub = openBook(oneShotBook, oneShotDir);
    ASSERT_TRUE(FootnotePreviews::resolveSpine(*oneShotEpub, kChapterSpine)) << shape.name;
    const std::string expected = storeBytes(*oneShotEpub);
    ASSERT_FALSE(expected.empty()) << shape.name;

    const std::string slicedDir = freshDir(std::string(shape.name) + "_sliced");
    const std::string slicedBook = makeBook(slicedDir, shape.noteCount, shape.chapterBytes);
    auto slicedEpub = openBook(slicedBook, slicedDir);
    ASSERT_GT(resolveStepByStep(*slicedEpub, kChapterSpine), 0) << shape.name;

    EXPECT_EQ(storeBytes(*slicedEpub), expected) << shape.name;
    EXPECT_TRUE(FootnotePreviews::spineResolved(slicedEpub->getCachePath(), kChapterSpine)) << shape.name;
  }
}

// The actual slicing guarantee: no single step may swallow a whole document. Asserted by counting
// steps against the bytes that had to be read — a step that read the lot would finish in far
// fewer. This is what fails if someone "optimises" the resolver by looping inside step().
TEST(FootnoteResolveSlice, NoStepReadsMoreThanOneChunk) {
  for (const Shape& shape : kShapes) {
    const std::string dir = freshDir(std::string(shape.name) + "_bounded");
    const std::string book = makeBook(dir, shape.noteCount, shape.chapterBytes);
    auto epub = openBook(book, dir);

    size_t chapterBytes = 0;
    size_t notesBytes = 0;
    ASSERT_TRUE(epub->getSpineItemInflatedSize(kChapterSpine, &chapterBytes));
    ASSERT_TRUE(epub->getSpineItemInflatedSize(kNotesSpine, &notesBytes));

    const int steps = resolveStepByStep(*epub, kChapterSpine);
    ASSERT_GT(steps, 0) << shape.name;

    // Pass A reads the chapter, pass B reads the notes document; both a chunk at a time.
    const size_t minSteps = (chapterBytes + notesBytes) / kStreamChunkBytes;
    EXPECT_GE(static_cast<size_t>(steps), minSteps)
        << shape.name << ": " << steps << " steps for " << (chapterBytes + notesBytes)
        << " bytes means a step read more than one " << kStreamChunkBytes << "-byte chunk";
  }
}

// A resolve that is abandoned part-way — the reader turned a page, Background-B handed its
// buffer back, the activity exited — must leave the store USABLE and no more complete than it
// found it. Slicing turns preemption from an error path into a routine one, so this is the case
// that has to hold at every point in the pass, not just at a tidy boundary.
//
// Note what is deliberately NOT asserted: byte-equality with a store that was never abandoned.
// Store::abandon() rolls the index back but cannot shrink the file — the HAL has no truncate —
// so the blobs the abandoned pass had already written stay behind as an unreferenced gap that the
// next pass appends after. That is the documented contract, and the file stays valid: what the
// store CONTAINS is what must not change.
TEST(FootnoteResolveSlice, AbandonedResolveLeavesTheStoreUsable) {
  const std::string dir = freshDir("abandoned_reference");
  const std::string book = makeBook(dir, /*noteCount=*/100, /*chapterBytes=*/200 * 1024);
  auto epub = openBook(book, dir);

  const int fullSteps = resolveStepByStep(*epub, kChapterSpine);
  ASSERT_GT(fullSteps, 8);
  const std::vector<std::string> expected = storeTexts(*epub);
  ASSERT_EQ(expected.size(), 100u);

  // Stop at points spread across the whole pass: inside pass A's scan, at the phase changes, and
  // deep inside pass B where the store is mid-append with most of the notes already written.
  for (const int stopAfter : {1, 2, 5, fullSteps / 4, fullSteps / 2, fullSteps - 2}) {
    const std::string abandonDir = freshDir("abandoned_at_" + std::to_string(stopAfter));
    const std::string abandonBook = makeBook(abandonDir, 100, 200 * 1024);
    auto abandonEpub = openBook(abandonBook, abandonDir);
    {
      FootnotePreviews::Resolver resolver;
      ASSERT_TRUE(resolver.begin(*abandonEpub, kChapterSpine));
      for (int i = 0; i < stopAfter; ++i) {
        ASSERT_EQ(resolver.step(), FootnotePreviews::Resolver::Step::More) << "stopAfter=" << stopAfter;
      }
    }  // destructor rolls the append back

    // Nothing may claim the spine is done off the back of a pass that did not finish.
    EXPECT_FALSE(FootnotePreviews::spineResolved(abandonEpub->getCachePath(), kChapterSpine))
        << "stopAfter=" << stopAfter;
    // And whatever landed on disk must still be a store the next pass can open and complete.
    ASSERT_TRUE(FootnotePreviews::resolveSpine(*abandonEpub, kChapterSpine)) << "stopAfter=" << stopAfter;
    EXPECT_EQ(storeTexts(*abandonEpub), expected) << "stopAfter=" << stopAfter;
    EXPECT_TRUE(FootnotePreviews::spineResolved(abandonEpub->getCachePath(), kChapterSpine))
        << "stopAfter=" << stopAfter;
  }
}

// Background-B may be preempted more than once on the same spine before it gets a clean run, so
// the gap an abandoned append leaves behind has to be survivable repeatedly — not just valid the
// first time.
TEST(FootnoteResolveSlice, RepeatedAbandonsStillResolveCleanly) {
  const std::string refDir = freshDir("repeat_reference");
  const std::string refBook = makeBook(refDir, /*noteCount=*/100, /*chapterBytes=*/8 * 1024);
  auto refEpub = openBook(refBook, refDir);
  ASSERT_GT(resolveStepByStep(*refEpub, kChapterSpine), 0);
  const std::vector<std::string> expected = storeTexts(*refEpub);
  ASSERT_EQ(expected.size(), 100u);

  const std::string dir = freshDir("repeat_abandons");
  const std::string book = makeBook(dir, 100, 8 * 1024);
  auto epub = openBook(book, dir);
  for (int cycle = 0; cycle < 5; ++cycle) {
    FootnotePreviews::Resolver resolver;
    ASSERT_TRUE(resolver.begin(*epub, kChapterSpine));
    for (int i = 0; i < 12 + cycle; ++i) {
      ASSERT_EQ(resolver.step(), FootnotePreviews::Resolver::Step::More) << "cycle=" << cycle;
    }
  }
  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, kChapterSpine));
  EXPECT_EQ(storeTexts(*epub), expected);
  EXPECT_TRUE(FootnotePreviews::spineResolved(epub->getCachePath(), kChapterSpine));
}

// The same guarantee one level up: a build driven with a 1 ms budget over a chapter whose resolve
// cannot fit in it must still produce the pages the blocking build produces. The resolve is a
// LAYOUT input, so an equal page count over a 100-note chapter says the previews were in place
// before the parse in both.
TEST(FootnoteResolveSlice, SlicedBuildResolvesManyNotesAcrossSlices) {
  Section::BuildParams params;
  params.viewportWidth = 480;
  params.viewportHeight = 800;
  params.lineCompression = 1.0f;
  params.inlineFootnotePreviews = true;

  GfxRenderer renderer;
  const std::string blockingDir = freshDir("build_blocking");
  const std::string blockingBook = makeBook(blockingDir, /*noteCount=*/100, /*chapterBytes=*/64 * 1024);
  auto blockingEpub = openBook(blockingBook, blockingDir);
  Section blocking(blockingEpub, kChapterSpine, renderer);
  ASSERT_TRUE(blocking.createSectionFile(params, {}, /*skipEviction=*/true));
  ASSERT_TRUE(blocking.loadSectionFile(params));
  ASSERT_GT(blocking.pageCount, 0);

  const std::string slicedDir = freshDir("build_sliced");
  const std::string slicedBook = makeBook(slicedDir, 100, 64 * 1024);
  auto slicedEpub = openBook(slicedBook, slicedDir);
  Section sliced(slicedEpub, kChapterSpine, renderer);
  int slices = 0;
  Section::BuildStep step = Section::BuildStep::More;
  while (step != Section::BuildStep::Done && step != Section::BuildStep::Failed && slices < 200000) {
    step = sliced.stepSectionBuild(params, /*budgetMs=*/1);
    ++slices;
  }
  ASSERT_EQ(step, Section::BuildStep::Done);
  ASSERT_TRUE(sliced.loadSectionFile(params));

  EXPECT_EQ(storeBytes(*slicedEpub), storeBytes(*blockingEpub));
  EXPECT_EQ(sliced.pageCount, blocking.pageCount);
  EXPECT_TRUE(FootnotePreviews::spineResolved(slicedEpub->getCachePath(), kChapterSpine));
}

// The generated books are STORED entries, so the paths above never exercise the inflate ring the
// ZIP path allocates. The corpus book is deflated: same equivalence, real archive.
TEST(FootnoteResolveSlice, SlicedResolveMatchesOneShotOnADeflatedArchive) {
  const std::string corpus = std::string(CORPUS_DIR) + "/test_inline_footnotes.epub";

  const std::string oneShotDir = freshDir("deflated_oneshot");
  auto oneShotEpub = openBook(corpus, oneShotDir);
  ASSERT_TRUE(FootnotePreviews::resolveSpine(*oneShotEpub, kChapterSpine));
  const std::string expected = storeBytes(*oneShotEpub);
  ASSERT_FALSE(expected.empty());

  const std::string slicedDir = freshDir("deflated_sliced");
  auto slicedEpub = openBook(corpus, slicedDir);
  ASSERT_GT(resolveStepByStep(*slicedEpub, kChapterSpine), 0);
  EXPECT_EQ(storeBytes(*slicedEpub), expected);
}

// The real-book shape, scaled from the device trace: three large chapters whose callers all point
// into one shared notes document. Device numbers for the first of them (275722 bytes, 14 notes,
// notes document not yet banked): 1125 ms of resolve, worst slice 90 ms — where before the whole
// 1125 ms was a single loop-task slice. Here we pin the part a host test can pin: that spreading
// it over slices changes nothing about the result, at every chapter of the book.
TEST(FootnoteResolveSlice, SharedNotesDocumentShapeSlicesIdentically) {
  const std::vector<Chapter> chapters = {{256 * 1024, 14}, {224 * 1024, 11}, {160 * 1024, 3}};
  constexpr int kFirstChapterSpine = 2;  // two front-matter documents precede them

  const std::string oneShotDir = freshDir("shared_oneshot");
  auto oneShotEpub = openBook(makeSharedNotesBook(oneShotDir, chapters), oneShotDir);
  for (size_t c = 0; c < chapters.size(); ++c) {
    ASSERT_TRUE(FootnotePreviews::resolveSpine(*oneShotEpub, kFirstChapterSpine + static_cast<int>(c))) << c;
  }
  const std::vector<std::string> expected = storeTexts(*oneShotEpub);
  ASSERT_EQ(expected.size(), 14u + 11u + 3u);

  const std::string slicedDir = freshDir("shared_sliced");
  auto slicedEpub = openBook(makeSharedNotesBook(slicedDir, chapters), slicedDir);
  for (size_t c = 0; c < chapters.size(); ++c) {
    const int spine = kFirstChapterSpine + static_cast<int>(c);
    const int steps = resolveStepByStep(*slicedEpub, spine);
    ASSERT_GT(steps, 0) << "chapter " << c;

    // Boundedness, per chapter: the chapter's own document is walked a chunk at a time in pass A.
    // (Only the first chapter also streams the notes document; the rest find it banked, and the
    // step floor below stays true either way.)
    size_t chapterBytes = 0;
    ASSERT_TRUE(slicedEpub->getSpineItemInflatedSize(spine, &chapterBytes));
    EXPECT_GE(static_cast<size_t>(steps), chapterBytes / kStreamChunkBytes)
        << "chapter " << c << ": " << steps << " steps for " << chapterBytes << " bytes";
  }
  EXPECT_EQ(storeTexts(*slicedEpub), expected);
}

// Front matter with no callers at all still has to end up marked resolved, in the same book as
// chapters that do have them. A book whose front matter looked permanently unresolved would send
// Background-B back to refusing spines — the shape of issue #211, one level down.
TEST(FootnoteResolveSlice, SpinesWithoutNotesAreMarkedInABookThatHasThem) {
  const std::string dir = freshDir("shared_frontmatter");
  auto epub = openBook(makeSharedNotesBook(dir, {{4 * 1024, 5}}), dir);

  for (int spine = 0; spine < 2; ++spine) {
    EXPECT_FALSE(FootnotePreviews::spineResolved(epub->getCachePath(), spine)) << spine;
    ASSERT_GT(resolveStepByStep(*epub, spine), 0) << spine;
    EXPECT_TRUE(FootnotePreviews::spineResolved(epub->getCachePath(), spine)) << spine;
  }
  EXPECT_TRUE(storeTexts(*epub).empty()) << "front matter has no notes to store";

  ASSERT_GT(resolveStepByStep(*epub, 2), 0);
  EXPECT_EQ(storeTexts(*epub).size(), 5u);
}

// Every note body in this shape opens with a back-link carrying the same marker the caller used.
// It is chrome: a preview that starts with a stray asterisk is the bug this pins. Also covers the
// short-marker heuristic accepting a bare `*` inside <sup>, which is what the real book uses and
// what the earlier generated fixtures (digit markers) do not exercise.
TEST(FootnoteResolveSlice, PreviewTextExcludesTheBackLinkChrome) {
  const std::string dir = freshDir("shared_chrome");
  auto epub = openBook(makeSharedNotesBook(dir, {{2 * 1024, 3}}), dir);
  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, 2));

  const std::vector<std::string> texts = storeTexts(*epub);
  ASSERT_EQ(texts.size(), 3u);
  for (const std::string& t : texts) {
    EXPECT_EQ(t.find('*'), std::string::npos) << "back-link marker leaked into the preview: " << t;
    EXPECT_EQ(t.rfind("Note ", 0), 0u) << "preview should start at the note prose: " << t;
    EXPECT_NE(t.find("nothing else."), std::string::npos) << "preview truncated: " << t;
  }
}

// One shared notes document, three chapters pointing into it: it must be streamed from the archive
// once and banked, so the chapters after the first cost an SD read instead of an inflate. On the
// device that is the difference between the first chapter's 1125 ms resolve and the later ones.
TEST(FootnoteResolveSlice, SharedNotesDocumentIsBankedOnFirstUse) {
  const std::string dir = freshDir("shared_banking");
  const std::vector<Chapter> chapters = {{8 * 1024, 4}, {8 * 1024, 4}};
  auto epub = openBook(makeSharedNotesBook(dir, chapters), dir);
  const int notesSpine = 2 + static_cast<int>(chapters.size());  // front matter + chapters, then notes

  const std::string bankPath = Section::sectionHtmlCachePath(epub->getCachePath(), notesSpine);
  EXPECT_FALSE(fs::exists(bankPath));

  ASSERT_TRUE(FootnotePreviews::resolveSpine(*epub, 2));
  ASSERT_TRUE(fs::exists(bankPath)) << "the note document should have been banked on the way past";

  size_t notesSize = 0;
  ASSERT_TRUE(epub->getSpineItemInflatedSize(notesSpine, &notesSize));
  EXPECT_EQ(fs::file_size(bankPath), notesSize) << "a short bank fails the builder's staleness test";

  // Second chapter into the same document: it is served from the bank, so the pass completes with
  // the archive gone.
  const std::string epubPath = epub->getPath();
  fs::remove(epubPath);
  EXPECT_FALSE(FootnotePreviews::spineResolved(epub->getCachePath(), 3));
  // Pass A still needs chapter 3's own document, which nothing has banked — so this legitimately
  // fails without the archive. What matters is WHERE it fails: the store must survive it intact.
  const std::vector<std::string> before = storeTexts(*epub);
  FootnotePreviews::resolveSpine(*epub, 3);
  EXPECT_EQ(storeTexts(*epub), before) << "a pass that could not read its spine must change nothing";
  EXPECT_FALSE(FootnotePreviews::spineResolved(epub->getCachePath(), 3));
}
