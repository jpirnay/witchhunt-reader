// Cover for the section cache's PARAGRAPH ANCHOR — the round trip
// page -> getParagraphIndexForPage -> getPageForParagraphIndex -> page.
//
// Three features anchor a position on a paragraph rather than a page number, because a page
// number stops meaning anything the moment the chapter is laid out again: returning from a
// footnote, holding the reader's place across a font/override change, and KOReader XPath sync.
// All three go through this pair of lookups.
//
// The LUT counts only <p> elements that are DIRECT children of <body>, so a chapter that wraps
// its content in a container div — `<body class="calibre"><div><p>...` , what Calibre emits and
// what a large share of real books therefore look like — records paragraph index 0 on every
// page. 0 is not a paragraph, and it must not be treated as one: `getPageForParagraphIndex`
// searches for the first page whose stored index is >= the one asked for, and 0 satisfies that
// on the very first entry, so an anchor on "paragraph 0" resolves to page 0. Every footnote
// return in such a book landed on the start of the chapter instead of the page it left.
//
// Every other fixture in the corpus is unwrapped, which is why nothing caught this;
// test_wrapped_paragraphs.epub carries both shapes so the guard and the working path are
// pinned side by side.
#include <Arduino.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

constexpr int kWrappedSpine = 0;    // <body><div><p> — no body-child paragraphs at all
constexpr int kUnwrappedSpine = 1;  // <body><p> — the control

const std::string kFixture = std::string(CORPUS_DIR) + "/test_wrapped_paragraphs.epub";

std::string freshCacheDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "paragraph_anchor_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.extraParagraphSpacing = false;
  p.paragraphAlignment = 0;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.hyphenationEnabled = false;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  p.bionicReadingEnabled = false;
  p.inlineFootnotePreviews = false;
  p.imageRendering = 0;
  return p;
}

// Builds `spineIndex` and hands back the loaded Section, so each test states only what it
// asserts. The Epub outlives the Section through the returned pair.
struct BuiltSection {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section;
};

BuiltSection build(const std::string& tag, int spineIndex) {
  BuiltSection out;
  out.epub = std::make_shared<Epub>(kFixture, freshCacheDir(tag));
  EXPECT_TRUE(out.epub->load(true));
  out.epub->setupCacheDir();

  GfxRenderer renderer;
  out.section = std::make_unique<Section>(out.epub, spineIndex, renderer);
  EXPECT_TRUE(out.section->createSectionFile(params(), {}, /*skipEviction=*/true));
  EXPECT_TRUE(out.section->loadSectionFile(params()));
  return out;
}

}  // namespace

// The guard itself, stated on its own: paragraph 0 is not a position, and answering page 0 for
// it is exactly the bug. Callers must be able to tell "no anchor" from "anchored at the start".
TEST(ParagraphAnchor, ParagraphZeroIsNotAnAnchor) {
  const auto built = build("zero_is_not_an_anchor", kUnwrappedSpine);
  EXPECT_FALSE(built.section->getPageForParagraphIndex(0).has_value());
}

// A wrapped chapter has no body-child <p> at all, so no page in it can be anchored on a
// paragraph. The lookup must say so, leaving each caller on its page-number fallback, rather
// than handing back the 0 the LUT stores.
TEST(ParagraphAnchor, WrappedChapterReportsNoParagraphForAnyPage) {
  const auto built = build("wrapped", kWrappedSpine);
  ASSERT_GT(built.section->pageCount, 1) << "fixture must span several pages to be meaningful";

  for (uint16_t page = 0; page < static_cast<uint16_t>(built.section->pageCount); ++page) {
    EXPECT_FALSE(built.section->getParagraphIndexForPage(page).has_value())
        << "page " << page << " of a <body><div><p> chapter has no anchorable paragraph";
  }
}

// The property the three anchoring features actually depend on: whatever the round trip
// resolves to, it is never AHEAD of the page it was taken from, and — the regression — never
// collapses to the start of the chapter for a page in the middle of it.
TEST(ParagraphAnchor, RoundTripNeverCollapsesToTheChapterStart) {
  const auto built = build("round_trip", kUnwrappedSpine);
  ASSERT_GT(built.section->pageCount, 1) << "fixture must span several pages to be meaningful";

  int anchored = 0;
  for (uint16_t page = 1; page < static_cast<uint16_t>(built.section->pageCount); ++page) {
    const auto index = built.section->getParagraphIndexForPage(page);
    if (!index) continue;  // page carries no body-child <p>; the caller falls back to the number
    ++anchored;
    const auto resolved = built.section->getPageForParagraphIndex(*index);
    ASSERT_TRUE(resolved.has_value()) << "page " << page << " anchored on p[" << *index
                                      << "] but it resolved to nothing";
    EXPECT_LE(*resolved, page) << "p[" << *index << "] resolved past the page it was taken from";
    EXPECT_GT(*resolved, 0) << "page " << page << " anchored on p[" << *index << "] fell back to the chapter start";
  }
  EXPECT_GT(anchored, 0) << "control chapter produced no paragraph anchors at all";
}
