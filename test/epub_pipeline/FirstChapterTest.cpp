// What the first page of a big book costs after its first open, step by step.
//
// X3, 2026-10-20 (PR #528 validated): King's Avatar's popup came down to ~5.2 s, and the first
// page followed ~3.6 s later -- its title page, a cover picture. This walks the reader's steps for
// that page on a book of the same shape (1732 chapters, the package documents behind every chapter
// and image, a 1200x1500 cover), as the first open leaves it: the text reference, the section
// build in the lent framebuffer, the page load, and the image warm.
//
// Host, before the spine items' ZIP stats moved into book.bin: the title page's section build made
// 3547 card calls and 2 walks at 433 chapters, 12725 calls and 2 walks (3337 entries read) at 1732
// -- a walk of the whole central directory and a book.bin read per spine item, to fill a RAM table
// of every spine item's stat before the first chapter opened. The page load and the image warm
// (the progressive cover decoded in bands, re-reading its file) do not grow with the book.
#include <BuildArena.h>
#include <HalStorage.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "BigSpineBook.h"
#include "CardCost.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;
using card_cost::Counters;

namespace {

constexpr size_t kX3LentFramebuffer = 52272;
constexpr size_t kX3FreeHeap = 43576;
constexpr size_t kX3Block = 20 * 1024;

// The corpus's large cover-sized picture: 1200x1500, 106 KB, progressive greyscale JPEG.
std::string coverJpeg() {
  Epub corpus(CORPUS_DIR "/test_jpeg_images.epub", (fs::temp_directory_path() / "first_chapter_corpus").string());
  size_t size = 0;
  uint8_t* bytes = corpus.readItemContentsToBytes("OEBPS/images/scaling_test.jpg", &size, false);
  std::string out(reinterpret_cast<const char*>(bytes), size);
  free(bytes);
  return out;
}

Section::BuildParams x3Params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.viewportWidth = 400;
  p.viewportHeight = 430;
  p.fontSizeNormalization = false;
  p.embeddedStyle = true;
  return p;
}

struct Step {
  Counters cost;
  double hostMs = 0;
};

template <typename F>
Step measure(F&& f) {
  const Counters before = card_cost::now();
  const auto start = std::chrono::steady_clock::now();
  f();
  Step s;
  s.hostMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  s.cost = card_cost::now() - before;
  return s;
}

struct FirstPage {
  Step textReference;
  Step build;
  Step load;
  Step warm;
  int pages = 0;
  bool built = false;
};

FirstPage firstPage(const fs::path& work, const int chapters) {
  big_spine_book::Options options;
  options.manifestInNameOrder = true;
  options.documentsLast = true;
  options.stylesheets = 2;
  options.titlePage = true;
  options.coverBytes = coverJpeg();
  const big_spine_book::Book book = big_spine_book::write(
      (work / ("book" + std::to_string(chapters) + ".epub")).string(), chapters, chapters * 12 / 13, options);
  auto lent = std::make_unique<uint8_t[]>(kX3LentFramebuffer);
  BuildArena region(lent.get(), kX3LentFramebuffer);
  fs::remove_all(work / "cache");

  FirstPage out;
  const HostFreeHeap freeHeap(kX3FreeHeap);
  const HostLargestFreeBlock largest(kX3Block);
  const AllocationRefusal refusal(kX3Block + 1);
  auto epub = std::make_shared<Epub>(book.path, (work / "cache").string());
  if (!epub->load(true, false, &region)) return out;
  epub->setupCacheDir();
  card_cost::reset();

  int spine = 0;
  out.textReference = measure([&] { spine = epub->getSpineIndexForTextReference(); });
  GfxRenderer renderer;
  const Section::BuildParams p = x3Params();
  Section section(epub, spine, renderer);
  out.build = measure([&] {
    region.reset();
    section.setExternalBuildScratch(&region);
    out.built = section.createSectionFile(p, {}, /*skipEviction=*/true);
  });
  std::unique_ptr<Page> page;
  out.load = measure([&] {
    if (!section.loadSectionFile(p)) return;
    section.currentPage = 0;
    page = section.loadPageFromSectionFile();
  });
  out.pages = section.pageCount;
  out.warm = measure([&] {
    if (page) page->warmImageCaches(renderer, 0, 0, /*forceLoadLargeImages=*/false, true, /*alsoWarmGrayscale=*/true);
  });
  return out;
}

void report(const int chapters, const FirstPage& f) {
  const std::string label = "spines=" + std::to_string(chapters);
  card_cost::print(label.c_str(), "text reference", f.textReference.cost, f.textReference.hostMs);
  card_cost::print(label.c_str(), "section build", f.build.cost, f.build.hostMs);
  card_cost::print(label.c_str(), "page load", f.load.cost, f.load.hostMs);
  card_cost::print(label.c_str(), "image warm", f.warm.cost, f.warm.hostMs);
}

TEST(FirstChapter, KingsAvatarTitlePageProfile) {
  const fs::path work = fs::temp_directory_path() / ("first_chapter_" + std::to_string(getpid()));
  fs::remove_all(work);
  fs::create_directories(work);
  const FirstPage small = firstPage(work, 433);
  const FirstPage large = firstPage(work, 1732);
  ASSERT_TRUE(small.built);
  ASSERT_TRUE(large.built);
  report(433, small);
  report(1732, large);
  // The title page opens from its book.bin record: one lookup, for its picture, and about as many
  // card calls at 1732 chapters as at 433.
  EXPECT_LE(large.build.cost.walks, 1u);
  EXPECT_LE(large.build.cost.entries, 16u);
  EXPECT_LE(large.build.cost.calls, small.build.cost.calls + 200);
  fs::remove_all(work);
}

// The OPF guide's text reference names where reading starts, often with a fragment
// ("Text/chapter2.xhtml#start"). With the fragment the href matched no spine item: the book opened
// at its first one, after a lookup that read every spine record a field at a time -- 10395 card
// calls for 1732 chapters, ~0.18 s on the X3, made twice by a first open (the font examination and
// the reader). In-book links to the far end of a big book made the same scan.
TEST(FirstChapter, AGuideTextReferenceWithAFragmentFindsItsChapter) {
  const fs::path work = fs::temp_directory_path() / ("first_chapter_guide_" + std::to_string(getpid()));
  fs::remove_all(work);
  fs::create_directories(work);
  big_spine_book::Options options;
  options.titlePage = true;
  options.guideTextReference = "Text/" + big_spine_book::chapterName(2) + "#start";
  const big_spine_book::Book book = big_spine_book::write((work / "book.epub").string(), 1732, 1600, options);
  auto epub = std::make_shared<Epub>(book.path, (work / "cache").string());
  ASSERT_TRUE(epub->load(true, false));

  card_cost::reset();
  const int spine = epub->getSpineIndexForTextReference();
  const Counters first = card_cost::now();
  EXPECT_EQ(spine, 3) << "the title page, then chapters 0, 1 and 2";
  EXPECT_EQ(epub->getSpineIndexForTextReference(), 3);
  const Counters both = card_cost::now();
  std::printf("[ guide    ] text reference: spine %d in %zu card calls, %zu for the second ask\n", spine, first.calls,
              both.calls - first.calls);
  EXPECT_LE(first.calls, 16u);
  EXPECT_EQ(both.calls, first.calls) << "the answer is kept";

  // A link to the last chapter reads the spine records through a window.
  card_cost::reset();
  EXPECT_EQ(epub->resolveHrefToSpineIndex("OEBPS/Text/" + big_spine_book::chapterName(1731) + "#n"), 1732);
  const Counters link = card_cost::now();
  std::printf("[ guide    ] link to the last chapter: %zu card calls\n", link.calls);
  EXPECT_LE(link.calls, 128u);
  fs::remove_all(work);
}

// A chapter's ZIP stat costs the open book no memory: it is read from the chapter's book.bin record
// when the chapter opens. A table of every spine item's stat used to be held for as long as the
// book stayed open, 16 B a spine item (27.7 KB for King's Avatar).
TEST(FirstChapter, AChaptersZipStatHoldsNoMemoryForTheBook) {
  const fs::path work = fs::temp_directory_path() / ("first_chapter_stat_" + std::to_string(getpid()));
  fs::remove_all(work);
  fs::create_directories(work);
  const big_spine_book::Book book = big_spine_book::write((work / "book.epub").string(), 1732, 1600);
  auto epub = std::make_shared<Epub>(book.path, (work / "cache").string());
  ASSERT_TRUE(epub->load(true, false));

  const size_t liveBefore = heapLiveBytes();
  ZipFile::FileStatSlim first = {};
  ZipFile::FileStatSlim last = {};
  ASSERT_TRUE(epub->getSpineItemStat(0, &first));
  ASSERT_TRUE(epub->getSpineItemStat(1731, &last));
  const size_t held = heapLiveBytes() - liveBefore;
  std::printf("[ stat     ] 1732 spine items: %zu B held after two chapters' stats\n", held);
  EXPECT_LT(held, 1024u);

  ZipFile zip(book.path);
  ZipFile::FileStatSlim expected = {};
  ASSERT_TRUE(zip.loadFileStatSlim(book.spineHrefs.back().c_str(), &expected));
  EXPECT_EQ(last.localHeaderOffset, expected.localHeaderOffset);
  EXPECT_EQ(last.compressedSize, expected.compressedSize);
  EXPECT_EQ(last.uncompressedSize, expected.uncompressedSize);
  EXPECT_EQ(last.method, expected.method);
  EXPECT_NE(first.localHeaderOffset, 0u);
  fs::remove_all(work);
}

}  // namespace
