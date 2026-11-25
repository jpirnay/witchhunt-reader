// Cover for FootnotePreviews::captureNote, the reader's footnote popup's source: one note, read from its document on
// demand, as a small XHTML fragment.
//
// The preview store's capture is pinned by FootnotePreviewStoreTest; this pins what the markup mode adds on top of it
// -- which tags survive, that the text inside them is escaped, that a note cut at the cap still closes what it opened
// -- and that it finds the same notes (container, Calibre's empty anchor, same-file) and refuses the same non-notes.
#include <Arduino.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <string>

#include "Epub.h"
#include "Epub/FootnotePreviews.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

std::string freshDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / ("footnote_capture_test_" + std::to_string(getpid())) / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

std::string xhtml(const std::string& body) {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
         "<head><title>T</title></head><body>" +
         body + "</body></html>\n";
}

// Spine 0 is the chapter, spine 1 the notes. The chapter holds its own same-file note and the caller anchors the
// notes link back to.
std::string makeBook(const std::string& dir, const std::string& notesBody) {
  const std::string chapter = xhtml(
      "<p>Body text<a id=\"r1\" href=\"notes.xhtml#n1\">1</a> and a same-file note<a href=\"#c1\">2</a>.</p>"
      "<div id=\"c1\"><p>The chapter's own note.</p></div>");
  test_zip::StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");
  zip.add("META-INF/container.xml",
          "<?xml version=\"1.0\"?>\n"
          "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
          "<rootfiles><rootfile full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>"
          "</container>\n");
  zip.add(
      "content.opf",
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">"
      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">capture-test</dc:identifier>"
      "<dc:title>Capture Test</dc:title><dc:language>en</dc:language></metadata>"
      "<manifest><item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"n\" href=\"notes.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
      "<spine><itemref idref=\"c\"/><itemref idref=\"n\"/></spine></package>\n");
  zip.add("chapter.xhtml", chapter);
  zip.add("notes.xhtml", xhtml(notesBody));
  const std::string path = dir + "/book.epub";
  zip.write(path);
  return path;
}

struct Book {
  std::shared_ptr<Epub> epub;
};

Book openBook(const std::string& tag, const std::string& notesBody) {
  const std::string dir = freshDir(tag);
  const std::string path = makeBook(dir, notesBody);
  Book book{std::make_shared<Epub>(path, dir + "/cache")};
  EXPECT_TRUE(book.epub->load(true));
  return book;
}

std::string capture(const Book& book, const char* href, const int fromSpine = 0) {
  std::string markup;
  if (!FootnotePreviews::captureNote(*book.epub, fromSpine, href, markup)) return "<none>";
  return markup;
}

}  // namespace

TEST(FootnoteNoteCapture, KeepsStructureAndEmphasisAndDropsTheRest) {
  const Book book =
      openBook("structure",
               "<aside id=\"n1\"><a href=\"chapter.xhtml#r1\">1</a><h2>Note 1</h2>"
               "<p>First <i>italic</i> words.</p><p>Second <b>bold</b>, <span class=\"x\">a span</span> and "
               "<sup>a sup</sup>.</p></aside>");
  // The back-link and the heading are chrome; the span gives its text without its tag.
  EXPECT_EQ(capture(book, "notes.xhtml#n1"),
            "<p>First <i>italic</i> words.</p><p>Second <b>bold</b>, a span and <sup>a sup</sup>.</p>");
}

TEST(FootnoteNoteCapture, EscapesWhatXmlReserves) {
  const Book book = openBook("escape", "<p id=\"n1\">Fish &amp; chips &lt;b&gt; are &gt; nothing.</p>");
  EXPECT_EQ(capture(book, "notes.xhtml#n1"), "Fish &amp; chips &lt;b&gt; are &gt; nothing.");
}

TEST(FootnoteNoteCapture, KeepsLineBreaks) {
  const Book book = openBook("breaks", "<p id=\"n1\">Line one<br/>line two</p>");
  EXPECT_EQ(capture(book, "notes.xhtml#n1"), "Line one<br/>line two");
}

TEST(FootnoteNoteCapture, FollowsAnEmptyAnchorToTheEndOfItsBlock) {
  // Calibre's filepos shape: the id on an empty <a>, the note's text after it in the same block.
  const Book book =
      openBook("calibre", "<p class=\"n\"><a id=\"filepos9\"></a>Tail <em>note</em> text.</p><p>Not part of it.</p>");
  EXPECT_EQ(capture(book, "notes.xhtml#filepos9"), "Tail <em>note</em> text.");
}

TEST(FootnoteNoteCapture, ReadsASameFileNote) {
  const Book book = openBook("samefile", "<p id=\"n1\">Unused.</p>");
  EXPECT_EQ(capture(book, "#c1"), "<p>The chapter's own note.</p>");
}

TEST(FootnoteNoteCapture, RefusesACallerAndAMissingTarget) {
  const Book book = openBook("refuse", "<p id=\"n1\">A note, <a href=\"chapter.xhtml#r1\">back</a>.</p>");
  // From the notes document, "chapter.xhtml#r1" lands on the caller's own link: not a note.
  EXPECT_EQ(capture(book, "chapter.xhtml#r1", 1), "<none>");
  EXPECT_EQ(capture(book, "notes.xhtml#nowhere"), "<none>");
  EXPECT_EQ(capture(book, "missing.xhtml#n1"), "<none>");
  EXPECT_EQ(capture(book, "notes.xhtml"), "<none>");
}

TEST(FootnoteNoteCapture, ACutNoteStaysWellFormed) {
  std::string longText;
  while (longText.size() < 3 * FootnotePreviews::MAX_NOTE_MARKUP_BYTES) longText += "word ";
  const Book book = openBook("cut", "<div id=\"n1\"><p>Start <i>" + longText + "</i> end.</p></div>");
  const std::string markup = capture(book, "notes.xhtml#n1");
  ASSERT_NE(markup, "<none>");
  EXPECT_LE(markup.size(), FootnotePreviews::MAX_NOTE_MARKUP_BYTES);
  EXPECT_EQ(markup.rfind("<p>Start <i>word ", 0), 0u);
  // The "..." where it stops, then every element it opened, closed.
  const std::string tail = "...</i></p>";
  ASSERT_GE(markup.size(), tail.size());
  EXPECT_EQ(markup.substr(markup.size() - tail.size()), tail);
}

TEST(FootnoteNoteCapture, CutsAtAWholeCharacter) {
  // Two-byte characters all the way: wherever the cap falls, the cut must not leave half of one.
  std::string longText;
  while (longText.size() < 2 * FootnotePreviews::MAX_NOTE_MARKUP_BYTES) longText += "\xC3\xA9";
  const Book book = openBook("utf8", "<p id=\"n1\">" + longText + "</p>");
  const std::string markup = capture(book, "notes.xhtml#n1");
  ASSERT_GE(markup.size(), 3u);
  EXPECT_EQ(markup.substr(markup.size() - 3), "...");
  const std::string text = markup.substr(0, markup.size() - 3);
  EXPECT_EQ(text.size() % 2, 0u);
  EXPECT_EQ(static_cast<uint8_t>(text.back()), 0xA9u);
}
