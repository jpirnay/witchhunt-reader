#pragma once

#include <Epub/Page.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class GfxRenderer;
class LanguageHyphenator;

// The body of a paged text viewer: a string laid out once into pages of a fixed box, then drawn a page at a time.
// Shared by the dictionary's definition viewer and the reader's footnote popup.
//
// Two layouts. HTML goes through the EPUB chapter parser into styled, reader-identical Pages (buildDictionaryHtmlPages)
// when it is small enough and the heap allows. Anything else -- plain text, or HTML too damaged, too large or laid out
// on too tight a heap -- is converted to plain text in place and word-wrapped once into byte spans of that same
// string, so no per-line copies are held.
//
// Moved out of DictionaryDefinitionActivity (ported from crosspoint-reader PR #2583 and the HTML layout of #2836, by
// Uri Tauber) when the footnote popup needed the same body.
class PagedTextBody {
 public:
  // Lays `text` out for a box of `width` x `height` in `fontId`. With `html`, the styled layout is tried first when the
  // text is at most `maxStyledBytes`. False when the plain layout's line table found no memory: there is then nothing
  // to draw, and both buffers have gone back to the heap.
  //
  // Under RenderLock: a TrueType or CFF face is measured only under it.
  bool layOut(GfxRenderer& renderer, std::string text, bool html, size_t maxStyledBytes, int fontId, int width,
              int height, const LanguageHyphenator* hyphenator);

  int pageCount() const { return totalPages; }
  // The height page `page`'s text takes from the box's top, at most the box's: what a sheet sized to its text needs.
  int usedHeight(const GfxRenderer& renderer, int page) const;
  // Page `page` with the box's top-left at (x, y). One pass: a caller that prewarms the font cache runs it twice, the
  // first inside the scan.
  void draw(GfxRenderer& renderer, int page, int x, int y) const;

 private:
  // One wrapped display line: a byte span of `text`. Wrapping keeps lines under the screen width, so uint16_t length is
  // ample.
  struct Line {
    uint32_t start;
    uint16_t len;
  };

  bool reserveLines(size_t capacity);
  bool wrapText(const GfxRenderer& renderer, int width, int height);
  int measureSpan(const GfxRenderer& renderer, const char* span, size_t len) const;

  // The plain layout's text, converted in place; empty once a styled layout owns the words.
  std::string text;
  int fontId = 0;
  // Styled layout: reader-identical Pages. Empty means the plain-text span layout below is active.
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<Line> lines;
  int totalPages = 1;
  int linesPerPage = 1;
  int boxHeight = 0;
};
