// What a table holds stays out of the page's flow while the table's rows wait in the fragment packer. A packed row is
// laid out for the page in progress -- on an arena build in that page's block of the lent region, which emitting the
// page rewinds -- and is placed only when its fragment is flushed. Two things inside a table acted on the page while
// rows waited:
//  - a TOC anchor in a later row (on its <tr> or <td>, or on a block in a cell) took its page break at the next block
//    in the cell: the page went out without the rows, its block was rewound under them, the next row was laid out over
//    their bytes, and the fragment was written, and its lines destroyed, from overwritten bytes (a TextBlock's arena
//    flags read back from them freed an arena address on the heap);
//  - an <hr> in a cell was drawn in the page's flow, which moved the page down under the packed rows: their fragment
//    no longer fitted where it had been packed, and an arena build stopped there, the chapter cut at the table.
// A nested table's cell is laid out as the page's paragraphs instead: in parts, and a paragraph per block in it.
// Each book is built on the heap and in a lent region of the X3's framebuffer size, at several page heights so that
// page ends fall across the table. This binary fills a rewound block (BUILD_ARENA_TRACE), so a line used or destroyed
// after its block was given back aborts it. Both builds must complete, keep every row's words, and lay out alike.
#include <Arduino.h>
#include <BuildArena.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "../book_metadata_cache/AllocationRefusal.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr size_t kLentFramebufferBytes = 52272;  // the X3's secondary framebuffer
constexpr int kViewportHeights[] = {300, 360, 420, 480, 540, 600, 700, 800};

struct PageText {
  std::vector<std::string> words;       // the page's lines' and table cells' words, in element order
  std::vector<std::string> lineStarts;  // the first word of each of the page's own lines (a table's are not)
  int lineMarkers = 0;                  // list markers on the page's own lines
  int rules = 0;                        // its PageHR elements
  bool operator==(const PageText& other) const {
    return words == other.words && lineStarts == other.lineStarts && lineMarkers == other.lineMarkers &&
           rules == other.rules;
  }
};

// A list item's marker as the parser writes it: a bullet, or the item's number and a full stop.
bool isListMarker(const std::string& word) {
  return word == "\xe2\x80\xa2" ||
         (word.size() >= 2 && word.back() == '.' && word.front() >= '0' && word.front() <= '9');
}

void collectWords(const TextBlock* block, std::vector<std::string>& out) {
  if (!block || !block->valid()) return;
  for (uint16_t w = 0; w < block->wordCount(); ++w) out.emplace_back(block->wordText(w));
}

PageText textOf(const Page& page) {
  PageText text;
  for (const auto& el : page.elements) {
    switch (el->getTag()) {
      case TAG_PageLine: {
        const size_t before = text.words.size();
        collectWords(static_cast<const PageLine&>(*el).getBlock(), text.words);
        if (text.words.size() > before) text.lineStarts.push_back(text.words[before]);
        for (size_t w = before; w < text.words.size(); ++w) text.lineMarkers += isListMarker(text.words[w]) ? 1 : 0;
        break;
      }
      case TAG_PageTable:
        for (const auto& row : static_cast<const PageTableFragment&>(*el).getRows()) {
          for (const auto& cell : row.cells) {
            for (const auto& line : cell.lines) collectWords(line.get(), text.words);
          }
        }
        break;
      case TAG_PageHR:
        ++text.rules;
        break;
      default:
        break;
    }
  }
  return text;
}

struct Built {
  bool complete = false;  // built, loaded back, and not truncated
  std::vector<PageText> pages;
  std::map<std::string, int> anchorPage;  // the anchor map's page for each id asked about; -1: not in the map

  // The page `word` is on; -1 when it is on none. Fails the test when it is on more than one, or twice on one.
  int pageOf(const std::string& word) const {
    int found = -1;
    int seen = 0;
    for (size_t pg = 0; pg < pages.size(); ++pg) {
      for (const auto& w : pages[pg].words) {
        if (w != word) continue;
        ++seen;
        found = static_cast<int>(pg);
      }
    }
    EXPECT_LE(seen, 1) << word << " is laid out " << seen << " times";
    return found;
  }

  int rules() const {
    int n = 0;
    for (const auto& page : pages) n += page.rules;
    return n;
  }

  // How often `word` is laid out, in lines and cells.
  int count(const std::string& word) const {
    int n = 0;
    for (const auto& page : pages) n += static_cast<int>(std::count(page.words.begin(), page.words.end(), word));
    return n;
  }

  int lineMarkers() const {
    int n = 0;
    for (const auto& page : pages) n += page.lineMarkers;
    return n;
  }

  // Whether `word` is the first word of one of the pages' own lines.
  bool startsALine(const std::string& word) const {
    for (const auto& page : pages) {
      if (std::find(page.lineStarts.begin(), page.lineStarts.end(), word) != page.lineStarts.end()) return true;
    }
    return false;
  }
};

fs::path freshWorkDir(const std::string& tag) {
  const fs::path dir =
      fs::temp_directory_path() / ("grid_table_page_flow_" + std::to_string(static_cast<long>(getpid()))) / tag;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

// An NCX naming the chapter and, inside it, each of `ids`: those are the chapter's TOC anchors.
std::string ncxFor(const std::vector<std::string>& ids) {
  std::string points =
      "<navPoint id=\"n0\"><navLabel><text>Chapter</text></navLabel><content src=\"chapter.xhtml\"/></navPoint>\n";
  for (size_t i = 0; i < ids.size(); ++i) {
    points += "<navPoint id=\"n" + std::to_string(i + 1) + "\"><navLabel><text>" + ids[i] +
              "</text></navLabel><content src=\"chapter.xhtml#" + ids[i] + "\"/></navPoint>\n";
  }
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" "
         "version=\"2005-1\">\n<navMap>\n" +
         points + "</navMap>\n</ncx>\n";
}

// The marker word a row's cell starts with: R07a for row 7's first cell, R07b for its second.
std::string mark(const int row, const char cell) {
  char buf[8];
  snprintf(buf, sizeof(buf), "R%02d%c", row, cell);
  return buf;
}

// Builds the book's one chapter at `viewportHeight`, on the heap or in a lent region, into a cache of its own, and
// reads every page back. A page that does not load is recorded as the single word "<unreadable>".
Built buildAndRead(const std::string& bookPath, const fs::path& cacheDir, const int viewportHeight, const bool inArena,
                   const std::vector<std::string>& anchors) {
  Built out;
  fs::create_directories(cacheDir);
  auto epub = std::make_shared<Epub>(bookPath, cacheDir.string());
  EXPECT_TRUE(epub->load(true));
  epub->setupCacheDir();

  GfxRenderer renderer;
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = TextScale();
  p.viewportWidth = 480;
  p.viewportHeight = static_cast<uint16_t>(viewportHeight);
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;

  Section section(epub, 0, renderer);
  std::vector<uint8_t> lent(kLentFramebufferBytes);
  BuildArena arena(lent.data(), lent.size());
  if (inArena) section.setExternalBuildScratch(&arena);
  const bool built = section.createSectionFile(p, {}, /*skipEviction=*/true);
  section.setExternalBuildScratch(nullptr);
  EXPECT_TRUE(built);
  if (!built || !section.loadSectionFile(p)) return out;
  out.complete = !section.isTruncatedCache();
  for (int pg = 0; pg < section.pageCount; ++pg) {
    section.currentPage = pg;
    const auto page = section.loadPageFromSectionFile();
    out.pages.push_back(page ? textOf(*page) : PageText{{"<unreadable>"}});
  }
  for (const auto& id : anchors) {
    const auto page = section.getPageForAnchor(id);
    out.anchorPage[id] = page ? static_cast<int>(*page) : -1;
  }
  return out;
}

// The book built both ways at one page height: both complete, page for page alike, and every marker laid out once.
struct HeapAndArena {
  Built heap;
  Built arena;
};

HeapAndArena buildBothWays(const fs::path& work, const std::string& bookPath, const int viewportHeight,
                           const std::vector<std::string>& anchors, const std::vector<std::string>& markers) {
  const std::string tag = std::to_string(viewportHeight);
  HeapAndArena both;
  both.heap = buildAndRead(bookPath, work / ("heap_" + tag), viewportHeight, /*inArena=*/false, anchors);
  both.arena = buildAndRead(bookPath, work / ("arena_" + tag), viewportHeight, /*inArena=*/true, anchors);
  EXPECT_TRUE(both.heap.complete) << "heap build truncated";
  EXPECT_TRUE(both.arena.complete) << "arena build truncated: the chapter is cut at the table";
  EXPECT_GT(both.heap.pages.size(), 1u) << "the fixture must span pages";
  EXPECT_EQ(both.heap.pages.size(), both.arena.pages.size());
  for (size_t pg = 0; pg < both.heap.pages.size() && pg < both.arena.pages.size(); ++pg) {
    EXPECT_EQ(both.heap.pages[pg], both.arena.pages[pg]) << "page " << pg;
  }
  for (const auto& m : markers) {
    EXPECT_GE(both.heap.pageOf(m), 0) << m << " missing from the heap build";
    EXPECT_GE(both.arena.pageOf(m), 0) << m << " missing from the arena build";
  }
  return both;
}

}  // namespace

// TOC anchors in later rows of a table that spans pages, reaching the parser each way a row can carry one: the <tr>'s
// id with a <p> in a cell, a <td>'s id with a <p> in it, a <p>'s own id, a heading's id, and the <tr>'s id with a <br>
// in a cell. None breaks the page under the rows packed above it; each names the page its row is laid out on.
TEST(GridTablePageFlow, ATocAnchorInALaterRowNamesItsRowsPageAndLeavesThePackedRowsWhole) {
  const fs::path work = freshWorkDir("toc_later_rows");
  const std::vector<std::string> anchors = {"toc-tr", "toc-td", "toc-p", "toc-h", "toc-br"};
  // The row each anchor is in.
  const std::map<std::string, int> anchorRow = {
      {"toc-tr", 2}, {"toc-td", 9}, {"toc-p", 17}, {"toc-h", 26}, {"toc-br", 33}};

  std::string body = "<p>INTRO comes before the table, with words enough to take a line or two of its page.</p><table>";
  std::vector<std::string> markers = {"INTRO", "OUTRO"};
  for (int r = 0; r < 40; ++r) {
    const std::string a = mark(r, 'a');
    const std::string b = mark(r, 'b');
    markers.push_back(a);
    markers.push_back(b);
    const std::string words = " with words enough to wrap in a cell";
    switch (r) {
      case 2:
        body += "<tr id=\"toc-tr\"><td><p>" + a + words + "</p></td><td>" + b + words + "</td></tr>";
        break;
      case 9:
        body += "<tr><td id=\"toc-td\"><p>" + a + words + "</p></td><td>" + b + words + "</td></tr>";
        break;
      case 17:
        body += "<tr><td><p id=\"toc-p\">" + a + words + "</p></td><td>" + b + words + "</td></tr>";
        break;
      case 26:
        body += "<tr><td><h3 id=\"toc-h\">" + a + "</h3>" + words + "</td><td>" + b + words + "</td></tr>";
        break;
      case 33:
        body += "<tr id=\"toc-br\"><td>" + a + " first<br/>R33c second</td><td>" + b + words + "</td></tr>";
        markers.push_back("R33c");
        break;
      default:
        body += "<tr><td>" + a + words + "</td><td>" + b + words + "</td></tr>";
        break;
    }
  }
  body += "</table><p>OUTRO follows the table.</p>";

  std::string bookPath;
  {
    const auto epub = syntheticBook(work, 0, body, {{"toc.ncx", "application/x-dtbncx+xml", ncxFor(anchors)}});
    ASSERT_TRUE(epub);
    bookPath = epub->getPath();
  }

  for (const int viewportHeight : kViewportHeights) {
    SCOPED_TRACE("viewport " + std::to_string(viewportHeight));
    const HeapAndArena both = buildBothWays(work, bookPath, viewportHeight, anchors, markers);
    // Nothing breaks the page before the table: the intro and its first row share one.
    EXPECT_EQ(both.heap.pageOf("INTRO"), both.heap.pageOf(mark(0, 'a')));
    for (const Built* built : {&both.heap, &both.arena}) {
      for (const auto& [id, row] : anchorRow) {
        EXPECT_EQ(built->anchorPage.at(id), built->pageOf(mark(row, 'a')))
            << (built == &both.heap ? "heap" : "arena") << ": " << id << " names another page than its row";
      }
    }
  }
  std::error_code ec;  // Windows keeps a just-closed file busy for a moment; a leftover temp dir is harmless
  fs::remove_all(work, ec);
}

// Before the table's first row nothing is packed: a TOC anchor there still opens its chapter on a fresh page, the
// table starting it.
TEST(GridTablePageFlow, ATocAnchorOnTheFirstRowStillStartsTheTableOnAFreshPage) {
  const fs::path work = freshWorkDir("toc_first_row");
  const std::vector<std::string> anchors = {"toc-first"};
  const std::string body =
      "<p>INTRO comes before the table.</p><table>"
      "<tr id=\"toc-first\"><td><p>F00a opens the chapter</p></td><td>F00b beside it</td></tr>"
      "<tr><td>F01a the second row</td><td>F01b beside it</td></tr>"
      "</table><p>OUTRO follows the table.</p>";
  std::string bookPath;
  {
    const auto epub = syntheticBook(work, 0, body, {{"toc.ncx", "application/x-dtbncx+xml", ncxFor(anchors)}});
    ASSERT_TRUE(epub);
    bookPath = epub->getPath();
  }
  const HeapAndArena both =
      buildBothWays(work, bookPath, 800, anchors, {"INTRO", "F00a", "F00b", "F01a", "F01b", "OUTRO"});
  for (const Built* built : {&both.heap, &both.arena}) {
    EXPECT_EQ(built->pageOf("INTRO") + 1, built->pageOf("F00a"));
    EXPECT_EQ(built->anchorPage.at("toc-first"), built->pageOf("F00a"));
    EXPECT_EQ(built->pageOf("F00a"), built->pageOf("F01a"));
  }
  std::error_code ec;
  fs::remove_all(work, ec);
}

// An <hr> in a cell of every row of a table that spans pages: it ends the word before it and draws nothing, so the
// page's flow stays where the packed rows were laid out for it. The rules before and after the table are drawn.
TEST(GridTablePageFlow, AnHrInACellEndsItsWordAndLeavesThePageFlowAlone) {
  const fs::path work = freshWorkDir("hr_in_cell");
  std::string body = "<p>INTRO comes before the table.</p><hr/><table>";
  std::vector<std::string> markers = {"INTRO", "OUTRO"};
  for (int r = 0; r < 40; ++r) {
    const std::string a = mark(r, 'a');
    const std::string b = mark(r, 'b');
    const std::string c = mark(r, 'c');
    markers.insert(markers.end(), {a, b, c});
    body += "<tr><td>" + a + " above the rule<hr/>" + c + " below it</td><td>" + b + " beside it</td></tr>";
  }
  body += "</table><hr/><p>OUTRO follows the table.</p>";
  std::string bookPath;
  {
    const auto epub = syntheticBook(work, 0, body);
    ASSERT_TRUE(epub);
    bookPath = epub->getPath();
  }
  for (const int viewportHeight : kViewportHeights) {
    SCOPED_TRACE("viewport " + std::to_string(viewportHeight));
    const HeapAndArena both = buildBothWays(work, bookPath, viewportHeight, {}, markers);
    EXPECT_EQ(both.heap.rules(), 2) << "only the rules outside the table are drawn";
    EXPECT_EQ(both.arena.rules(), 2);
    // A row's cells stay together: the word after the rule is in its row's fragment.
    for (int r = 0; r < 40; ++r) EXPECT_EQ(both.heap.pageOf(mark(r, 'a')), both.heap.pageOf(mark(r, 'c'))) << r;
  }
  std::error_code ec;
  fs::remove_all(work, ec);
}

namespace {

// A layout table that nests the chapter: an outer table whose one cell holds an inner table whose one cell holds
// `inner`. A nested table's cells are never a grid: each is laid out as the page's paragraphs.
std::string nestedChapter(const std::string& inner) {
  return "<p>INTRO comes before the table.</p><table><tr><td><table><tr><td>" + inner +
         "</td></tr></table></td></tr></table><p>OUTRO follows the table.</p>";
}

// `count` words of running text, a marker word (mark(i, 'w')) opening every tenth.
std::string runningText(const int count) {
  std::string out;
  for (int i = 0; i < count; ++i) out += (i % 10 == 0 ? mark(i / 10, 'w') : std::string("word")) + " ";
  return out;
}

// The heap a heap build of the book's chapter peaks at over what was live when it started.
size_t heapBuildPeak(const std::string& bookPath, const fs::path& cacheDir) {
  fs::create_directories(cacheDir);
  auto epub = std::make_shared<Epub>(bookPath, cacheDir.string());
  EXPECT_TRUE(epub->load(true));
  epub->setupCacheDir();
  GfxRenderer renderer;
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = TextScale();
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  Section section(epub, 0, renderer);
  resetHeapPeak();
  const size_t before = heapLiveBytes();
  EXPECT_TRUE(section.createSectionFile(p, {}, /*skipEviction=*/true));
  const size_t peak = heapPeakBytes();
  return peak > before ? peak - before : 0;
}

}  // namespace

// A nested cell that holds a chapter's running text goes out in parts as a long paragraph does (the main block's split
// threshold, its last line kept for the words to come): it never holds the chapter's words at once. It used to build
// up in one cell text until </td>, until the C3's heap refused it the next word growth and the build stopped there,
// everything after the table lost.
TEST(GridTablePageFlow, ANestedCellHoldingAChapterIsLaidOutInParts) {
  const fs::path work = freshWorkDir("nested_running_text");
  constexpr int kShortWords = 600;
  constexpr int kLongWords = 6000;
  std::string shortBook;
  std::string longBook;
  {
    const auto epub = syntheticBook(work, 0, nestedChapter(runningText(kShortWords)));
    ASSERT_TRUE(epub);
    shortBook = epub->getPath();
  }
  {
    const auto epub = syntheticBook(work, 1, nestedChapter(runningText(kLongWords)));
    ASSERT_TRUE(epub);
    longBook = epub->getPath();
  }
  const size_t shortPeak = heapBuildPeak(shortBook, work / "peak_short");
  const size_t longPeak = heapBuildPeak(longBook, work / "peak_long");
  // Ten times the words: in one cell text, ~40 B a token on the host before the layout's own tables.
  EXPECT_LT(longPeak, shortPeak + 32 * 1024) << "short " << shortPeak << " B, long " << longPeak << " B";

  std::vector<std::string> markers = {"INTRO", "OUTRO"};
  for (int i = 0; i < kLongWords / 10; ++i) markers.push_back(mark(i, 'w'));
  for (const int viewportHeight : {360, 800}) {
    SCOPED_TRACE("viewport " + std::to_string(viewportHeight));
    const HeapAndArena both = buildBothWays(work, longBook, viewportHeight, {}, markers);
    for (const Built* built : {&both.heap, &both.arena}) {
      EXPECT_EQ(built->count("word"), kLongWords - kLongWords / 10) << "every word, once";
    }
  }
  std::error_code ec;
  fs::remove_all(work, ec);
}

// The paragraphs of a nested cell stay paragraphs: each starts a line of its own, as it would outside the table. They
// were one paragraph, run together, the cell's text having no block break in it.
TEST(GridTablePageFlow, ANestedCellsParagraphsEachStartALine) {
  const fs::path work = freshWorkDir("nested_paragraphs");
  constexpr int kParagraphs = 30;
  std::string inner;
  std::vector<std::string> markers = {"INTRO", "OUTRO"};
  for (int i = 0; i < kParagraphs; ++i) {
    const std::string m = mark(i, 'p');
    markers.push_back(m);
    // Short ones end mid-line, where the next one's first word ran on.
    inner += (i % 2 == 0 ? "<p>" + m + " is short.</p>"
                         : "<div><p>" + m +
                               " is a paragraph with words enough to take a few lines of the page.</p>"
                               "</div>");
  }
  std::string bookPath;
  {
    const auto epub = syntheticBook(work, 0, nestedChapter(inner));
    ASSERT_TRUE(epub);
    bookPath = epub->getPath();
  }
  for (const int viewportHeight : kViewportHeights) {
    SCOPED_TRACE("viewport " + std::to_string(viewportHeight));
    const HeapAndArena both = buildBothWays(work, bookPath, viewportHeight, {}, markers);
    for (const Built* built : {&both.heap, &both.arena}) {
      for (int i = 0; i < kParagraphs; ++i) EXPECT_TRUE(built->startsALine(mark(i, 'p'))) << mark(i, 'p');
    }
  }
  std::error_code ec;
  fs::remove_all(work, ec);
}
